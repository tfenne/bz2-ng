/*
 * dec_block.c - decoder block finishing and text source
 *
 * Block finishing for the decoder: Huffman decoding tables, the per-stream
 * arrays, the inverse BWT (segmented table build, splitter marking and a
 * multi-chain walk into a pool of chunks, or libbz2's single chain in small
 * mode) and the text source the output stage reads.
 *
 * Notation: L is the BWT (the symbol loop's output), F the sorted column,
 * cftab the exclusive prefix sums of L's symbol counts. The table maps each
 * row to its successor in text order, T(LF(i)) = i; it is a permutation of
 * [0, n) because cftab is the exact prefix sum of the symbols written, so no
 * walk step needs a bounds check.
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */
#include <stdlib.h>
#include <string.h>

#include "dec.h"

/*
 * Builds the decoding table for one code-length set. The result decodes
 * every bit pattern exactly as libbz2's limit/base/perm tables do
 * (huffman.c BZ2_hbCreateDecodeTables with decompress.c GET_MTF_VAL): codes
 * are assigned canonically by length, then symbol; for an oversubscribed set
 * the codes that fall past the end of the code space are unreachable, and for
 * an incomplete set the unused patterns are no code.
 */
void
bzng_build_htable(bzng_htable *t, const u8 *length, s32 alpha_size)
{
	u32 count[BZNG_MAX_CODE + 2];
	memset(count, 0, sizeof count);
	for (s32 i = 0; i < alpha_size; i++)
		count[length[i]]++;
	int maxlen = 0;
	for (int l = 1; l <= BZNG_MAX_CODE; l++)
		if (count[l])
			maxlen = l;
	t->maxlen = (u8)maxlen;
	s32 offs[BZNG_MAX_CODE + 2];
	s32 code = 0, idx = 0;
	for (int l = 1; l <= BZNG_MAX_CODE; l++) {
		t->first_code[l] = code;
		t->first_idx[l] = idx;
		offs[l] = idx;
		code += (s32)count[l];
		idx += (s32)count[l];
		/*
		 * code <= 258 * 2^(l-1), so the shifted value stays below 2^28
		 */
		t->limit_lj[l] = (u32)code << (BZNG_MAX_CODE - l);
		code <<= 1;
	}
	t->limit_lj[BZNG_MAX_CODE + 1] = 0;
	for (s32 i = 0; i < alpha_size; i++)
		t->perm[offs[length[i]]++] = (u16)i;

	/*
	 * Codes of length <= K fill the primary table in canonical order; a
	 * code's slots start where the previous one's end, so stopping when the
	 * table is full is exactly libbz2's truncation of oversubscribed sets.
	 */
	const u32 size = 1u << BZNG_HUFF_K;
	u32 pos = 0;
	const int short_max = maxlen < BZNG_HUFF_K ? maxlen : BZNG_HUFF_K;
	for (int l = 1; l <= short_max && pos < size; l++) {
		const u32 span = 1u << (BZNG_HUFF_K - l);
		const u16 *sp = t->perm + t->first_idx[l];
		for (u32 k = 0; k < count[l] && pos < size; k++) {
			const u16 e = (u16)(sp[k] << 5 | l);
#if GCC_PREREQ(1, 0)
			/*
			 * gcc stores the entries one at a time; four per
			 * 8-byte store (spans are powers of two, so the
			 * stores end at the code's last slot).
			 */
			if (span >= 4) {
				const u64 e4 = 0x0001000100010001ull * e;
				for (u32 j = 0; j < span; j += 4)
					memcpy(&t->prim[pos + j], &e4, 8);
				pos += span;
				continue;
			}
#endif
			for (u32 j = 0; j < span; j++)
				t->prim[pos + j] = e;
			pos += span;
		}
	}
	if (pos < size)
		memset(t->prim + pos, 0, (size - pos) * sizeof t->prim[0]);
}

/*
 * The table build splits L into BUILD_PARTS parts of q = n / BUILD_PARTS rows,
 * the last part also taking the n % BUILD_PARTS rows after them, and counts
 * and scatters them interleaved, each part with counters and cursors of its
 * own: L is mostly runs of one symbol, so one part's updates form a chain of
 * dependent loads and stores, and the parts' chains run in parallel.
 */
#define BUILD_PARTS 8

/* Repeats X(k) for parts 0..7. */
#define PARTS_8(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)

/* Counts row i of part k. */
#define PART_COUNT(k) h[k][L[i + (k) * q]]++;

/* Per-part symbol counts of L[0..n), in one interleaved pass. */
static void
part_hist(const u8 *L, u32 n, u32 h[BUILD_PARTS][256])
{
	/* PARTS_8, PARTS_LO and PARTS_HI name the parts by number. */
	STATIC_ASSERT(BUILD_PARTS == 8);
	memset(h, 0, BUILD_PARTS * 256 * sizeof(u32));
	const u32 q = n / BUILD_PARTS;
	for (u32 i = 0; i < q; i++) {
		PARTS_8(PART_COUNT)
	}
	for (u32 i = BUILD_PARTS * q; i < n; i++)
		h[BUILD_PARTS - 1][L[i]]++;
}

/*
 * Fills cftab (exclusive prefix sums, cftab[256] == n) from the per-part
 * counts and turns the counts into the parts' scatter cursors in place: part
 * k's rows of symbol c go to [h[k][c], h[k + 1][c]), and the last part's end
 * at cftab[c + 1].
 */
static void
cursors(bzng_dstate *s, u32 h[BUILD_PARTS][256])
{
	u32 sum = 0;
	for (int c = 0; c < 256; c++) {
		s->cftab[c] = sum;
		for (int k = 0; k < BUILD_PARTS; k++) {
			const u32 count = h[k][c];
			h[k][c] = sum;
			sum += count;
		}
	}
	s->cftab[256] = sum;
}

/*
 * The row -> F symbol guess table.  It occupies the Huffman tables' bytes,
 * which nothing reads between the end of a block's symbol loop and the next
 * block header, while the guess table is needed only from block finishing
 * to the end of the block's output.
 */
static inline u8 *
guess_table(bzng_dstate *s)
{
	return (u8 *)(void *)s->tab;
}

/* guess_table() for reading. */
static inline const u8 *
guess_table_r(const bzng_dstate *s)
{
	return (const u8 *)(const void *)s->tab;
}

/*
 * Row -> F symbol guess: entry q is the F symbol of row q << shift; the true
 * symbol of a row is that or a later one (F is non-decreasing).
 */
static void
build_fsym(bzng_dstate *s, u32 n)
{
	STATIC_ASSERT(BZNG_FSYM_MAX <= sizeof(s->tab));
	u8 *const fsym = guess_table(s);
	u32 sh = 0;
	while (((n - 1) >> sh) >= BZNG_FSYM_MAX)
		sh++;
	s->fsym_shift = sh;
	const u32 entries = ((n - 1) >> sh) + 1;
	u32 c = 0;
	for (u32 q = 0; q < entries; q++) {
		const u32 r = q << sh;
		while (s->cftab[c + 1] <= r)
			c++;
		fsym[q] = (u8)c;
	}
}

/*
 * The F symbol of a row: the guess table's symbol for its group of rows, or
 * a later one (F is non-decreasing).
 */
static inline u8
row_symbol(const bzng_dstate *s, u32 row)
{
	u32 c = guess_table_r(s)[row >> s->fsym_shift];
	while (row >= s->cftab[c + 1])
		c++;
	return (u8)c;
}

#define PMASK 0x000FFFFFu

/*
 * A 3-byte table entry is little-endian on every target: byte 0 holds bits
 * 0-7, byte 1 bits 8-15 and byte 2 bits 16-23.  Bit 23 is the splitter mark
 * and bit 22 the guess mark: the row's F symbol is not the guess table's.
 * build_t, mark_guess_misses and mark_splitters address bytes 1 and 2
 * directly.
 */
#define SPLIT3 0x800000u
#define GUESS3 0x400000u

/*
 * Sets the guess mark on every 3-byte entry whose row's F symbol differs
 * from the guess table's entry for its group of rows, so that the walk can
 * take every other row's symbol from the guess table without checking it.
 * Those are the rows of each symbol that starts inside a group, up to the end
 * of that group: at most one group per symbol.
 */
static void
mark_guess_misses(bzng_dstate *s)
{
	const u32 gmask = (1u << s->fsym_shift) - 1;
	for (int c = 0; c < 256; c++) {
		u32 r = s->cftab[c];
		u32 end = s->cftab[c + 1];
		if ((r & gmask) == 0)
			continue;
		if (end > (r | gmask) + 1)
			end = (r | gmask) + 1;
		for (; r < end; r++)
			s->tbl[3 * (size_t)r + 2] |= GUESS3 >> 16;
	}
}

/*
 * Stores v (< 2^24) as entry j with one 4-byte store; byte 3j+3 (the next
 * entry's low byte, or padding) is overwritten with zero.
 */
static inline void
store3w(u8 *p3, u32 j, u32 v)
{
	const u32 le = le32_bswap(v);

	memcpy(p3 + 3 * (size_t)j, &le, 4);
}

/* Entry j of a 3-byte table (reads one pad byte past the end). */
static inline u32
load3(const u8 *p3, u32 j)
{
	u32 e;
	memcpy(&e, p3 + 3 * (size_t)j, 4);
	return le32_bswap(e) & 0x00FFFFFFu;
}

/*
 * The scatter takes the parts four at a time, parts 0-3 and then 4-7, every
 * part named by a constant: reached through a run-time part offset, gcc
 * stores some cursors before their entries, which is slower.
 */
#define PARTS_LO(X) X(0) X(1) X(2) X(3)
#define PARTS_HI(X) X(4) X(5) X(6) X(7)
/* Row i of part k in the scatter: its symbol c##k, cursor j##k and row. */
#define PART_SYM(k) const u32 c##k = L[i + (k) * q];
#define PART_CUR(k) const u32 j##k = cf[k][c##k];
#define PART_ROW(k) (i + (k) * q)
#define PART_STORE(k)                       \
	tt[j##k] = PART_ROW(k) << 8 | c##k; \
	cf[k][c##k] = j##k + 1;
#define PART_PUT(k)  tt[cf[k][c##k]++] = PART_ROW(k) << 8 | c##k;
#define PART_PUT3(k) store3w(p3, cf[k][L[i + (k) * q]]++, PART_ROW(k));

/*
 * Builds the T table from L (T(LF(i)) = i), BUILD_PARTS parts with cursors of
 * their own. fit: 3-byte entries holding the 20-bit pointer; otherwise 4-byte
 * entries i << 8 | L[i] (the symbol is F of the row being indexed).
 */
static void
build_t(bzng_dstate *s, u32 n, int fit)
{
	u32 cf[BUILD_PARTS][256];
	const u8 *L = s->L;
	part_hist(L, n, cf);
	cursors(s, cf);
	const u32 q = n / BUILD_PARTS;
	if (fit) {
		/*
		 * One 4-byte store per entry: it also zeroes the low byte of
		 * the next entry, which within a (part, symbol) cursor range
		 * is written later anyway; the first entry of each range is
		 * repaired below.
		 */
		u8 *p3 = s->tbl;
		for (u32 i = 0; i < q; i++) {
			PARTS_LO(PART_PUT3)
		}
		for (u32 i = 0; i < q; i++) {
			PARTS_HI(PART_PUT3)
		}
		for (u32 i = BUILD_PARTS * q; i < n; i++)
			store3w(p3, cf[BUILD_PARTS - 1][L[i]]++, i);
		/*
		 * A range's first entry holds the first i of its part with
		 * that symbol; bits 8-19 survived, so the low byte is found by
		 * a scan of at most 256 symbols.  Part k's range for c starts
		 * where part k - 1's cursor for c ended, and is empty if part
		 * k's cursor ends there too.
		 */
		for (int k = 0; k < BUILD_PARTS; k++) {
			const u32 q_lo = (u32)k * q;
			for (int c = 0; c < 256; c++) {
				const u32 first =
					k ? cf[k - 1][c] : s->cftab[c];
				if (cf[k][c] == first)
					continue;
				u8 *e = p3 + 3 * (size_t)first;
				u32 i = ((u32)e[1] | (u32)e[2] << 8) << 8;
				if (i < q_lo)
					i = q_lo;
				while (L[i] != c)
					i++;
				e[0] = (u8)i;
			}
		}
	} else {
		u32 *tt = (u32 *)(void *)s->tbl;
		/*
		 * Each part has its own cursors, so all the cursors of an
		 * iteration are loaded before any entry is stored, and each
		 * entry is then stored before its cursor: with clang that order
		 * decodes repetitive data markedly faster on Granite Rapids
		 * (spec D7 has the measurement). The cursors never escape, so
		 * the compiler may reorder these accesses; clang keeps the
		 * order written here. gcc gets the post-increment form, which
		 * it compiles to tighter code that still stores each entry
		 * before its cursor as long as every part is named by a
		 * constant.
		 */
		for (u32 i = 0; i < q; i++) {
			PARTS_LO(PART_SYM)
#if GCC_PREREQ(1, 0)
			PARTS_LO(PART_PUT)
#else
			PARTS_LO(PART_CUR)
			PARTS_LO(PART_STORE)
#endif
		}
		for (u32 i = 0; i < q; i++) {
			PARTS_HI(PART_SYM)
#if GCC_PREREQ(1, 0)
			PARTS_HI(PART_PUT)
#else
			PARTS_HI(PART_CUR)
			PARTS_HI(PART_STORE)
#endif
		}
		for (u32 i = BUILD_PARTS * q; i < n; i++) {
			const u32 c = L[i];
			tt[cf[BUILD_PARTS - 1][c]++] = i << 8 | c;
		}
	}
}

/* Index of `row` in the sorted splitter list (row must be a splitter). */
static u32
seg_index(const bzng_dstate *s, u32 row)
{
	u32 lo = 0, hi = s->n_seg - 1;
	while (lo < hi) {
		const u32 mid = (lo + hi) >> 1;
		if (s->seg_row[mid] < row)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Chooses splitter rows (evenly spaced plus origPtr), marks them in the
 * table and returns the lane count.  Each lane gets at least three segments
 * where the arrays allow: the lock-step walk ends when the first lane runs
 * out of segments, and the other lanes then finish theirs with less overlap.
 */
static u32
mark_splitters(bzng_dstate *s, u32 n, int fit)
{
	u32 lanes = 2;
	if (n >= 4096)
		lanes = BZNG_LANES;
	else if (n >= 1024)
		lanes = 8;
	u32 want = n / BZNG_SEG_ROWS;
	/* seg_cap is at least 50, so 8 or 24 lanes get a segment each */
	STATIC_ASSERT(100000u / BZNG_SEG_ROWS + 2 > BZNG_LANES);
	if (lanes > 2 && want < 3 * lanes)
		want = 3 * lanes;
	if (want < 1)
		want = 1;
	if (want > s->seg_cap - 1)
		want = s->seg_cap - 1;
	const u32 op = (u32)s->orig_ptr;
	u32 m = 0;
	int inserted = 0;
	for (u32 k = 0; k < want; k++) {
		const u32 r = (u32)(((u64)k * n) / want);
		if (!inserted && op <= r) {
			if (m == 0 || s->seg_row[m - 1] != op)
				s->seg_row[m++] = op;
			inserted = 1;
		}
		if (m == 0 || s->seg_row[m - 1] != r)
			s->seg_row[m++] = r;
	}
	if (!inserted && (m == 0 || s->seg_row[m - 1] != op))
		s->seg_row[m++] = op;
	s->n_seg = m;
	for (u32 k = 0; k < m; k++) {
		if (fit)
			s->tbl[3 * (size_t)s->seg_row[k] + 2] |= SPLIT3 >> 16;
		else
			((u32 *)(void *)s->tbl)[s->seg_row[k]] |= 0x80000000u;
	}
	s->seg_origin = seg_index(s, op);
	return m >= 2 ? lanes : 1;
}

/*
 * A lane's row and write position, the state every step reads and writes.
 * The write position is an offset into the pool and always names a writable
 * byte: a lane takes its next chunk as soon as its current one is full.
 */
typedef struct {
	u32 p; /* current row */
	u32 o; /* pool offset of the next byte */
} walk_hot;

/* The rest of a lane's state, which only segment and chunk ends touch. */
typedef struct {
	u32 written; /* bytes written by this lane before its current chunk */
	u32 seg;     /* segment being walked */
	u32 mark;    /* lane byte count at the segment's start */
} walk_cold;

typedef struct {
	u8 *pool;
	u32 *chunk_next;
	u32 next_chunk;
	u32 next_split;
} walk_pool;

/* Takes a lane's first pool chunk and returns its offset. */
static inline u32
first_chunk(walk_pool *w)
{
	const u32 k = w->next_chunk++;
	w->chunk_next[k] = 0;
	return k * BZNG_CHUNK;
}

/*
 * Takes the next pool chunk for a lane whose current chunk ends at offset
 * `full`, links it after that chunk and returns its offset.
 */
static inline u32
take_chunk(walk_pool *w, walk_cold *c, u32 full)
{
	const u32 k = w->next_chunk++;
	w->chunk_next[full / BZNG_CHUNK - 1] = k;
	w->chunk_next[k] = 0;
	c->written += BZNG_CHUNK;
	return k * BZNG_CHUNK;
}

/* Writes one byte at a lane's write position *o and advances it. */
static forceinline void
lane_put(walk_pool *w, walk_cold *c, u8 *pool, u32 *o, u8 sym)
{
	pool[*o] = sym;
	if (unlikely(++*o % BZNG_CHUNK == 0))
		*o = take_chunk(w, c, *o);
}

/*
 * Starts segment `seg` on a lane whose next byte goes to pool offset o:
 * records where the segment's bytes begin, emits the splitter row's own byte
 * and returns the lane's successor row and write position.  They are
 * returned by value so that no pointer to a lane's locals leaves the step.
 */
static walk_hot
seg_begin(bzng_dstate *s, walk_pool *w, walk_cold *c, u8 *pool, u32 o, u32 seg,
	  int fit)
{
	walk_hot h;
	s->seg_start[seg] = o;
	c->seg = seg;
	c->mark = c->written + o % BZNG_CHUNK;
	const u32 row = s->seg_row[seg];
	u8 sym;
	if (fit) {
		sym = row_symbol(s, row);
		h.p = load3(s->tbl, row) & PMASK;
	} else {
		const u32 e = ((const u32 *)(const void *)s->tbl)[row];
		sym = (u8)e;
		h.p = (e >> 8) & PMASK;
	}
	h.o = o;
	lane_put(w, c, pool, &h.o, sym);
	return h;
}

/*
 * Records the length and successor segment of the segment a lane has just
 * finished at row p, with its write position at o.
 */
static inline void
seg_end(bzng_dstate *s, const walk_cold *c, u32 p, u32 o)
{
	s->seg_len[c->seg] = c->written + o % BZNG_CHUNK - c->mark;
	s->seg_next[c->seg] = seg_index(s, p);
}

/*
 * One step of a lane: emits the current row's F byte and follows T, or ends
 * the segment at a marked row. Returns 0 when the lane has nothing left.
 * The lane's row and write position come by pointer so that the lock-step
 * loops can keep them in locals.  tbl, pool and fsh are the walk's copies of
 * s->tbl, s->L and s->fsym_shift: the pool's byte stores may alias the
 * state, so reading s would reload them on every step.
 * pf: prefetch the next row's table entry as soon as its index is known.
 */
static forceinline int
lane_step(bzng_dstate *s, walk_pool *w, walk_cold *c, u32 *p, u32 *o,
	  const u8 *tbl, u8 *pool, u32 fsh, int fit, int pf)
{
	u32 e, next;
	u8 sym;
	if (fit) {
		e = load3(tbl, *p);
		if (unlikely(e & (SPLIT3 | GUESS3))) {
			if (e & SPLIT3)
				goto seg_done;
			sym = row_symbol(s, *p);
		} else {
			const u8 *const guess = guess_table_r(s);

			sym = guess[*p >> fsh];
		}
		next = e & PMASK;
	} else {
		e = ((const u32 *)(const void *)tbl)[*p];
		if (unlikely(e & 0x80000000u))
			goto seg_done;
		sym = (u8)e;
		next = (e >> 8) & PMASK;
	}
	if (pf)
		prefetchr(fit ? tbl + 3 * (size_t)next :
				tbl + 4 * (size_t)next);
	*p = next;
	lane_put(w, c, pool, o, sym);
	return 1;
seg_done:
	seg_end(s, c, *p, *o);
	if (w->next_split < s->n_seg) {
		const walk_hot h =
			seg_begin(s, w, c, pool, *o, w->next_split++, fit);
		*p = h.p;
		*o = h.o;
		return 1;
	}
	return 0;
}

/*
 * Walks with at least this many lanes (blocks of at least 4 Ki rows)
 * prefetch.  Blocks of 64 Ki rows and more need it; for smaller ones, whose
 * table is in cache, it measured neither faster nor slower on the M4.
 */
#define PREFETCH_MIN_LANES 16

/* Lane i's hot state in locals of its own, one step of it, and back. */
#define LANE_IN(i)  u32 p##i = h[i].p, o##i = h[i].o;
#define LANE_OUT(i) h[i].p = p##i, h[i].o = o##i;
#define LANE_STEP(i)                                                        \
	if (!lane_step(s, w, &c[i], &p##i, &o##i, tbl, pool, fsh, fit, pf)) \
		dead |= 1u << (i);

/* One step of lane i with its hot state left in the array. */
#define LANE_STEP_MEM(i)                                                   \
	if (!lane_step(s, w, &c[i], &h[i].p, &h[i].o, tbl, pool, fsh, fit, \
		       pf))                                                \
		dead |= 1u << (i);

#define LANES_1(X) X(0)
#define LANES_2(X) X(0) X(1)
#define LANES_8(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define LANES_24(X)                                                       \
	LANES_8(X)                                                        \
	X(8)                                                              \
	X(9)                                                              \
	X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) \
		X(21) X(22) X(23)

/*
 * K lanes in lock step until one runs out of segments; returns the mask of
 * the lanes that have. Each lane's row and write position are copied into
 * locals of their own for the loop so that the compiler can keep them in
 * registers: the array, which the other phases index by variable, stays in
 * memory.
 */
#define DEFINE_LOCKSTEP_REG(K)                                           \
	static forceinline u32 lockstep_##K(                             \
		bzng_dstate *s, walk_pool *w, walk_hot *h, walk_cold *c, \
		const u8 *tbl, u8 *pool, u32 fsh, int fit, int pf)       \
	{                                                                \
		u32 dead = 0;                                            \
		LANES_##K(LANE_IN);                                      \
		do {                                                     \
			LANES_##K(LANE_STEP)                             \
		} while (!dead);                                         \
		LANES_##K(LANE_OUT);                                     \
		return dead;                                             \
	}

DEFINE_LOCKSTEP_REG(1)
DEFINE_LOCKSTEP_REG(2)
DEFINE_LOCKSTEP_REG(8)

/*
 * lockstep_K for more lanes than there are registers for: the hot state
 * stays in its array.  clang unrolls the loop fully, and its code measured
 * faster on Graviton4 than the written-out steps.  gcc 13 does not honour
 * the pragma here, and a loop of one lane step per iteration runs up to 12%
 * slower on Zen 5 depending only on where it lands in the binary, so other
 * compilers get the 24 steps written out.
 */
static forceinline u32
lockstep_24(bzng_dstate *s, walk_pool *w, walk_hot *h, walk_cold *c,
	    const u8 *tbl, u8 *pool, u32 fsh, int fit, int pf)
{
	u32 dead = 0;
#if defined(__clang__)
	while (!dead) {
		UNROLL_FULL
		for (int k = 0; k < 24; k++)
			LANE_STEP_MEM(k)
	}
#else
	do {
		LANES_24(LANE_STEP_MEM)
	} while (!dead);
#endif
	return dead;
}

/*
 * Walks every segment with K lanes in lock step, then finishes the
 * stragglers. Every table is a permutation of [0, n) because cftab is the
 * exact prefix sum of the symbols written, so no step needs a bounds check
 * and every segment ends at a marked row.
 */
#define DEFINE_WALK(K, FIT)                                                    \
	static void walk_##K##_##FIT(bzng_dstate *s, walk_pool *w)             \
	{                                                                      \
		const int fit = (FIT), pf = (K) >= PREFETCH_MIN_LANES;         \
		const u8 *const tbl = s->tbl;                                  \
		u8 *const pool = w->pool;                                      \
		const u32 fsh = fit ? s->fsym_shift : 0;                       \
		walk_hot h[K];                                                 \
		walk_cold c[K];                                                \
		u32 active = 0;                                                \
		while (active < (K) && w->next_split < s->n_seg) {             \
			c[active].written = 0;                                 \
			h[active] = seg_begin(s, w, &c[active], pool,          \
					      first_chunk(w), w->next_split++, \
					      fit);                            \
			active++;                                              \
		}                                                              \
		if (active == (K)) {                                           \
			const u32 dead = lockstep_##K(s, w, h, c, tbl, pool,   \
						      fsh, fit, pf);           \
			u32 a = 0;                                             \
			for (int k = 0; k < (K); k++) {                        \
				if (!((dead >> k) & 1)) {                      \
					h[a] = h[k];                           \
					c[a] = c[k];                           \
					a++;                                   \
				}                                              \
			}                                                      \
			active = a;                                            \
		}                                                              \
		while (active > 0) {                                           \
			for (u32 k = 0; k < active; k++) {                     \
				if (!lane_step(s, w, &c[k], &h[k].p, &h[k].o,  \
					       tbl, pool, fsh, fit, pf)) {     \
					active--;                              \
					h[k] = h[active];                      \
					c[k] = c[active];                      \
					k--;                                   \
				}                                              \
			}                                                      \
		}                                                              \
	}

DEFINE_WALK(1, 0)
DEFINE_WALK(2, 0)
DEFINE_WALK(8, 0)
DEFINE_WALK(24, 0)
DEFINE_WALK(1, 1)
DEFINE_WALK(2, 1)
DEFINE_WALK(8, 1)
DEFINE_WALK(24, 1)

/* Walks every segment with the given number of lanes and table width. */
static void
walk(bzng_dstate *s, u32 lanes, int fit)
{
	walk_pool w = {s->L, s->chunk_next, 0, 0};
	switch (lanes) {
	case 24:
		fit ? walk_24_1(s, &w) : walk_24_0(s, &w);
		break;
	case 8:
		fit ? walk_8_1(s, &w) : walk_8_0(s, &w);
		break;
	case 2:
		fit ? walk_2_1(s, &w) : walk_2_0(s, &w);
		break;
	default:
		fit ? walk_1_1(s, &w) : walk_1_0(s, &w);
		break;
	}
	/* The pool's slack covers one partly filled chunk per lane. */
	ASSERT(w.next_chunk <= s->chunk_cap);
}

/* libbz2's small-mode packed 20-bit entries (bzlib_private.h SET_LL/GET_LL). */
static inline u32
get_ll(const bzng_dstate *s, u32 i)
{
	const u16 *ll16 = (const u16 *)(const void *)s->tbl;
	return (u32)ll16[i] |
	       ((u32)((s->ll4[i >> 1] >> ((i << 2) & 0x4)) & 0xF) << 16);
}

/* Sets small-mode entry i to v (< 2^20) (libbz2's SET_LL). */
static inline void
set_ll(bzng_dstate *s, u32 i, u32 v)
{
	u16 *ll16 = (u16 *)(void *)s->tbl;
	ll16[i] = (u16)(v & 0xffff);
	const u32 hi = v >> 16;
	if ((i & 1) == 0)
		s->ll4[i >> 1] = (u8)((s->ll4[i >> 1] & 0xf0) | hi);
	else
		s->ll4[i >> 1] = (u8)((s->ll4[i >> 1] & 0x0f) | (hi << 4));
}

/*
 * Builds the inverse-BWT data for the block in s->L[0..n) and positions the
 * text reader at text position 0. No input or output is touched.
 */
void
bzng_finish_block(bzng_dstate *s)
{
	const u32 n = s->nblock;
	if (s->small) {
		/*
		 * L bytes sit at the start of the ll16 array. LF is computed in
		 * place from the end, so entry i (bytes 2i, 2i+1) only
		 * overwrites symbols already used; then libbz2's pointer
		 * reversal turns the cycle through origPtr into T.
		 */
		u32 h[BUILD_PARTS][256], cend[256];
		part_hist(s->L, n, h);
		cursors(s, h);
		for (int c = 0; c < 256; c++)
			cend[c] = s->cftab[c + 1];
		for (u32 i = n; i-- > 0;) {
			const u8 c = s->L[i];
			set_ll(s, i, --cend[c]);
		}
		u32 i = (u32)s->orig_ptr, j = get_ll(s, i);
		do {
			const u32 tmp = get_ll(s, j);
			set_ll(s, j, i);
			i = j;
			j = tmp;
		} while (i != (u32)s->orig_ptr);
		build_fsym(s, n);
		s->source = BZNG_SRC_SMALL;
		s->chain_pos = (u32)s->orig_ptr;
	} else {
		/*
		 * The fit layout reserves 3 bytes per row of the digit's block
		 * size for the table; a block of at most 3/4 of that size fits
		 * 4-byte entries there, which need no F-symbol lookup in the
		 * walk.
		 */
		const int fit = s->layout == BZNG_LAYOUT_FIT &&
				4 * (size_t)n > s->tbl_bytes;
		build_t(s, n, fit);
		if (fit) {
			build_fsym(s, n);
			mark_guess_misses(s);
		}
		const u32 lanes = mark_splitters(s, n, fit);
		walk(s, lanes, fit);
		ASSERT(s->cftab[256] == n);
		s->source = BZNG_SRC_POOL;
		s->rd_seg = s->seg_origin;
		s->rd_off = s->seg_start[s->rd_seg];
		s->rd_seg_left = s->seg_len[s->rd_seg];
	}
	s->text_left = n;
	s->rd_len = 0;
	s->rd_ptr = NULL;
	s->r_n_to_go = 0;
	s->r_t_pos = 0;
	s->ur_eq = 0;
	s->ur_b = 0;
	s->pend_len = 0;
	s->pend_ch = 0;
	s->corrupt_pending = 0;
	s->calc_block_crc = 0xffffffffu;
}

/* ---- text source --------------------------------------------------------- */

/*
 * Next contiguous run of walk output from the pool, following segments in
 * text order (wrapping round the cycle as a forward walk would).
 */
static void
pool_piece(bzng_dstate *s, const u8 **ptr, u32 *len)
{
	if (s->rd_seg_left == 0) {
		s->rd_seg = s->seg_next[s->rd_seg];
		s->rd_off = s->seg_start[s->rd_seg];
		s->rd_seg_left = s->seg_len[s->rd_seg];
	}
	const u32 chunk = s->rd_off / BZNG_CHUNK;
	const u32 room = (chunk + 1) * BZNG_CHUNK - s->rd_off;
	const u32 l = room < s->rd_seg_left ? room : s->rd_seg_left;
	*ptr = s->L + s->rd_off;
	*len = l;
	s->rd_seg_left -= l;
	s->rd_off =
		(l == room) ? s->chunk_next[chunk] * BZNG_CHUNK : s->rd_off + l;
}

/* Next text symbol of the small-mode chain (before randomisation). */
static inline u8
chain_symbol(bzng_dstate *s)
{
	const u32 row = s->chain_pos;
	const u8 c = row_symbol(s, row);
	s->chain_pos = get_ll(s, row);
	return c;
}

/* libbz2's BZ_RAND_UPD_MASK / BZ_RAND_MASK for the next text position. */
static inline u8
rand_mask(bzng_dstate *s)
{
	if (s->r_n_to_go == 0) {
		s->r_n_to_go = bzng_rand_nums[s->r_t_pos];
		s->r_t_pos++;
		if (s->r_t_pos == 512)
			s->r_t_pos = 0;
	}
	s->r_n_to_go--;
	return s->r_n_to_go == 1 ? 1 : 0;
}

/*
 * Makes the next piece of text (at most `limit` bytes) current in
 * s->rd_ptr/s->rd_len. Pool blocks that are not randomised hand out the pool
 * directly; everything else goes through minibuf.
 */
void
bzng_next_piece(bzng_dstate *s, u32 limit)
{
	if (s->source == BZNG_SRC_POOL && !s->randomised) {
		const u8 *p;
		u32 l;
		pool_piece(s, &p, &l);
		if (l > limit) {
			/*
			 * keep the rest for a later piece: step the reader back
			 */
			const u32 back = l - limit;
			s->rd_seg_left += back;
			s->rd_off = (u32)(p - s->L) + limit;
			l = limit;
		}
		s->rd_ptr = p;
		s->rd_len = l;
		return;
	}
	u32 m = limit < BZNG_MINIBUF ? limit : BZNG_MINIBUF;
	u8 *mb = s->minibuf;
	if (s->source == BZNG_SRC_POOL) {
		/*
		 * randomised pool block: the pool reader state lives in rd_*
		 * too, so drain it byte-wise into minibuf through a private
		 * cursor
		 */
		for (u32 j = 0; j < m; j++) {
			if (s->rd_seg_left == 0 && s->rd_len == 0) {
				s->rd_seg = s->seg_next[s->rd_seg];
				s->rd_off = s->seg_start[s->rd_seg];
				s->rd_seg_left = s->seg_len[s->rd_seg];
			}
			const u32 chunk = s->rd_off / BZNG_CHUNK;
			const u8 c = s->L[s->rd_off];
			s->rd_seg_left--;
			s->rd_off++;
			if (s->rd_off == (chunk + 1) * BZNG_CHUNK &&
			    s->rd_seg_left)
				s->rd_off = s->chunk_next[chunk] * BZNG_CHUNK;
			mb[j] = c ^ rand_mask(s);
		}
	} else if (s->randomised) {
		for (u32 j = 0; j < m; j++)
			mb[j] = chain_symbol(s) ^ rand_mask(s);
	} else {
		for (u32 j = 0; j < m; j++)
			mb[j] = chain_symbol(s);
	}
	s->rd_ptr = mb;
	s->rd_len = m;
}

/*
 * Sets s->area_bytes (and s->area2_bytes in small mode) to what the
 * per-stream arrays of the header digit and layout need, with the sizes they
 * are carved into.  The driver allocates the areas (decompress.c), then
 * calls bzng_place_stream_arrays.
 */
void
bzng_size_stream_arrays(bzng_dstate *s)
{
	const u32 nmax = 100000u * (u32)s->block_size_100k;
	if (s->small) {
		s->area_bytes = 2 * (size_t)nmax + 64;
		s->area2_bytes = ((1 + (size_t)nmax) >> 1) + 8;
		return;
	}
	s->tbl_bytes = ((s->layout == BZNG_LAYOUT_FIT ? 3 : 4) * (size_t)nmax +
			8 + 15) &
		       ~(size_t)15;
	s->chunk_cap = (nmax + (BZNG_LANES + 1) * BZNG_CHUNK) / BZNG_CHUNK + 1;
	s->seg_cap = nmax / BZNG_SEG_ROWS + 2;
	s->area_bytes = s->tbl_bytes + (size_t)s->chunk_cap * BZNG_CHUNK +
			(size_t)s->chunk_cap * 4 + (size_t)s->seg_cap * 16;
	s->area2_bytes = 0;
}

/* Carves the allocated s->area (and s->area2) into the per-stream arrays. */
void
bzng_place_stream_arrays(bzng_dstate *s)
{
	if (s->small) {
		s->tbl = s->area;
		s->L = s->area;
		s->ll4 = s->area2;
		return;
	}
	const size_t pool_bytes = (size_t)s->chunk_cap * BZNG_CHUNK;
	s->tbl = s->area;
	s->L = s->area + s->tbl_bytes;
	s->chunk_next = (u32 *)(void *)(s->L + pool_bytes);
	s->seg_row = s->chunk_next + s->chunk_cap;
	s->seg_start = s->seg_row + s->seg_cap;
	s->seg_len = s->seg_start + s->seg_cap;
	s->seg_next = s->seg_len + s->seg_cap;
}
