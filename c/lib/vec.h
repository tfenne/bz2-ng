/*
 * vec.h - 16-byte vector operations for the kernel tiers
 *
 * 16-byte vector operations for the kernel tiers, implemented once with
 * AArch64 NEON and once with x86 SSE2..SSE4.1 intrinsics. Included only by
 * the tier translation units (kern_*.c via kern_template.h); which
 * implementation is compiled follows the architecture, and VEC_ENABLED says
 * whether any vector code is compiled at all (the scalar tier defines
 * KERN_NO_VECTORS to turn it off).
 *
 * Lane masks: vec_mask() packs a 0x00/0xff byte-lane mask into an integer
 * with VEC_MB bits per lane (4 on NEON, from shrn; 1 on x86, from movemask),
 * so lane k of a mask m is set iff bit VEC_MB*k is, and the first set lane is
 * ctz(m) >> VEC_SHIFT. VEC_FULL is the mask of all 16 lanes.
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */
#ifndef LIB_VEC_H
#define LIB_VEC_H

#include "lib_common.h"

#if !defined(KERN_NO_VECTORS) && defined(ARCH_ARM64)
#  include <arm_neon.h>
#  define VEC_ENABLED  1
#  define VEC_NEON     1
#  define VEC_X86      0
#  define VEC_MB       4
#  define VEC_SHIFT    2
#  define VEC_FULL     (~0ull)
#  define VEC_LANE_LSB 0x1111111111111111ull
typedef uint8x16_t vec_t;
/* Loads 16 bytes from p (any alignment). */
static inline vec_t
vec_ld(const u8 *p)
{
	return vld1q_u8(p);
}
/* Stores 16 bytes to p (any alignment). */
static inline void
vec_st(u8 *p, vec_t v)
{
	vst1q_u8(p, v);
}
/* c in every lane. */
static inline vec_t
vec_dup(u8 c)
{
	return vdupq_n_u8(c);
}
/* 0xff in the lanes where a and b are equal, else 0. */
static inline vec_t
vec_eq(vec_t a, vec_t b)
{
	return vceqq_u8(a, b);
}
/* Bitwise a & b. */
static inline vec_t
vec_and(vec_t a, vec_t b)
{
	return vandq_u8(a, b);
}
/* The lane mask of m (VEC_MB bits per lane). */
static inline u64
vec_mask(vec_t m)
{
	return vget_lane_u64(
		vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)),
		0);
}
/* (prev:cur) shifted up one lane: lane 0 = last lane of prev. */
static inline vec_t
vec_shift1(vec_t prev, vec_t cur)
{
	return vextq_u8(prev, cur, 15);
}
/* Lanes 0..k take a, the rest b (k <= 15). */
static inline vec_t
vec_sel_upto(unsigned k, vec_t a, vec_t b)
{
	static const u8 iota[16] = {0, 1, 2,  3,  4,  5,  6,  7,
				    8, 9, 10, 11, 12, 13, 14, 15};
	return vbslq_u8(vcleq_u8(vld1q_u8(iota), vdupq_n_u8((u8)k)), a, b);
}
/*
 * Lanes i with base + i <= k take a, the rest b (base <= 240, k <= 255): the
 * lanes of a list's entries base..base+15 at positions up to k.
 */
static inline vec_t
vec_sel_upto_at(unsigned base, unsigned k, vec_t a, vec_t b)
{
	static const u8 iota[16] = {0, 1, 2,  3,  4,  5,  6,  7,
				    8, 9, 10, 11, 12, 13, 14, 15};
	const vec_t pos = vaddq_u8(vld1q_u8(iota), vdupq_n_u8((u8)base));
	return vbslq_u8(vcleq_u8(pos, vdupq_n_u8((u8)k)), a, b);
}
/* Lane 0 of v. */
static inline u8
vec_lane0(vec_t v)
{
	return vgetq_lane_u8(v, 0);
}
/* Lane k (k <= 15) broadcast to every lane. */
static inline vec_t
vec_bcast_lane(vec_t v, unsigned k)
{
	return vqtbl1q_u8(v, vdupq_n_u8((u8)k));
}

#elif !defined(KERN_NO_VECTORS) && defined(ARCH_X86_64)
/*
 * A vector tier's translation unit must have SSE4.1 enabled (CMakeLists.txt
 * gives it -march=x86-64-v2 or -v3). cl has no SSE4.1 macro and compiles
 * these intrinsics in any file.
 */
#  if !defined(__SSE4_1__) && !(defined(_MSC_VER) && !defined(__clang__))
#    error "an x86 vector tier needs SSE4.1 enabled (-march=x86-64-v2)"
#  endif
#  include <immintrin.h>
#  define VEC_ENABLED  1
#  define VEC_NEON     0
#  define VEC_X86      1
#  define VEC_MB       1
#  define VEC_SHIFT    0
#  define VEC_FULL     0xffffull
#  define VEC_LANE_LSB 0xffffull
typedef __m128i vec_t;
/* Loads 16 bytes from p (any alignment). */
static inline vec_t
vec_ld(const u8 *p)
{
	return _mm_loadu_si128((const __m128i *)p);
}
/* Stores 16 bytes to p (any alignment). */
static inline void
vec_st(u8 *p, vec_t v)
{
	_mm_storeu_si128((__m128i *)p, v);
}
/* c in every lane. */
static inline vec_t
vec_dup(u8 c)
{
	return _mm_set1_epi8((char)c);
}
/* 0xff in the lanes where a and b are equal, else 0. */
static inline vec_t
vec_eq(vec_t a, vec_t b)
{
	return _mm_cmpeq_epi8(a, b);
}
/* Bitwise a & b. */
static inline vec_t
vec_and(vec_t a, vec_t b)
{
	return _mm_and_si128(a, b);
}
/* The lane mask of m (VEC_MB bits per lane). */
static inline u64
vec_mask(vec_t m)
{
	return (u32)_mm_movemask_epi8(m);
}
/* (prev:cur) shifted up one lane: lane 0 = last lane of prev. */
static inline vec_t
vec_shift1(vec_t prev, vec_t cur)
{
	return _mm_alignr_epi8(cur, prev, 15);
}
/* Lanes 0..k take a, the rest b (k <= 15). */
static inline vec_t
vec_sel_upto(unsigned k, vec_t a, vec_t b)
{
	const __m128i iota = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
					   12, 13, 14, 15);
	__m128i m = _mm_cmpgt_epi8(_mm_set1_epi8((char)(k + 1)),
				   iota); /* lanes 0..k */
	return _mm_blendv_epi8(b, a, m);
}
/*
 * Lanes i with base + i <= k take a, the rest b (base <= 240, k <= 255): the
 * lanes of a list's entries base..base+15 at positions up to k.
 */
static inline vec_t
vec_sel_upto_at(unsigned base, unsigned k, vec_t a, vec_t b)
{
	const __m128i pos =
		_mm_add_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
					   12, 13, 14, 15),
			     _mm_set1_epi8((char)base));
	const __m128i kv = _mm_set1_epi8((char)k);
	/* unsigned pos <= k: positions pass 127 */
	return _mm_blendv_epi8(b, a, _mm_cmpeq_epi8(_mm_max_epu8(pos, kv), kv));
}
/* Lane 0 of v. */
static inline u8
vec_lane0(vec_t v)
{
	return (u8)_mm_cvtsi128_si32(v);
}
/* Lane k (k <= 15) broadcast to every lane. */
static inline vec_t
vec_bcast_lane(vec_t v, unsigned k)
{
	return _mm_shuffle_epi8(v, _mm_set1_epi8((char)k));
}

#else
#  define VEC_ENABLED 0
#  define VEC_NEON    0
#  define VEC_X86     0
#endif

#endif /* LIB_VEC_H */
