/*
 * kern_template.h - the kernels of one tier
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */

/*
 * This file is included once per kernel tier, by a translation unit compiled
 * with that tier's instruction-set flags (kern_scalar.c, arm/kern_neon.c,
 * x86/kern_sse4.c, x86/kern_avx2.c).  Before including it, define SUFFIX to
 * the tier's suffix (e.g. _avx2), and KERN_NO_VECTORS for the portable tier.
 * It defines the five kernels of kern.h with that suffix; everything else in
 * it is static.  Every tier produces the same output.
 *
 * The portable tier's SWAR code works on 8-byte words in little-endian order
 * on every target: byte k of the input is bits 8k..8k+7, as lane k is in a
 * vector tier's masks.  Native loads are converted with a byte swap that is
 * a no-op on little-endian targets; a word of byte-wise results (zero_bytes,
 * xor) is converted once, after the byte-wise operations, which do not depend
 * on the order.
 *
 * Kernels: the encoder's RLE1 input stage, MTF and zero-run coding and least
 * rotation; the decoder's symbol loop and bulk un-RLE1.
 */

#ifndef SUFFIX
#  error "SUFFIX must name the tier"
#endif

#include "dec.h"
#include "enc.h"
#include "vec.h"

/* ---- encoder: change masks ----------------------------------------------- */

/*
 * Bit k set iff p[k] != p[k-1], k = 0..63 (reads p[-1..64)).  MTF visits the
 * positions where the BWT changes; the RLE1 input stage reads the mask one
 * byte on, as the positions that differ from their successor.
 */
#if VEC_X86
static inline u64
change_mask64(const u8 *p)
{
#  if defined(__AVX2__)
	u64 lo = (u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
		_mm256_loadu_si256((const __m256i *)p),
		_mm256_loadu_si256((const __m256i *)(p - 1))));
	u64 hi = (u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
		_mm256_loadu_si256((const __m256i *)(p + 32)),
		_mm256_loadu_si256((const __m256i *)(p + 31))));
	return ~(lo | hi << 32);
#  else
	u64 m = 0;
	for (int k = 0; k < 4; k++)
		m |= vec_mask(
			     vec_eq(vec_ld(p + 16 * k), vec_ld(p + 16 * k - 1)))
		     << (16 * k);
	return ~m;
#  endif
}
#elif VEC_NEON
static inline u64
change_mask64(const u8 *p)
{
	static const u8 bits[16] = {1, 2, 4, 8, 16, 32, 64, 128,
				    1, 2, 4, 8, 16, 32, 64, 128};
	uint8x16_t w = vld1q_u8(bits);
	uint8x16_t e0 = vandq_u8(vceqq_u8(vld1q_u8(p), vld1q_u8(p - 1)), w);
	uint8x16_t e1 =
		vandq_u8(vceqq_u8(vld1q_u8(p + 16), vld1q_u8(p + 15)), w);
	uint8x16_t e2 =
		vandq_u8(vceqq_u8(vld1q_u8(p + 32), vld1q_u8(p + 31)), w);
	uint8x16_t e3 =
		vandq_u8(vceqq_u8(vld1q_u8(p + 48), vld1q_u8(p + 47)), w);
	uint8x16_t s = vpaddq_u8(vpaddq_u8(e0, e1), vpaddq_u8(e2, e3));
	s = vpaddq_u8(s, s);
	return ~vgetq_lane_u64(vreinterpretq_u64_u8(s), 0);
}
#else
static inline u64
change_mask64(const u8 *p)
{
	const u64 lo7 = 0x7f7f7f7f7f7f7f7f;
	u64 m = 0;

	for (int k = 0; k < 8; k++) {
		u64 x = load_u64_unaligned(p + 8 * k) ^
			load_u64_unaligned(p + 8 * k - 1);
		/* 0x80 in each nonzero byte, exactly; byte j at bits 8j.. */
		u64 nz = le64_bswap((((x & lo7) + lo7) | x) & ~lo7);
		/*
		 * Bit 8j to bit 56 + j: the product's terms land on distinct
		 * bits, so nothing carries into the top byte.
		 */
		m |= ((nz >> 7) * 0x0102040810204080ULL) >> 56 << (8 * k);
	}
	return m;
}
#endif

/* ---- encoder: RLE1 input stage ------------------------------------------- */

/*
 * Exactly libbz2's ADD_CHAR_TO_BLOCK / copy_input_until_stop semantics (see
 * enc.h), done in bulk: inside one call every unit that is followed by
 * another byte of the same call is complete, so whole literal spans and runs
 * are written at once; only the last unit of the call stays pending. The CRC
 * covers one contiguous input range per call, plus the pending unit carried
 * in from the previous call (whose bytes are no longer available, so they
 * are added as len copies of its byte).
 *
 * Per tier: change_mask64 (above) for the window phase; run4_mask (which of
 * RUN4_STEP positions start four equal bytes), copy_step (copies RUN4_STEP
 * bytes) and run_length for the step phase.
 */
#if VEC_X86 && defined(__AVX2__)
/* Bit k is set iff p[k..k+4) are equal (k = 0..31); reads p[0..35). */
static inline u64
run4_mask(const u8 *p)
{
	__m256i v0 = _mm256_loadu_si256((const __m256i *)p),
		v1 = _mm256_loadu_si256((const __m256i *)(p + 1));
	__m256i v2 = _mm256_loadu_si256((const __m256i *)(p + 2)),
		v3 = _mm256_loadu_si256((const __m256i *)(p + 3));
	__m256i e =
		_mm256_and_si256(_mm256_and_si256(_mm256_cmpeq_epi8(v0, v1),
						  _mm256_cmpeq_epi8(v1, v2)),
				 _mm256_cmpeq_epi8(v2, v3));
	return (u32)_mm256_movemask_epi8(e);
}
#  define RUN4_STEP  32
#  define RUN4_SHIFT 0
/* Copies RUN4_STEP bytes from src to dst. */
static inline void
copy_step(u8 *dst, const u8 *src)
{
	_mm256_storeu_si256((__m256i *)dst,
			    _mm256_loadu_si256((const __m256i *)src));
}

/* Number of bytes equal to c at p[0..avail). */
static inline size_t
run_length(const u8 *p, size_t avail, u8 c)
{
	size_t L = 0;
	__m256i vc = _mm256_set1_epi8((char)c);
	while (L + 32 <= avail) {
		u32 m = ~(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
			_mm256_loadu_si256((const __m256i *)(p + L)), vc));
		if (m)
			return L + (size_t)bsf64(m);
		L += 32;
	}
	while (L < avail && p[L] == c)
		L++;
	return L;
}
#elif VEC_ENABLED
/* Lane k is set iff p[k..k+4) are equal (k = 0..15); reads p[0..19). */
static inline u64
run4_mask(const u8 *p)
{
	vec_t v0 = vec_ld(p), v1 = vec_ld(p + 1), v2 = vec_ld(p + 2),
	      v3 = vec_ld(p + 3);
	return vec_mask(vec_and(vec_and(vec_eq(v0, v1), vec_eq(v1, v2)),
				vec_eq(v2, v3)));
}
#  define RUN4_STEP  16
#  define RUN4_SHIFT VEC_SHIFT
static inline void
copy_step(u8 *dst, const u8 *src)
{
	vec_st(dst, vec_ld(src));
}

/* Number of bytes equal to c at p[0..avail). */
static inline size_t
run_length(const u8 *p, size_t avail, u8 c)
{
	size_t L = 0;
	vec_t vc = vec_dup(c);
	while (L + 16 <= avail) {
		u64 m = ~vec_mask(vec_eq(vec_ld(p + L), vc)) & VEC_FULL;
		if (m)
			return L + ((size_t)bsf64(m) >> VEC_SHIFT);
		L += 16;
	}
	while (L < avail && p[L] == c)
		L++;
	return L;
}
#else
/* Bit 8k+7 is set iff p[k..k+4) are equal (k = 0..7); reads p[0..11). */
static inline u64
run4_mask(const u8 *p)
{
	u64 a = load_u64_unaligned(p), b = load_u64_unaligned(p + 1),
	    c = load_u64_unaligned(p + 2), d = load_u64_unaligned(p + 3);
	return le64_bswap(zero_bytes(a ^ b) & zero_bytes(b ^ c) &
			  zero_bytes(c ^ d));
}
#  define RUN4_STEP  8
#  define RUN4_SHIFT 3
static inline void
copy_step(u8 *dst, const u8 *src)
{
	memcpy(dst, src, 8);
}

static inline size_t
run_length(const u8 *p, size_t avail, u8 c)
{
	size_t L = 0;
	u64 vc = 0x0101010101010101ULL * c;
	while (L + 8 <= avail) {
		u64 x = le64_bswap(load_u64_unaligned(p + L) ^ vc);
		if (x)
			return L + ((size_t)bsf64(x) >> 3);
		L += 8;
	}
	while (L < avail && p[L] == c)
		L++;
	return L;
}
#endif

/* ---- block filling ------------------------------------------------------- */

/*
 * The fast phases keep the block this far below nblock_max, so the exact
 * unit-by-unit tail makes every block-full decision.
 */
#define RLE_GUARD 8
/* Calls with fewer input bytes than this take the byte loop. */
#define SMALL_CHUNK 16
/* Input the window phase reads from a window's start: its mask and the next. */
#define RLE_LOOKAHEAD 129

/*
 * Copies at least n bytes from src to dst in whole steps: up to
 * RUN4_STEP - 1 bytes past n, and RUN4_STEP when n is 0.
 */
static inline void
copy_literals(u8 *dst, const u8 *src, size_t n)
{
	copy_step(dst, src);
	for (size_t q = RUN4_STEP; q < n; q += RUN4_STEP)
		copy_step(dst + q, src + q);
}

/*
 * Window phase: from the unit boundary p with the block at *nbp, the runs of
 * 64-byte windows from their change masks.  In a window's mask d (bit k:
 * in[k] != in[k + 1]), a run of four or more starts at k when bits k, k + 1
 * and k + 2 are clear and ends at the next set bit, so successive runs come
 * from bit scans with no load between them; each mask is computed once, as
 * the lookahead of the window before.  Literal bytes are copied when a run
 * or the window's end comes, which may write RUN4_STEP bytes past limit.  A
 * run's 255-byte units are written as the windows pass it, each once a byte
 * of the run follows it, so a run longer than the block's room is read only
 * until the block is full.  It stops at a unit boundary, or between windows
 * inside a literal unit of <= 3 bytes that ends within this call (which writes
 * the same bytes however it is split), with the block at most at limit; it
 * returns where it stopped, every byte before which is in the block.
 */
static inline size_t
rle_windows(const u8 *in, size_t avail, size_t p, u8 *blk, s32 *nbp, s32 limit)
{
	s32 nb = *nbp;
	/*
	 * in[lit..) is not yet in the block; with in_run set it starts a run
	 * of four or more whose end has not been reached.
	 */
	size_t lit = p, base = p;
	int in_run = 0;

	if (avail - p < RLE_LOOKAHEAD)
		return p;
	u64 d = change_mask64(in + base + 1), dn;
	for (; avail - base >= RLE_LOOKAHEAD; base += 64, d = dn) {
		dn = change_mask64(in + base + 65);
		u64 r4 = ~(d | (d >> 1 | dn << 63) | (d >> 2 | dn << 62));
		u64 ends = d;
		for (;;) {
			if (in_run) {
				if (ends == 0)
					break; /* it goes on past this window */
				u32 e = bsf64(ends);
				size_t len = base + e + 1 - lit;
				u8 c = in[lit];
				while (len >= 255) {
					if (nb + 5 > limit)
						goto stop;
					nb = bzng_put_unit(blk, nb, c, 255);
					lit += 255;
					len -= 255;
				}
				if (len > 0) {
					if (nb + 5 > limit)
						goto stop;
					nb = bzng_put_unit(blk, nb, c,
							   (s32)len);
					lit += len;
				}
				in_run = 0;
				r4 &= ~(u64)1 << e;
			}
			if (r4 == 0)
				break;
			u32 k = bsf64(r4);
			size_t j = base + k;
			if (nb + (s32)(j - lit) > limit)
				goto stop;
			copy_literals(blk + nb, in + lit, j - lit);
			nb += (s32)(j - lit);
			lit = j;
			ends = d & (~(u64)0 << k);
			in_run = 1;
		}
		if (in_run) {
			/* in[lit..base + 64] are all the run's byte. */
			while (base + 64 - lit >= 255) {
				if (nb + 5 > limit)
					goto stop;
				nb = bzng_put_unit(blk, nb, in[lit], 255);
				lit += 255;
			}
		} else {
			if (nb + (s32)(base + 64 - lit) > limit)
				goto stop;
			copy_literals(blk + nb, in + lit, base + 64 - lit);
			nb += (s32)(base + 64 - lit);
			lit = base + 64;
		}
	}
stop:
	*nbp = nb;
	return lit;
}

size_t
ADD_SUFFIX(bzng_rle_fill)(bzng_rle_state *st, const u8 *in, size_t avail,
			  u8 *blk, s32 nblock_max)
{
	if (avail == 0)
		return 0;
	s32 nb = st->nblock;
	u32 crc = st->crc;
	size_t p = 0;

	/*
	 * Small calls: libbz2's byte loop, which costs less than setting up the
	 * bulk scan when there is little to scan.
	 */
	if (avail < SMALL_CHUNK) {
		u32 ch = st->ch;
		s32 len = st->len;
		for (; p < avail; p++) {
			u32 c = in[p];
			if (c != ch && len == 1) {
				crc = crc32_byte(crc, (u8)ch);
				blk[nb++] = (u8)ch;
				ch = c;
			} else if (c != ch || len == 255) {
				if (ch < 256) {
					crc = bzng_crc_repeat(crc, (u8)ch, len);
					nb = bzng_put_unit(blk, nb, (u8)ch,
							   len);
				}
				ch = c;
				len = 1;
			} else {
				len++;
				continue;
			}
			if (nb >= nblock_max) {
				p++;
				break;
			}
		}
		st->ch = ch;
		st->len = len;
		st->nblock = nb;
		st->crc = crc;
		return p;
	}

	/*
	 * The unit pending from the previous call: extend it, or write it out
	 * once a byte that ends it arrives.
	 */
	if (st->len > 0) {
		u8 c = (u8)st->ch;
		size_t room = (size_t)(255 - st->len);
		p = run_length(in, avail < room ? avail : room, c);
		if (p == avail) {
			st->len += (s32)p;
			return avail;
		}
		crc = bzng_crc_repeat(crc, c, st->len + (s32)p);
		nb = bzng_put_unit(blk, nb, c, st->len + (s32)p);
		if (nb >= nblock_max) {
			st->ch = in[p];
			st->len = 1;
			st->nblock = nb;
			st->crc = crc;
			return p + 1;
		}
	}

	/*
	 * From here every unit starts in this call; in[u..p) is written and not
	 * yet in the CRC.
	 */
	const size_t u = p;
	const s32 limit = nblock_max - RLE_GUARD;

	p = rle_windows(in, avail, p, blk, &nb, limit);

	/*
	 * Step phase, from wherever the window phase stopped (inside a run that
	 * outlasts its last window, at the block limit, or at the last
	 * RLE_LOOKAHEAD bytes it cannot read ahead of): whole literal spans and
	 * runs while the block has room for a whole step and the input for the
	 * loads. It stops only at a
	 * unit boundary or inside a literal unit of <= 3 bytes that ends within
	 * this call (which writes the same bytes however it is split), so it
	 * may start where the window phase stopped.
	 */
	while (nb + RUN4_STEP <= limit && p + 2 * RUN4_STEP <= avail) {
		u64 m = run4_mask(in + p);
		copy_step(blk + nb, in + p);
		if (likely(m == 0)) {
			nb += RUN4_STEP;
			p += RUN4_STEP;
			continue;
		}
		s32 j = (s32)(bsf64(m) >> RUN4_SHIFT);
		nb += j;
		p += (size_t)j;
		/*
		 * A maximal run of >= 4 starts at p.  If it reaches the end of
		 * the call, its last 1..255 bytes stay pending as one unit.
		 */
		u8 c = in[p];
		size_t L = run_length(in + p, avail - p, c);
		size_t keep = p + L == avail ? (L - 1) % 255 + 1 : 0;
		L -= keep;
		while (L >= 255) {
			if (nb + 5 > limit)
				goto tail;
			nb = bzng_put_unit(blk, nb, c, 255);
			p += 255;
			L -= 255;
		}
		if (L > 0) {
			if (nb + 5 > limit)
				goto tail;
			nb = bzng_put_unit(blk, nb, c, (s32)L);
			p += L;
		}
		if (keep > 0)
			goto pending;
	}

tail:
	/*
	 * Exact unit-by-unit tail: libbz2's block-full decision, and the
	 * pending unit at the end of the call.
	 */
	for (;;) {
		u8 c = in[p];
		size_t max = avail - p < 255 ? avail - p : 255;
		size_t L = 1;
		while (L < max && in[p + L] == c)
			L++;
		if (p + L == avail)
			break;
		nb = bzng_put_unit(blk, nb, c, (s32)L);
		p += L;
		if (nb >= nblock_max) {
			st->ch = in[p];
			st->len = 1;
			st->nblock = nb;
			st->crc = bzng_crc32(crc, in + u, p - u);
			return p + 1;
		}
	}
pending:
	/* in[p..avail) is one unit of at most 255 bytes, left pending. */
	st->ch = in[p];
	st->len = (s32)(avail - p);
	st->nblock = nb;
	st->crc = bzng_crc32(crc, in + u, p - u);
	return avail;
}

/* ---- encoder: MTF + zero-run coding -------------------------------------- */

/*
 * Output: mtfv[0..nMTF) with RUNA = 0, RUNB = 1, rank k >= 1 coded as k + 1,
 * EOB = nInUse + 1 last; mtf_freq[] their counts (libbz2's
 * generateMTFValues). Works on raw byte values: the list starts as the in-use
 * bytes in ascending order, which gives the same ranks as libbz2's list of
 * sequence numbers.
 * - Rank 0 is exactly L[i] == L[i-1], so only positions where the byte
 *   changes are visited (64-position change masks, vector tiers).
 * - List positions 0..7 and 8..15 live in two 64-bit registers (head,
 *   head1): ranks 1-15 are a SWAR byte search and a bit-select update, so
 *   the common ranks never touch memory.
 * - Positions 16..255 live in a 16-byte-aligned array (tail), searched and
 *   shifted in one pass, 16 bytes at a time (vector) or 8 (scalar).
 * - Zero runs: the RUNA/RUNB digits of a run of z are the bits of z + 1
 *   below its leading one, least significant first, written with one
 *   unconditional store and counted with a popcount.
 */

/*
 * Writes the digits of a zero run of length z >= 0 at mtfv[wr]; returns the
 * new write position. May write up to 8 values past it.
 */
static inline s32
put_zero_run(u16 *mtfv, s32 wr, s32 *mtf_freq, u32 z)
{
	u32 x = z + 1;
	s32 k = bsr32(x);
	if (likely(k <= 8)) {
#if VEC_NEON
		static const u16 bit[8] = {1, 2, 4, 8, 16, 32, 64, 128};
		uint16x8_t d = vandq_u16(
			vtstq_u16(vdupq_n_u16((u16)x), vld1q_u16(bit)),
			vdupq_n_u16(1));
		vst1q_u16(mtfv + wr, d);
#elif VEC_X86
		/* min(x & bit, 1) per 16-bit lane: the digit, 0 or 1 */
		__m128i d = _mm_min_epi16(
			_mm_and_si128(
				_mm_set1_epi16((short)x),
				_mm_setr_epi16(1, 2, 4, 8, 16, 32, 64, 128)),
			_mm_set1_epi16(1));
		_mm_storeu_si128((__m128i *)(mtfv + wr), d);
#else
		if (CPU_IS_LITTLE_ENDIAN()) {
			/* four 16-bit digits per 8-byte store, lane 0 low */
			u64 lo = (x & 1) | ((u64)((x >> 1) & 1) << 16) |
				 ((u64)((x >> 2) & 1) << 32) |
				 ((u64)((x >> 3) & 1) << 48);
			u64 hi = ((x >> 4) & 1) | ((u64)((x >> 5) & 1) << 16) |
				 ((u64)((x >> 6) & 1) << 32) |
				 ((u64)((x >> 7) & 1) << 48);
			memcpy(mtfv + wr, &lo, 8);
			memcpy(mtfv + wr + 4, &hi, 8);
		} else {
			for (s32 i = 0; i < 8; i++)
				mtfv[wr + i] = (u16)((x >> i) & 1);
		}
#endif
	} else {
		for (s32 i = 0; i < k; i++)
			mtfv[wr + i] = (u16)((x >> i) & 1);
	}
	s32 ones = popcount32(x & ((1u << k) - 1));
	mtf_freq[BZ_RUNB] += ones;
	mtf_freq[BZ_RUNA] += k - ones;
	return wr + k;
}

/*
 * Takes byte c out of the tail (list positions 16..255): the entries before
 * it move one position on and carry, the entry leaving position 15, takes
 * position 16.  Returns c's rank.
 */
static inline s32
tail_move_to_front(u8 *tail, u8 carry, u8 c)
{
	s32 r;
#if VEC_ENABLED
	vec_t vc = vec_dup(c), prev = vec_dup(carry);
	for (s32 k = 0;; k++) {
		vec_t cur = vec_ld(tail + 16 * k);
		vec_t shifted = vec_shift1(prev, cur);
		u64 m = vec_mask(vec_eq(cur, vc));
		if (m) {
			s32 lane = (s32)(bsf64(m) >> VEC_SHIFT);
			vec_st(tail + 16 * k,
			       vec_sel_upto((unsigned)lane, shifted, cur));
			r = 16 + 16 * k + lane;
			break;
		}
		vec_st(tail + 16 * k, shifted);
		prev = cur;
	}
#else
	u64 vc = 0x0101010101010101ULL * c, in = carry;
	for (s32 k = 0;; k++) {
		u64 w;
		memcpy(&w, tail + 8 * k, 8);
		if (!CPU_IS_LITTLE_ENDIAN())
			w = bswap64(w);
		u64 shifted = (w << 8) | in, z = zero_bytes(w ^ vc);
		if (z) {
			u64 upto = (z << 1) - 1;
			w ^= (w ^ shifted) & upto;
			if (!CPU_IS_LITTLE_ENDIAN())
				w = bswap64(w);
			memcpy(tail + 8 * k, &w, 8);
			r = 16 + 8 * k + (s32)(bsf64(z) >> 3);
			break;
		}
		if (!CPU_IS_LITTLE_ENDIAN())
			shifted = bswap64(shifted);
		memcpy(tail + 8 * k, &shifted, 8);
		in = w >> 56;
	}
#endif
	return r;
}

/*
 * 0x80 in the lowest zero byte of x, and perhaps in bytes above it, where the
 * borrow reaches.  A list word xor c has at most one zero byte (the list holds
 * each byte in use once, and the filler is not in use), so the lowest flag is
 * the match, and z ^ (z - 1) masks the bytes up to it.  It keeps the chain
 * from one symbol's head to the next two operations shorter than zero_bytes()
 * would.
 */
static forceinline u64
borrow_zero_bytes(u64 x)
{
	return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
}

/*
 * Moves c to the front of the list and codes its rank.  The list is entries
 * 0..7 in head (entry 0 in the low byte), 8..15 in head1 and 16..255 in tail;
 * the head words are locals whose address is never taken, so compilers keep
 * them in registers.  Ranks below 16 take no branch: at most one of z0 and z1
 * is nonzero, z0 ^ (z0 - 1) is all ones when z0 is 0 (head then shifts up
 * whole, its last entry going to head1), and z1 >> 7 lets one bit scan find
 * either flag.
 */
#define MTF_SYMBOL(c)                                                          \
	do {                                                                   \
		u8 c_ = (c);                                                   \
		u64 cc_ = 0x0101010101010101ULL * c_;                          \
		u64 z0_ = borrow_zero_bytes(head ^ cc_);                       \
		u64 z1_ = borrow_zero_bytes(head1 ^ cc_);                      \
		s32 r_;                                                        \
		if (likely(z0_ | z1_)) {                                       \
			u64 m1_ = (z1_ ^ (z1_ - 1)) & ((u64)0 - (z1_ != 0));   \
			r_ = (s32)(bsf64(z0_ | z1_ >> 7) >> 3) +               \
			     8 * (z0_ == 0);                                   \
			head1 ^=                                               \
				(head1 ^ ((head1 << 8) | (head >> 56))) & m1_; \
			head ^= (head ^ ((head << 8) | c_)) &                  \
				(z0_ ^ (z0_ - 1));                             \
		} else {                                                       \
			r_ = tail_move_to_front(tail, (u8)(head1 >> 56), c_);  \
			head1 = (head1 << 8) | (head >> 56);                   \
			head = (head << 8) | c_;                               \
		}                                                              \
		mtfv[wr++] = (u16)(r_ + 1);                                    \
		mtf_freq[r_ + 1]++;                                            \
	} while (0)

s32
ADD_SUFFIX(bzng_mtf)(const u8 *L, s32 n, const u8 *in_use, u16 *mtfv,
		     s32 *mtf_freq)
{
	u8 list[256 + 16];
	s32 n_in_use = 0;
	/* No branch per byte: an irregular alphabet would mispredict. */
	for (int i = 0; i < 256; i++) {
		list[n_in_use] = (u8)i;
		n_in_use += in_use[i];
	}
	/*
	 * Unused slots must never match a symbol: fill with a byte not in use
	 * (there is one whenever there are unused slots).
	 */
	u8 filler = 0;
	for (int i = 0; i < 256; i++)
		if (!in_use[i]) {
			filler = (u8)i;
			break;
		}
	for (int i = n_in_use; i < 256 + 16; i++)
		list[i] = filler;
	u64 head = get_unaligned_le64(list),
	    head1 = get_unaligned_le64(list + 8);
	_Alignas(16) u8 tail[256];
	memcpy(tail, list + 16, 240);
	memset(tail + 240, filler, 16);

	s32 eob = n_in_use + 1;
	for (s32 i = 0; i <= eob; i++)
		mtf_freq[i] = 0;
	s32 wr = 0;
	u32 z_pend = 0;
	if (n == 0)
		goto done;

	/* Position 0 compares with the list head (seq 0), the others with
	 * L[i-1]. */
	if (L[0] == (u8)head)
		z_pend = 1;
	else
		MTF_SYMBOL(L[0]);

	s32 i = 1;
#if VEC_ENABLED
	s32 cur = 1; /* first position not yet accounted for */
	for (; i + 64 <= n; i += 64) {
		u64 m = change_mask64(L + i);
		while (m) {
			s32 pos = i + (s32)bsf64(m);
			m &= m - 1;
			wr = put_zero_run(mtfv, wr, mtf_freq,
					  z_pend + (u32)(pos - cur));
			z_pend = 0;
			MTF_SYMBOL(L[pos]);
			cur = pos + 1;
		}
	}
	z_pend += (u32)(i - cur);
#endif
	for (; i < n; i++) {
		u8 c = L[i];
		if (c == L[i - 1]) {
			z_pend++;
			continue;
		}
		wr = put_zero_run(mtfv, wr, mtf_freq, z_pend);
		z_pend = 0;
		MTF_SYMBOL(c);
	}
	wr = put_zero_run(mtfv, wr, mtf_freq, z_pend);
done:
	mtfv[wr++] = (u16)eob;
	mtf_freq[eob]++;
	return wr;
}

/* ---- least rotation ------------------------------------------------------ */

/*
 * Two-pointer minimum-rotation search over candidate start positions, with
 * periodicity read off its exit (a minimal rotation is never skipped, so for
 * T = u^m the two pointers end on two equal rotations and the prefix
 * comparison reaches n; for primitive T it never does).
 *
 * Vector tiers seed the candidates with the positions of the smallest 2-byte
 * window (one vector pass for the minimum, one for the positions), then
 * refine them byte by byte while that keeps shrinking the set. The scalar
 * tier uses the smallest byte instead (memchr scan when sparse, the same
 * refinement when dense), since without a vector unit the 2-byte seeding pass
 * costs more than it saves.
 */
#define LROT_DENSE_DIV	32
#define LROT_SPARSE_DIV 1024
#define LROT_KMAX	64

/*
 * Length of the common prefix of tt+a and tt+b, starting the comparison at k
 * and capped at n (bytes beyond tt[2n) are padding).
 */
static inline s32
common_prefix(const u8 *tt, s32 a, s32 b, s32 k, s32 n)
{
#if VEC_X86 && defined(__AVX2__)
	while (k < n) {
		u32 m = ~(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
			_mm256_loadu_si256((const __m256i *)(tt + a + k)),
			_mm256_loadu_si256((const __m256i *)(tt + b + k))));
		if (m) {
			k += bsf64(m);
			return k < n ? k : n;
		}
		k += 32;
	}
	return n;
#elif VEC_ENABLED
	while (k < n) {
		u64 m = ~vec_mask(vec_eq(vec_ld(tt + a + k),
					 vec_ld(tt + b + k))) &
			VEC_FULL;
		if (m) {
			k += (s32)(bsf64(m) >> VEC_SHIFT);
			return k < n ? k : n;
		}
		k += 16;
	}
	return n;
#else
	while (k + 8 <= n) {
		u64 x = le64_bswap(load_u64_unaligned(tt + a + k) ^
				   load_u64_unaligned(tt + b + k));
		if (x)
			return k + (s32)(bsf64(x) >> 3);
		k += 8;
	}
	while (k < n && tt[a + k] == tt[b + k])
		k++;
	return k;
#endif
}

#if !VEC_ENABLED
/*
 * First position >= pos holding c0, or n (libc memchr is vectorised on every
 * mainstream platform).
 */
static inline s32
next_cand(const u8 *tt, s32 pos, s32 n, u8 c0)
{
	if (pos >= n)
		return n;
	const u8 *p = memchr(tt + pos, c0, (size_t)(n - pos));
	return p ? (s32)(p - tt) : n;
}

/* Smallest byte and its count over tt[0..n). */
static u8
min_byte(const u8 *tt, s32 n, s32 *count)
{
	u8 cmin = 255;
	for (s32 x = 0; x < n; x++)
		cmin = tt[x] < cmin ? tt[x] : cmin;
	s32 cnt = 0;
	for (s32 x = 0; x < n; x++)
		cnt += tt[x] == cmin;
	*count = cnt;
	return cmin;
}

/* Two-pointer search over all positions holding c0. */
static s32
lrot_sparse(const u8 *tt, s32 n, u8 c0, int *periodic)
{
	s32 i = next_cand(tt, 0, n, c0);
	s32 j = next_cand(tt, i + 1, n, c0);
	while (i < n && j < n) {
		s32 k = common_prefix(tt, i, j, 1, n);
		if (k >= n) {
			*periodic = 1;
			break;
		}
		if (tt[i + k] > tt[j + k])
			i = next_cand(tt, i + k + 1, n, c0);
		else
			j = next_cand(tt, j + k + 1, n, c0);
		if (i == j)
			j = next_cand(tt, j + 1, n, c0);
	}
	return i < j ? i : j;
}
#endif

/*
 * Refines a sorted candidate list cand[0..m) whose members all share their
 * first k bytes, whose (k+1)-th byte has minimum cm, then runs the
 * two-pointer search over the list (list indices only move forward).
 */
static s32
lrot_refine(const u8 *tt, s32 n, s32 *cand, s32 m, s32 k, u8 cm, int *periodic)
{
	s32 stalled = 0;
	s64 budget = 2 * (s64)n - m;
	while (m > n / LROT_SPARSE_DIV && k < LROT_KMAX && budget > 0) {
		s32 w = 0;
		u8 next = 255;
		for (s32 i = 0; i < m; i++) {
			s32 pos = cand[i];
			int hit = tt[pos + k] == cm;
			cand[w] = pos;
			w += hit;
			u8 f = hit ? tt[pos + k + 1] : 255;
			next = f < next ? f : next;
		}
		budget -= m;
		stalled = w < m - m / 16 ? 0 : stalled + 1;
		m = w;
		cm = next;
		k++;
		if (stalled >= 2)
			break;
	}
	if (m == 1)
		return cand[0];
	s32 a = 0, b = 1;
	while (a < m && b < m) {
		s32 i = cand[a], j = cand[b];
		s32 kk = common_prefix(tt, i, j, k, n);
		if (kk >= n) {
			*periodic = 1;
			break;
		}
		if (tt[i + kk] > tt[j + kk]) {
			s32 lim = i + kk + 1;
			while (a < m && cand[a] < lim)
				a++;
		} else {
			s32 lim = j + kk + 1;
			while (b < m && cand[b] < lim)
				b++;
		}
		if (a == b)
			b++;
	}
	s32 pi = a < m ? cand[a] : n, pj = b < m ? cand[b] : n;
	return pi < pj ? pi : pj;
}

s32
ADD_SUFFIX(bzng_least_rotation)(const u8 *tt, s32 n, s32 *cand, int *periodic)
{
	*periodic = 0;
	if (n <= 1)
		return 0;
	s32 m = 0;
	u8 cm = 255;
#if VEC_ENABLED
	/* Smallest big-endian 2-byte window over all n positions. */
	s32 x = 0;
#  if VEC_NEON
	uint16x8_t vm = vdupq_n_u16(0xffff);
	for (; x + 16 <= n; x += 16) {
		uint8x16_t a = vld1q_u8(tt + x), b = vld1q_u8(tt + x + 1);
		vm = vminq_u16(vm, vreinterpretq_u16_u8(vzip1q_u8(b, a)));
		vm = vminq_u16(vm, vreinterpretq_u16_u8(vzip2q_u8(b, a)));
	}
	u16 wmin = vminvq_u16(vm);
#  elif defined(__AVX2__)
	__m256i vm = _mm256_set1_epi8(-1);
	for (; x + 32 <= n; x += 32) {
		__m256i a = _mm256_loadu_si256((const __m256i *)(tt + x)),
			b = _mm256_loadu_si256((const __m256i *)(tt + x + 1));
		vm = _mm256_min_epu16(vm, _mm256_unpacklo_epi8(b, a));
		vm = _mm256_min_epu16(vm, _mm256_unpackhi_epi8(b, a));
	}
	__m128i m128 = _mm_min_epu16(_mm256_castsi256_si128(vm),
				     _mm256_extracti128_si256(vm, 1));
	u16 wmin = (u16)_mm_cvtsi128_si32(_mm_minpos_epu16(m128));
#  else
	__m128i vm = _mm_set1_epi8(-1);
	for (; x + 16 <= n; x += 16) {
		__m128i a = vec_ld(tt + x), b = vec_ld(tt + x + 1);
		vm = _mm_min_epu16(vm, _mm_unpacklo_epi8(b, a));
		vm = _mm_min_epu16(vm, _mm_unpackhi_epi8(b, a));
	}
	u16 wmin = (u16)_mm_cvtsi128_si32(_mm_minpos_epu16(vm));
#  endif
	for (; x < n; x++) {
		u16 w = (u16)(tt[x] << 8 | tt[x + 1]);
		wmin = w < wmin ? w : wmin;
	}
	const u8 c0 = (u8)(wmin >> 8), c1 = (u8)wmin;

	/* Its positions, and the smallest byte after them. */
	x = 0;
	vec_t v0 = vec_dup(c0), v1 = vec_dup(c1);
	for (; x + 16 <= n; x += 16) {
		vec_t eq = vec_and(vec_eq(vec_ld(tt + x), v0),
				   vec_eq(vec_ld(tt + x + 1), v1));
		u64 mk = vec_mask(eq) & VEC_LANE_LSB;
		while (mk) {
			s32 pos = x + (s32)(bsf64(mk) >> VEC_SHIFT);
			mk &= mk - 1;
			cand[m++] = pos;
			u8 f = tt[pos + 2];
			cm = f < cm ? f : cm;
		}
	}
	for (; x < n; x++) {
		int hit = tt[x] == c0 && tt[x + 1] == c1;
		cand[m] = x;
		m += hit;
		u8 f = hit ? tt[x + 2] : 255;
		cm = f < cm ? f : cm;
	}
	return lrot_refine(tt, n, cand, m, 2, cm, periodic);
#else
	s32 count;
	u8 c0 = min_byte(tt, n, &count);
	if (count <= n / LROT_DENSE_DIV)
		return lrot_sparse(tt, n, c0, periodic);
	for (s32 x = 0; x < n; x++) {
		cand[m] = x;
		int hit = tt[x] == c0;
		m += hit;
		u8 f = hit ? tt[x + 1] : 255;
		cm = f < cm ? f : cm;
	}
	return lrot_refine(tt, n, cand, m, 1, cm, periodic);
#endif
}

/* ---- decoder: symbol loop ------------------------------------------------ */

/*
 * Huffman decoding, inverse MTF and RUNA/RUNB of one block, resumable at
 * symbol boundaries. Vector tiers keep the first 16 entries of the MTF list
 * in a register (the rest in s->mtf[16..]); the scalar tier moves the whole
 * list in memory.
 */

#if VEC_ENABLED
/*
 * The most symbols in use for which mtf_tail_move rewrites the whole tail on
 * every move: past this a block's moves are mostly deep and spread (binary
 * data), and the loop up to the moved entry costs less than the rewrite.
 */
#  define MTF_FULL_TAIL_MAX 160

/*
 * For a list whose first 16 entries are held in the register head: removes
 * m[nn] (nn >= 16), shifts m[16..nn-1] up by one, puts head's last entry at
 * m[16] and returns the removed byte. If tail_limit is zero the loop stops at
 * nn's vector, and m needs 16 bytes of slack. Otherwise (the block's
 * n_in_use, at most MTF_FULL_TAIL_MAX, rounded up to a multiple of 16) every
 * vector of m[16..tail_limit) is rewritten whatever nn is, so the loop runs
 * the same number of times on every move of a block: one that stopped at
 * nn's vector would mispredict its exit on most moves of a block whose ranks
 * spread over several vectors. The first case is the fall-through because
 * its blocks make far more tail moves per byte (Silesia's mozilla about
 * 690,000 per MB, QUAL about 13,000).
 */
static inline u8
mtf_tail_move(u8 *m, u32 nn, u32 tail_limit, vec_t head)
{
	const u8 c = m[nn];
	vec_t carry = head; /* only its last lane is used */
	if (likely(!tail_limit)) {
		u32 j = 16;
		for (; j + 16 <= nn; j += 16) {
			const vec_t v = vec_ld(m + j);
			vec_st(m + j, vec_shift1(carry, v));
			carry = v;
		}
		const vec_t v = vec_ld(m + j);
		vec_st(m + j, vec_sel_upto(nn - j, vec_shift1(carry, v), v));
		return c;
	}
	for (u32 j = 16; j < tail_limit; j += 16) {
		const vec_t v = vec_ld(m + j);
		vec_st(m + j, vec_sel_upto_at(j, nn, vec_shift1(carry, v), v));
		carry = v;
	}
	return c;
}

/*
 * The run/literal selects of SYMBOL_STEP: IS_RUN is 1 for RUNA and RUNB
 * (symbols 0 and 1) and 0 for any other symbol, KEEP_LIT is v for a literal
 * and 0 for a run symbol, KEEP_RUN the reverse.  clang compiles the
 * comparison and the conditional expressions to conditional moves.  gcc
 * turns the conditional expressions into branches on the run/literal
 * pattern, which is close to random, so for gcc they are mask arithmetic;
 * clang is slower with the masks on x86.  For gcc the flag is the sign of
 * sym - 2 (symbols are below 2^31) rather than a comparison: gcc 13 writes a
 * comparison's result into the low byte of the register that holds the
 * previous symbol's output byte, which makes every symbol wait for the
 * previous one's vector shuffle.
 */
#  if GCC_PREREQ(1, 0)
#    define IS_RUN(sym)		(((sym) - 2u) >> 31)
#    define KEEP_LIT(is_run, v) ((v) & ((is_run) - 1u))
#    define KEEP_RUN(is_run, v) ((v) & (0u - (is_run)))
#  else
#    define IS_RUN(sym)		((sym) < 2)
#    define KEEP_LIT(is_run, v) ((is_run) ? 0 : (v))
#    define KEEP_RUN(is_run, v) ((is_run) ? (v) : 0)
#  endif

/*
 * Processes one decoded symbol sym_ on the symbol-loop locals (L, nmax, nb,
 * run, rshift, eob, head, tail_limit) and sets res_ to 0, BZNG_EOB or
 * BZ_DATA_ERROR (libbz2's checks at this symbol: N >= 2^21, or the block
 * growing past 100,000 x digit). RUNA/RUNB accumulate a run length in
 * bijective base 2; any other symbol first flushes a pending run of the
 * list's front byte.
 * Run and literal symbols take the same branch-free path: a pending run is
 * flushed with 16-byte stores of the front byte (length 0 for a run symbol),
 * the literal is moved to the front (index 0, a no-op, for a run symbol) and
 * stored, and nb advances only for a literal. Stores past nb land in the
 * array's slack. A macro rather than a function so the state stays in
 * registers in both loops that use it.
 */
#  define SYMBOL_STEP(sym_, res_)                                             \
	  do {                                                                \
		  const u32 sy_ = (sym_);                                     \
		  (res_) = 0;                                                 \
		  if (unlikely(sy_ == eob)) {                                 \
			  if (run) {                                          \
				  if (nb + run > nmax) {                      \
					  (res_) = BZ_DATA_ERROR;             \
					  break;                              \
				  }                                           \
				  memset(L + nb, vec_lane0(head), run);       \
				  nb += run;                                  \
				  run = 0;                                    \
				  rshift = 0;                                 \
			  }                                                   \
			  (res_) = BZNG_EOB;                                  \
			  break;                                              \
		  }                                                           \
		  const u32 is_run_ = IS_RUN(sy_);                            \
		  const u32 flush_ = KEEP_LIT(is_run_, run);                  \
		  if (unlikely((is_run_ & (rshift >= 21)) ||                  \
			       nb + flush_ + !is_run_ > nmax)) {              \
			  (res_) = BZ_DATA_ERROR;                             \
			  break;                                              \
		  }                                                           \
		  const vec_t front_ = vec_bcast_lane(head, 0);               \
		  vec_st(L + nb, front_);                                     \
		  if (unlikely(flush_ > 16))                                  \
			  for (u32 j_ = 16; j_ < flush_; j_ += 16)            \
				  vec_st(L + nb + j_, front_);                \
		  nb += flush_;                                               \
		  const u32 idx_ = KEEP_LIT(is_run_, sy_ - 1);                \
		  u8 c_;                                                      \
		  if (likely(idx_ < 16)) {                                    \
			  const vec_t cv_ = vec_bcast_lane(head, idx_);       \
			  head = vec_sel_upto(idx_, vec_shift1(cv_, head),    \
					      head);                          \
			  c_ = vec_lane0(cv_);                                \
		  } else {                                                    \
			  c_ = mtf_tail_move(s->mtf, idx_, tail_limit, head); \
			  head = vec_shift1(vec_dup(c_), head);               \
		  }                                                           \
		  L[nb] = c_;                                                 \
		  nb += !is_run_;                                             \
		  run = KEEP_RUN(is_run_, run + ((sy_ + 1) << rshift));       \
		  rshift = KEEP_RUN(is_run_, rshift + 1);                     \
	  } while (0)
#else
/* Portable version of the step above with the whole list in memory. */
#  define SYMBOL_STEP(sym_, res_)                         \
	  do {                                            \
		  const u32 sy_ = (sym_);                 \
		  (res_) = 0;                             \
		  if (sy_ <= 1) {                         \
			  if (rshift >= 21) {             \
				  (res_) = BZ_DATA_ERROR; \
				  break;                  \
			  }                               \
			  run += (sy_ + 1) << rshift;     \
			  rshift++;                       \
			  break;                          \
		  }                                       \
		  if (run) {                              \
			  if (nb + run > nmax) {          \
				  (res_) = BZ_DATA_ERROR; \
				  break;                  \
			  }                               \
			  memset(L + nb, s->mtf[0], run); \
			  nb += run;                      \
			  run = 0;                        \
			  rshift = 0;                     \
		  }                                       \
		  if (sy_ == eob) {                       \
			  (res_) = BZNG_EOB;              \
			  break;                          \
		  }                                       \
		  if (nb >= nmax) {                       \
			  (res_) = BZ_DATA_ERROR;         \
			  break;                          \
		  }                                       \
		  const u32 nn_ = sy_ - 1;                \
		  const u8 c_ = s->mtf[nn_];              \
		  memmove(s->mtf + 1, s->mtf, nn_);       \
		  s->mtf[0] = c_;                         \
		  L[nb++] = c_;                           \
	  } while (0)
#endif

/*
 * The symbol loop from the current symbol boundary. Whole groups take the
 * fast path while BZNG_FAST_IN_MARGIN input bytes remain; otherwise symbols
 * are decoded one at a time from the bits actually available, exactly as
 * libbz2 consumes them. Both use SYMBOL_STEP on the same register-resident
 * state. Returns BZNG_EOB at the end of the block's symbols, BZNG_NEED when
 * input ran out (all of it consumed), or BZ_DATA_ERROR at libbz2's error
 * position.
 */
int
ADD_SUFFIX(bzng_symbols)(bzng_dstate *s, bzng_bits *br)
{
	u8 *const L = s->L;
	const u32 nmax = s->nblock_max, eob = s->eob;
	u32 nb = s->nblock, run = s->run, rshift = s->run_shift;
#if VEC_ENABLED
	vec_t head = vec_ld(s->mtf);
	/* a literal's index is below n_in_use */
	const u32 tail_limit = s->n_in_use <= MTF_FULL_TAIL_MAX ?
				       ((u32)s->n_in_use + 15) & ~15u :
				       0;
#endif
	int result;
	for (;;) {
		/* fast groups */
		while (s->group_pos == 0 &&
		       BZNG_SPAN(br->p, br->end) >= BZNG_FAST_IN_MARGIN &&
		       s->group_no + 1 < s->n_selectors) {
			const bzng_htable *t =
				&s->tab[s->selector[s->group_no + 1]];
			const bzng_bits group_start = *br;
			u16 syms[BZ_G_SIZE];
			const int ns = bzng_fast_group(br, t, syms, eob);
			if (ns < 0)
				break; /* the exact path below finds libbz2's
					  error */
			s->group_no++;
			s->group_pos = BZ_G_SIZE - ns;
			for (int k = 0; k < ns; k++) {
				SYMBOL_STEP(syms[k], result);
				if (unlikely(result)) {
					if (result == BZ_DATA_ERROR) {
						/* libbz2 stops right after this
						 * symbol */
						*br = group_start;
						bzng_skip_codes(br, t, k + 1);
					}
					goto out;
				}
			}
		}
		/* one symbol, exactly as libbz2 consumes it */
		if (s->group_pos == 0) {
			s->group_no++;
			if (s->group_no >= s->n_selectors) {
				result = BZ_DATA_ERROR;
				goto out;
			}
			s->group_pos = BZ_G_SIZE;
		}
		const bzng_htable *t = &s->tab[s->selector[s->group_no]];
		if (br->cnt < 24)
			bzng_br_fill(br, 24);
		u32 sym, len;
		const int d =
			bzng_decode_exact(t, br->buf, br->cnt, &sym, &len);
		if (d == 0) {
			result = BZNG_NEED;
			goto out;
		}
		if (d < 0) {
			bzng_br_take(br, BZNG_INVALID_BITS);
			result = BZ_DATA_ERROR;
			goto out;
		}
		bzng_br_take(br, len);
		s->group_pos--;
		SYMBOL_STEP(sym, result);
		if (result)
			goto out;
	}
out:
#if VEC_ENABLED
	vec_st(s->mtf, head);
#endif
	ASSERT(nb <= nmax);
	s->nblock = nb;
	s->run = run;
	s->run_shift = rshift;
	return result;
}

#undef SYMBOL_STEP
#undef IS_RUN
#undef KEEP_LIT
#undef KEEP_RUN

/* ---- decoder: bulk un-RLE1 ----------------------------------------------- */

/*
 * See bzng_unrle_io (dec.h).  Each step tests UNRLE_STEP candidate run starts
 * at once: candidate k is set when in[k..k+3] are equal, and the first set
 * candidate after a clean state is a true run start (an earlier equal byte
 * would have set an earlier one).
 *
 * The steps store whole words or vectors: the candidate step stores
 * UNRLE_STEP bytes and advances by fewer when it finds a run, and a run is
 * filled in whole vectors.  So a step may leave bytes stored past out, at most
 * `stray` of them (UNRLE_STRAY, or UNRLE_WIDE_FILL in the wide stage), and
 * the loop runs only while the rest of the block is sure to produce that
 * much, so that its output overwrites them: after a step at most
 * UNRLE_STEP + 4 of the R text bytes counted at its start are used, and the
 * R' >= R - UNRLE_STEP - 4 bytes left decode to at least R' - (R' / 5 + 1)
 * bytes (at most one in five text bytes is a run's count byte), which is at
 * least `stray` once R >= UNRLE_MIN_TEXT(stray).  If the caller's buffer
 * fills first, the stores were inside it and are overwritten too.
 *
 * The vector tiers start with a wide stage, which runs while the buffer and
 * the block's text have room for the longest run in whole vectors and fills
 * every run with that many: a loop or a branch on the run's length would
 * mispredict on most runs of run-dense text, and the fixed stores cost less.
 */
#if VEC_ENABLED
#  define UNRLE_STEP  ((u32)sizeof(vec_t))
#  define UNRLE_SHIFT VEC_SHIFT
/* Bytes a narrow step may leave stored past out: unrle_fill's two vectors. */
#  define UNRLE_STRAY (2 * UNRLE_STEP)
/* The longest run, 4 + 255 bytes, rounded up to whole vectors. */
#  define UNRLE_WIDE_FILL \
	  ((4u + 255u + UNRLE_STEP - 1) / UNRLE_STEP * UNRLE_STEP)
/* Candidate mask of in[0..UNRLE_STEP), storing those bytes to out. */
static inline u64
unrle_candidates(const u8 *in, u8 *out)
{
	const vec_t v0 = vec_ld(in), v1 = vec_ld(in + 1), v2 = vec_ld(in + 2),
		    v3 = vec_ld(in + 3);
	vec_st(out, v0);
	return vec_mask(vec_and(vec_and(vec_eq(v0, v1), vec_eq(v0, v2)),
				vec_eq(v0, v3)));
}
/*
 * Writes total (4..259) copies of c to out, which has room for
 * max(total, UNRLE_STRAY) bytes: two vectors, then for a longer run whole
 * vectors up to one that ends at out + total.
 */
static inline void
unrle_fill(u8 *out, u8 c, u32 total)
{
	const vec_t cv = vec_dup(c);
	vec_st(out, cv);
	vec_st(out + UNRLE_STEP, cv);
	if (total > 2 * UNRLE_STEP) {
		for (u32 j = 2 * UNRLE_STEP; j + UNRLE_STEP < total;
		     j += UNRLE_STEP)
			vec_st(out + j, cv);
		vec_st(out + total - UNRLE_STEP, cv);
	}
}
/*
 * Writes UNRLE_WIDE_FILL copies of c to out, enough for any run, whatever the
 * run's length.
 */
static inline void
unrle_fill_wide(u8 *out, u8 c)
{
	const vec_t cv = vec_dup(c);
	UNROLL_FULL
	for (u32 j = 0; j < UNRLE_WIDE_FILL; j += UNRLE_STEP)
		vec_st(out + j, cv);
}
#else
#  define UNRLE_STEP  8u
#  define UNRLE_SHIFT 3
/* The candidate store's bytes; the run fill writes exactly total bytes. */
#  define UNRLE_STRAY UNRLE_STEP
static inline u64
unrle_candidates(const u8 *in, u8 *out)
{
	const u64 a = load_u64_unaligned(in), b = load_u64_unaligned(in + 1),
		  c = load_u64_unaligned(in + 2),
		  d = load_u64_unaligned(in + 3);
	memcpy(out, &a, 8);
	return le64_bswap(zero_bytes(a ^ b) & zero_bytes(a ^ c) &
			  zero_bytes(a ^ d));
}
static inline void
unrle_fill(u8 *out, u8 c, u32 total)
{
	memset(out, c, total);
}
#endif

/*
 * Remaining block text that guarantees `stray` more output bytes after one
 * more step: R' - (R' / 5 + 1) >= stray needs R' >= 5 * stray / 4 + 2, and
 * R = R' + UNRLE_STEP + 4.
 */
#define UNRLE_MIN_TEXT(stray) (UNRLE_STEP + 4 + (5 * (stray) + 3) / 4 + 2)

/*
 * The least in_len at which a step may run, given the text after the piece:
 * a step reads in[0..UNRLE_STEP + 4), and the piece and the text after it
 * must hold min_text bytes.
 */
static inline u32
unrle_floor(u32 text_after, u32 min_text)
{
	return text_after >= min_text - (UNRLE_STEP + 4) ?
		       UNRLE_STEP + 4 :
		       min_text - text_after;
}

void
ADD_SUFFIX(bzng_unrle)(bzng_unrle_io *io)
{
	const u8 *p = io->in;
	u32 plen = io->in_len;
	u8 *out = io->out;
	u32 avail = io->avail;
	const u8 *lit = p;
	u32 min_len;
#if VEC_ENABLED
	min_len = unrle_floor(io->text_after, UNRLE_MIN_TEXT(UNRLE_WIDE_FILL));
	while (plen >= min_len && avail >= UNRLE_STEP + UNRLE_WIDE_FILL) {
		const u64 bits = unrle_candidates(p, out);
		if (bits == 0) {
			p += UNRLE_STEP;
			out += UNRLE_STEP;
			plen -= UNRLE_STEP;
			avail -= UNRLE_STEP;
			continue;
		}
		const u32 k = bsf64(bits) >> UNRLE_SHIFT;
		const u8 c = p[k];
		const u32 total = 4u + p[k + 4];
		p += k + 5;
		plen -= k + 5;
		lit = p;
		unrle_fill_wide(out + k, c);
		out += k + total;
		avail -= k + total;
	}
#endif
	min_len = unrle_floor(io->text_after, UNRLE_MIN_TEXT(UNRLE_STRAY));
	while (plen >= min_len && avail >= UNRLE_STEP) {
		const u64 bits = unrle_candidates(p, out);
		if (bits == 0) {
			p += UNRLE_STEP;
			out += UNRLE_STEP;
			plen -= UNRLE_STEP;
			avail -= UNRLE_STEP;
			continue;
		}
		const u32 k = bsf64(bits) >> UNRLE_SHIFT;
		p += k;
		out += k;
		plen -= k;
		avail -= k;
		const u8 c = p[0];
		const u32 total = 4u + p[4];
		p += 5;
		plen -= 5;
		lit = p;
		if (avail >= total + UNRLE_STRAY) {
			unrle_fill(out, c, total);
			out += total;
			avail -= total;
		} else {
			const u32 k2 = total < avail ? total : avail;
			memset(out, c, k2);
			out += k2;
			avail -= k2;
			io->pend_len = total - k2;
			io->pend_ch = c;
			break;
		}
	}
	/* trailing equal literals carry into the next step */
	io->eq = 0;
	if (p != lit) {
		const u8 b = p[-1];
		u32 e = 1;
		while (e < 3 && p - e > lit && p[-1 - (ptrdiff_t)e] == b)
			e++;
		io->last = b;
		io->eq = (u8)e;
	}
	ASSERT(out <= io->out + io->avail && avail <= io->avail);
	io->in = p;
	io->in_len = plen;
	io->out = out;
	io->avail = avail;
}

#undef UNRLE_STEP
#undef UNRLE_SHIFT
#undef UNRLE_STRAY
#undef UNRLE_WIDE_FILL
#undef UNRLE_MIN_TEXT
