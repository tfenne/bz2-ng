/*
 * dec.h - decoder internals
 *
 * Decoder internals shared by the legacy driver (decompress.c), block
 * finishing (dec_block.c) and the kernel tiers (kern_template.h).
 *
 * The decoder is a resumable state machine with the observable behaviour of
 * libbz2 1.0.8, built from three parts:
 * 1. stream and block header parsing: a per-bit resumable machine with the
 *    structure of libbz2's decompress.c;
 * 2. the symbol loop (a kernel): a margin-gated fast path (table-driven
 *    Huffman over a 64-bit buffer, register-head inverse MTF) and an exact
 *    per-symbol path used near the end of the caller's input;
 * 3. block finishing (inverse-BWT table build and walk, no I/O) followed by
 *    a resumable un-RLE1 + CRC output stage.
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */
#ifndef LIB_DEC_H
#define LIB_DEC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"
#include "kern.h"
#include "lib_common.h"

#define BZNG_HUFF_K   11 /* primary Huffman table width in bits */
#define BZNG_MAX_CODE 20 /* longest code bzip2 allows */
#define BZNG_INVALID_BITS \
	21 /* libbz2 reads 21 bits before rejecting a pattern */
/*
 * Input bytes that must remain for the unchecked group decoder: 50 codes of
 * at most 20 bits (125 bytes) plus the 8-byte refill loads.
 */
#define BZNG_FAST_IN_MARGIN 160
#define BZNG_CHUNK	    1024u  /* walk output pool chunk */
#define BZNG_SEG_ROWS	    2048u  /* target rows per splitter segment */
#define BZNG_FSYM_MAX	    16384u /* row -> F symbol guess table entries */
#define BZNG_MINIBUF \
	512u /* text staging for chain walks and randomised blocks */
#define BZNG_LEN_SLACK 16 /* bytes after each code-length set */

/*
 * Internal results of the decode and output stages (never returned to the
 * caller).
 */
enum {
	BZNG_NEED = 1000,
	BZNG_BLOCK_READY = 1001,
	BZNG_EOB = 1002,
	BZNG_DISCARD = 1003
};

/*
 * Per-stream memory layout of the block-proportional arrays (small = 0):
 *
 *   BZNG_LAYOUT_WIDE: 4-byte table entries for every block, and a separate
 *     symbol array that becomes the walk's output pool: 100 kB + 5 x block
 *     size, one block more than libbz2 documents.  The default.
 *   BZNG_LAYOUT_FIT: a table of 3 bytes per row of the header digit's block
 *     size, inside libbz2's documented 100 kB + 4 x block size.  Blocks
 *     larger than three quarters of the block size use 3-byte entries
 *     (20-bit pointer; the output symbol is recovered from the row), smaller
 *     ones 4-byte entries in the same space.
 *
 * small = 1 always uses libbz2's 2.5-byte packed table with one chain.
 */
typedef enum {
	BZNG_LAYOUT_FIT = 0,
	BZNG_LAYOUT_WIDE = 1
} bzng_layout;

/*
 * Chains walked in lockstep for blocks of at least 4 Ki rows, in both
 * layouts.  The walk's output pool keeps one chunk of slack per chain.
 */
#define BZNG_LANES 24u

/*
 * The layout BZ2_bzDecompressInit uses; the CMake option
 * BZ2NG_DECODER_IN_FORMULA sets it to BZNG_LAYOUT_FIT.
 */
#ifndef BZNG_DEFAULT_LAYOUT
#  define BZNG_DEFAULT_LAYOUT BZNG_LAYOUT_WIDE
#endif

/* Text sources for the output stage. */
typedef enum {
	BZNG_SRC_POOL = 0,
	BZNG_SRC_SMALL = 1,
	BZNG_SRC_TEXT = 2 /* the pool's text gathered in order into tbl */
} bzng_source;

/* Decoding table for one coding group. */
typedef struct {
	/* entry = symbol << 5 | length; length 0 = longer than K, or no code */
	u16 prim[1u << BZNG_HUFF_K];
	/*
	 * exclusive end of each length's codes, left-justified to 20 bits; may
	 * exceed 2^20 for oversubscribed sets (every pattern then matches)
	 */
	u32 limit_lj[BZNG_MAX_CODE + 2];
	s32 first_code[BZNG_MAX_CODE + 2];
	s32 first_idx[BZNG_MAX_CODE + 2];
	u16 perm[BZ_MAX_ALPHA_SIZE];
	u8 maxlen;
} bzng_htable;

/* Bits read from the caller during one BZ2_bzDecompress call. */
typedef struct bzng_bits {
	u64 buf; /* next bits at the top */
	u32 cnt; /* valid bits in buf */
	const u8 *p, *end;
} bzng_bits;

/*
 * hi - lo in bytes, for lo <= hi in one caller's buffer.  A pointer difference
 * is defined only up to PTRDIFF_MAX, and a 32-bit process can be handed a
 * longer buffer, so there the count is taken from the addresses.  With 64-bit
 * pointers no buffer comes near PTRDIFF_MAX, so the pointer difference is
 * used: gcc on x86-64 allocates registers in the symbol loop differently for
 * the address form.
 */
#if UINTPTR_MAX > 0xffffffffu
#  define BZNG_SPAN(lo, hi) ((hi) - (lo))
#else
#  define BZNG_SPAN(lo, hi) ((uintptr_t)(hi) - (uintptr_t)(lo))
#endif

/*
 * Decompression state; replaces libbz2's DState. Allocated through bzalloc
 * with slack and aligned internally (lib_common.h); raw is what bzfree gets.
 */
typedef struct bzng_dstate {
	bz_stream *strm;
	void *raw;
	s32 state;  /* BZ_X_* header states, or BZNG_ST_* below */
	s32 sticky; /* error returned again by any later call, 0 if none */
	u8 small;
	u8 verbosity;
	u8 randomised;
	u8 layout; /* bzng_layout */
	s32 block_size_100k;
	s32 curr_block_no;

	/*
	 * bits read from the caller but not yet used, MSB-aligned; bits below
	 * the top bit_cnt are zero
	 */
	u64 bit_buf;
	u32 bit_cnt;

	u32 stored_block_crc, stored_combined_crc;
	u32 calc_block_crc, calc_combined_crc;
	s32 orig_ptr;

	/* header parse loop variables (kept here so every read is resumable) */
	s32 i, j, t, curr;
	s32 n_in_use, alpha_size, n_groups, n_selectors;
	u64 sel_mtf; /* selector MTF list: byte k is the group at position k */
	u8 groups_used; /* bit g set if a kept selector names group g */
	u8 in_use16[16];
	u8 seq_to_unseq[256];
	u8 selector[BZ_MAX_SELECTORS];
	/*
	 * code lengths; the BZNG_LEN_SLACK bytes after each set take whole
	 * stores of runs of equal lengths (decompress.c)
	 */
	u8 len[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE + BZNG_LEN_SLACK];
	/*
	 * Used from a block's header to the end of its symbol loop; from then
	 * to the end of the block's output these bytes hold block finishing's
	 * row -> F symbol guess table instead (dec_block.c).
	 */
	bzng_htable tab[BZ_N_GROUPS];

	/* symbol loop */
	s32 group_no, group_pos;
	u32 eob;
	u32 nblock, nblock_max;
	u32 run, run_shift;
	u8 mtf[256 + 16];

	/* per-stream arrays: one allocation (two in small mode, as libbz2) */
	u8 *area, *area2;
	size_t area_bytes, area2_bytes;
	u8 *L;	 /* symbol-loop output; pool after the build */
	u8 *tbl; /* inverse-BWT table (3- or 4-byte entries) or small-mode
		    ll16; after the walk, a gathered block's text */
	u8 *ll4; /* small mode: high nibbles of the 20-bit entries */
	u32 *chunk_next;
	u32 *seg_row, *seg_start, *seg_len, *seg_next;
	u32 seg_cap, chunk_cap;
	size_t tbl_bytes; /* bytes reserved for the table */
	u32 n_seg, seg_origin;

	/* block finishing */
	u32 cftab[257];
	u32 fsym_shift;

	/* output stage */
	u8 source;     /* bzng_source */
	u32 text_left; /* text positions 0..n-1 not yet handed to un-RLE1 */
	const u8 *rd_ptr;
	u32 rd_len;			 /* bytes left in the current piece */
	u32 rd_seg, rd_seg_left, rd_off; /* pool reader: segment, bytes after
					    the piece, offset */
	u32 chain_pos;			 /* small mode: current row */
	s32 r_n_to_go, r_t_pos;		 /* randomisation mask state */
	u8 ur_b, ur_eq; /* un-RLE1: last byte, equal bytes ending at it (4 =
			   count next) */
	u8 pend_ch;
	u8 corrupt_pending;
	u32 pend_len;
	u8 minibuf[BZNG_MINIBUF + 32];
} bzng_dstate;

enum {
	BZNG_ST_SYMBOLS =
		100, /* inside the symbol loop, at a symbol boundary */
	BZNG_ST_OUTPUT = 101,
	BZNG_ST_IDLE = 102
};

/*
 * In/out of the bulk un-RLE1 kernel: text in[0..in_len) to out[0..avail),
 * from a clean run state (no equal bytes carried in).  text_after is the
 * number of the block's text bytes that follow in[in_len - 1].  The kernel
 * copies literals and expands runs while it has its margins; a run that does
 * not fit the output is left in pend_len and pend_ch.  On return eq is the
 * number (0..3) of equal literal bytes, of value last, that end the text
 * consumed.
 *
 * The kernel never leaves a byte of the caller's buffer modified beyond what
 * the decoder delivers, which libbz2 does only in the call that reports a
 * corrupt block (output() in decompress.c): its whole-vector stores stay
 * inside out[0..avail) but may run past io->out, by up to the longest run in
 * whole vectors, and it uses them only while the rest of the block is long
 * enough that its output will overwrite them (or the caller's buffer fills up
 * first); see kern_template.h.
 */
typedef struct bzng_unrle_io {
	const u8 *in;
	u32 in_len;
	u32 text_after;
	u8 *out;
	u32 avail;
	u32 pend_len;
	u8 pend_ch;
	u8 last, eq;
} bzng_unrle_io;

/* ---- bit reader ---------------------------------------------------------- */

/*
 * Pulls whole bytes, one at a time, until n (<= 32) bits are held or the
 * input is exhausted (libbz2's GET_BITS refill). Returns whether n bits are
 * held.
 */
static inline int
bzng_br_fill(bzng_bits *b, u32 n)
{
	while (b->cnt < n) {
		if (b->p == b->end)
			return 0;
		b->buf |= (u64)*b->p++ << (56 - b->cnt);
		b->cnt += 8;
	}
	return 1;
}

/*
 * Tops the buffer up to at least 56 bits with one 8-byte load; needs 8
 * readable input bytes at b->p. Bits loaded past cnt are the stream's next
 * bits, so later refills OR in the same values.
 */
static inline void
bzng_br_refill_fast(bzng_bits *b)
{
	b->buf |= get_unaligned_be64(b->p) >> b->cnt;
	b->p += (63 - b->cnt) >> 3;
	b->cnt |= 56;
}

/* Takes n (1..32) bits that bzng_br_fill has made available. */
static inline u32
bzng_br_take(bzng_bits *b, u32 n)
{
	u32 v = (u32)(b->buf >> (64 - n));
	b->buf <<= n;
	b->cnt -= n;
	return v;
}

/* ---- Huffman decoding ---------------------------------------------------- */

/* Builds the decoding table t for one code-length set (dec_block.c). */
void bzng_build_htable(bzng_htable *t, const u8 *length, s32 alpha_size);

/*
 * The code length held in a primary-table entry (its low 5 bits).  gcc
 * computes e & 31 as a 16-bit operation and widens the result again, one
 * more step on the group decoder's critical path; with BMI2 it is one bzhi.
 */
#if GCC_PREREQ(1, 0) && defined(__BMI2__)
#  include <immintrin.h>
#  define BZNG_CODE_LEN(e) _bzhi_u32((e), 5)
#else
#  define BZNG_CODE_LEN(e) ((e) & 31)
#endif

/*
 * Decodes one code from the top bits of buf (at least 20 meaningful bits:
 * real stream bits, or anything at all past the stream's available bits, see
 * bzng_decode_exact). Returns the code length and sets *sym, or 0 if the
 * pattern is no code.
 */
static inline u32
bzng_decode_code(const bzng_htable *t, u64 buf, u32 *sym)
{
	const u32 e = t->prim[buf >> (64 - BZNG_HUFF_K)];
	if (likely(BZNG_CODE_LEN(e))) {
		*sym = e >> 5;
		return BZNG_CODE_LEN(e);
	}
	const u32 v = (u32)(buf >> (64 - BZNG_MAX_CODE));
	for (u32 l = BZNG_HUFF_K + 1; l <= t->maxlen; l++) {
		if (v < t->limit_lj[l]) {
			*sym = t->perm[t->first_idx[l] +
				       (s32)(v >> (BZNG_MAX_CODE - l)) -
				       t->first_code[l]];
			return l;
		}
	}
	return 0;
}

/*
 * Decodes one code when only `avail` bits are real. The decision at length l
 * depends only on the first l bits, so a code of length <= avail is decided
 * exactly whatever follows. Returns 1 with the symbol and length, 0 if more
 * input is needed, or -1 for no code (only once 21 bits are available).
 */
static inline int
bzng_decode_exact(const bzng_htable *t, u64 buf, u32 avail, u32 *sym, u32 *len)
{
	const u32 l = bzng_decode_code(t, buf, sym);
	if (l) {
		if (l > avail)
			return 0;
		*len = l;
		return 1;
	}
	return avail >= BZNG_INVALID_BITS ? -1 : 0;
}

/*
 * gcc inlines the group decoder into the symbol loop and then keeps the
 * loop's run state on the stack; out of line, the run state stays in
 * registers. (gcc warns about noinline on an inline function, hence
 * MAYBE_UNUSED for the files that include this header without calling it.)
 */
#if GCC_PREREQ(1, 0)
#  define GROUP_DECODER_INLINE MAYBE_UNUSED __attribute__((noinline))
#else
#  define GROUP_DECODER_INLINE inline
#endif

/*
 * Decodes up to 50 codes of one group into syms, stopping after the EOB
 * symbol, without bounds checks: the caller guarantees BZNG_FAST_IN_MARGIN
 * input bytes. Refills once per two codes (a refill leaves at least 56 bits
 * and a code has at most 20). Returns the count, or -1 if a pattern is no
 * code (br is then unchanged).
 */
static GROUP_DECODER_INLINE int
bzng_fast_group(bzng_bits *br, const bzng_htable *t, u16 *syms, u32 eob)
{
	u64 buf = br->buf;
	u32 cnt = br->cnt;
	const u8 *p = br->p;
	int k = 0;
	while (k < BZ_G_SIZE) {
		buf |= get_unaligned_be64(p) >> cnt;
		p += (63 - cnt) >> 3;
		cnt |= 56;
		for (int two = 0; two < 2; two++) {
			u32 sym;
			const u32 l = bzng_decode_code(t, buf, &sym);
			if (unlikely(l == 0))
				return -1;
			buf <<= l;
			cnt -= l;
			syms[k++] = (u16)sym;
			if (sym == eob)
				goto done;
		}
	}
done:
	br->buf = buf;
	br->cnt = cnt;
	br->p = p;
	return k;
}

/*
 * Advances br past the first n codes of a group that bzng_fast_group decoded
 * from the same position (used to find libbz2's exact stopping point).
 */
static inline void
bzng_skip_codes(bzng_bits *br, const bzng_htable *t, int n)
{
	for (int k = 0; k < n; k++) {
		bzng_br_refill_fast(br);
		u32 sym;
		const u32 l = bzng_decode_code(t, br->buf, &sym);
		br->buf <<= l;
		br->cnt -= l;
	}
}

/* ---- API (decompress.c) -------------------------------------------------- */

/*
 * BZ2_bzDecompressInit with the per-stream memory layout given (a
 * bzng_layout); BZ2_bzDecompressInit passes BZNG_DEFAULT_LAYOUT. Internal:
 * tests and the benchmark call it to measure both layouts.
 */
int bzng_decompress_init(bz_stream *strm, int verbosity, int small, int layout);

/* ---- block finishing (dec_block.c) --------------------------------------- */

/*
 * Sizes of the per-stream arrays for the header digit and layout (into
 * s->area_bytes, s->area2_bytes), and their placement once the driver has
 * allocated them.
 */
void bzng_size_stream_arrays(bzng_dstate *s);
void bzng_place_stream_arrays(bzng_dstate *s);

/*
 * Builds the inverse-BWT data for the block in s->L[0..s->nblock) and
 * positions the text reader at text position 0. Touches no input or
 * output.
 */
void bzng_finish_block(bzng_dstate *s);

/*
 * Makes the next piece of text, at most limit bytes, current in
 * s->rd_ptr/s->rd_len (randomisation applied).
 */
void bzng_next_piece(bzng_dstate *s, u32 limit);

#endif /* LIB_DEC_H */
