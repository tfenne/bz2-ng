/*
 * sort_libsais.c - the block sorter on libsais
 *
 * Part of bz2-ng; MIT licence (see LICENSE). This file compiles libsais
 * (third_party/libsais/, by Ilya Grebnov), which is licensed under
 * Apache-2.0 WITH LLVM-exception: see licenses/libsais-1a696e6.txt, and
 * sort_recursion.h, derived from Yuta Mori's sais-lite (MIT): see
 * licenses/sais-lite-2.4.1.txt.
 */

/*
 * bzng_bwt (sort.h) on libsais. libsais, as the build patches it
 * (third_party/libsais/patches), is compiled into this translation unit so
 * that its public functions can be given internal linkage (a static library
 * then defines no libsais symbol to clash with an application's own copy),
 * its single-thread context can be built here with its bucket counters in the
 * caller's work array, and malloc and free can be replaced for it alone.
 *
 * During a sort libsais allocates only in its recursion, a buffer of k ints
 * at a level whose alphabet k exceeds both 1000 and the level's free space.
 * Following the free space down the recursion shows that free space of at
 * least floor(n/2) - 2 at the top rules that out for every input of length n
 * (docs/spec.md section 4.3): each level's free space minus its length never
 * decreases, and its alphabet is smaller than its length. Patches 0001 and
 * 0004 keep the LMS positions in that free space, as a list or as a bitmap,
 * only where what they leave the recursion satisfies the same argument, and
 * patch 0003's recursion for repetitive blocks (sort_recursion.h) needs at
 * each level only free space for the level's alphabet, which it has at every
 * level given the free space libsais gives its own recursion (see that file).
 * The work array leaves at least floor(n/2). libsais's malloc returns NULL
 * here, so if that bound were ever wrong a sort would fail with an internal
 * error instead of allocating behind the caller's bzalloc.
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sort.h"
#include "sort_recursion.h"

/*
 * Entries of the reduced string whose LMS positions bzng_rec_repetitive
 * counts: enough for a stable density, few enough to cost nothing next to
 * the sort.
 */
#define BZNG_REC_SAMPLE 8192

/*
 * Whether the reduced string Tr[0..m) of a block, over names distinct names,
 * goes to sort_recursion.h's recursion rather than libsais's (patch 0003).
 * That recursion keeps each scan's bucket pointer in a register, which wins
 * on strings as repetitive as CRAM's MC:Z tags and loses on text, binary and
 * the other tag and quality data measured. What sets those blocks apart is a
 * reduced string with few distinct names (at most 1 in 8 of its entries;
 * MC:Z blocks have at most 0.11) whose own LMS positions are nearly as dense
 * as they can be (more than 0.42 of its entries; at least 0.44 in every
 * MC:Z block measured, at most 0.41 in every other block of the corpora,
 * whose reduced strings mostly lie between 0.19 and 0.37). The density is
 * taken over the first BZNG_REC_SAMPLE entries, the last of them counted as
 * L-type. Decided from the block alone, so a block always takes the same
 * path.
 *
 * The number of LMS positions is bounded first, in one pass with no
 * dependency from entry to entry, which compilers vectorise. An LMS position
 * i has Tr[i - 1] > Tr[i] <= Tr[i + 1], and every strict valley,
 * Tr[i - 1] > Tr[i] < Tr[i + 1], is one; the two counts differ only by runs
 * of equal names after a step down, and they settle the rule on every block
 * of the corpora. Otherwise the positions are counted exactly, each entry's
 * type following the next entry's through a run.
 */
static int
bzng_rec_repetitive(const s32 *Tr, s32 m, s32 names)
{
	if ((s64)names * 8 > m)
		return 0;
	s32 len = m < BZNG_REC_SAMPLE ? m : BZNG_REC_SAMPLE;
	s64 threshold = (s64)len * 21;
	s32 valleys = 0, at_most = 0;
	for (s32 i = 1; i < len - 1; i++) {
		s32 down = Tr[i - 1] > Tr[i];
		valleys += down & (Tr[i] < Tr[i + 1]);
		at_most += down & (Tr[i] <= Tr[i + 1]);
	}
	if ((s64)valleys * 50 > threshold)
		return 1;
	if ((s64)at_most * 50 <= threshold)
		return 0;

	s32 lms = 0, s1 = 0;
	for (s32 i = len - 2; i >= 0; i--) {
		s32 s0 = Tr[i] == Tr[i + 1] ? s1 : Tr[i] < Tr[i + 1];
		lms += s1 & (s0 ^ 1);
		s1 = s0;
	}
	return (s64)lms * 50 > threshold;
}

/*
 * Ints of the bitmap of the LMS positions of a block of n bytes that patch
 * 0004 keeps through the recursion where patch 0001's list does not fit: a
 * bit per position, in whole machine words.
 */
static s32
bzng_lms_bitmap_ints(s32 n)
{
	return (n / WORDBITS + 1) * (WORDBYTES / 4);
}

/*
 * Sets the bit of position p in the bitmap whose word *word is being built in
 * *w, first storing that word if p lies in a later one. The words are stored
 * little-endian, so that position p is bit p % 8 of byte p / 8.
 */
static forceinline void
bzng_lms_bitmap_mark(u8 *bitmap, machine_word_t *w, u32 *word, u32 p)
{
	if (p / WORDBITS != *word) {
		put_unaligned_leword(*w, &bitmap[*word * WORDBYTES]);
		*word = p / WORDBITS;
		*w = 0;
	}
	*w |= (machine_word_t)1 << (p % WORDBITS);
}

/*
 * Marks the m ascending LMS positions lms[0..m) in the bitmap of ints ints
 * (bzng_lms_bitmap_ints), which need only be 4-byte aligned. Four positions
 * that end in the word being built, and so lie in it, are set together; a
 * group that crosses into a later word is marked one by one.
 */
static void
bzng_lms_bitmap_store(const s32 *lms, s32 m, s32 *bitmap, s32 ints)
{
	/* Positions are not negative: unsigned arithmetic divides by shifts. */
	const u32 *pos = (const u32 *)lms;
	u8 *bytes = (u8 *)bitmap;
	machine_word_t w = 0;
	u32 word = 0;
	s32 j = 0;

	memset(bitmap, 0, (size_t)ints * sizeof(bitmap[0]));
	for (; j + 4 <= m; j += 4) {
		if (pos[j + 3] / WORDBITS == word) {
			w |= ((machine_word_t)1 << (pos[j] % WORDBITS)) |
			     ((machine_word_t)1 << (pos[j + 1] % WORDBITS)) |
			     ((machine_word_t)1 << (pos[j + 2] % WORDBITS)) |
			     ((machine_word_t)1 << (pos[j + 3] % WORDBITS));
			continue;
		}
		for (s32 k = j; k < j + 4; k++)
			bzng_lms_bitmap_mark(bytes, &w, &word, pos[k]);
	}
	for (; j < m; j++)
		bzng_lms_bitmap_mark(bytes, &w, &word, pos[j]);
	put_unaligned_leword(w, &bytes[word * WORDBYTES]);
}

/*
 * Writes the positions marked in the bitmap of ints ints, ascending, to out.
 * Each half byte stores its four table entries whatever its count, so out
 * needs room for 4 entries beyond the positions marked.
 */
static void
bzng_lms_bitmap_load(const s32 *bitmap, s32 ints, s32 *out)
{
	/* The set bits of each half byte, in order, and their count. */
	static const s32 nibble_bits[16][4] = {
		{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0},
		{2, 0, 0, 0}, {0, 2, 0, 0}, {1, 2, 0, 0}, {0, 1, 2, 0},
		{3, 0, 0, 0}, {0, 3, 0, 0}, {1, 3, 0, 0}, {0, 1, 3, 0},
		{2, 3, 0, 0}, {0, 2, 3, 0}, {1, 2, 3, 0}, {0, 1, 2, 3},
	};
	static const u8 nibble_count[16] = {0, 1, 1, 2, 1, 2, 2, 3,
					    1, 2, 2, 3, 2, 3, 3, 4};
	const u8 *bytes = (const u8 *)bitmap;

	for (s32 i = 0; i < ints * 4; i++) {
		u32 lo = bytes[i] & 15, hi = bytes[i] >> 4;

		for (int k = 0; k < 4; k++)
			out[k] = 8 * i + nibble_bits[lo][k];
		out += nibble_count[lo];
		for (int k = 0; k < 4; k++)
			out[k] = 8 * i + 4 + nibble_bits[hi][k];
		out += nibble_count[hi];
	}
}

/*
 * Largest block whose final induction, once bzng_rec_repetitive has judged
 * it repetitive, is sort_recursion.h's (patch 0006). On x86-64 the register
 * scans gained on MC:Z blocks of up to 64 KiB on every machine measured but
 * lost to libsais's on 900 kB blocks on Granite Rapids; 2^17 covers every
 * block of a 64 KiB stream, which RLE1 lengthens by at most a quarter. On
 * AArch64 they gained at every size.
 */
#ifndef BZNG_REC_FINAL_MAX
#  ifdef ARCH_X86_64
#    define BZNG_REC_FINAL_MAX (1 << 17)
#  else
#    define BZNG_REC_FINAL_MAX INT32_MAX
#  endif
#endif

/*
 * libsais's public functions, declared static before libsais.h declares
 * them: its declarations and the definitions then inherit internal linkage
 * (C11 6.2.2), and a prototype that no longer matches is a compile error.
 */
static void *libsais_create_ctx(void);
static void libsais_free_ctx(void *ctx);
static int32_t libsais(const uint8_t *T, int32_t *SA, int32_t n, int32_t fs,
		       int32_t *freq);
static int32_t libsais_gsa(const uint8_t *T, int32_t *SA, int32_t n, int32_t fs,
			   int32_t *freq);
static int32_t libsais_int(int32_t *T, int32_t *SA, int32_t n, int32_t k,
			   int32_t fs);
static int32_t libsais_ctx(const void *ctx, const uint8_t *T, int32_t *SA,
			   int32_t n, int32_t fs, int32_t *freq);
static int32_t libsais_gsa_ctx(const void *ctx, const uint8_t *T, int32_t *SA,
			       int32_t n, int32_t fs, int32_t *freq);
static int32_t libsais_bwt(const uint8_t *T, uint8_t *U, int32_t *A, int32_t n,
			   int32_t fs, int32_t *freq);
static int32_t libsais_bwt_aux(const uint8_t *T, uint8_t *U, int32_t *A,
			       int32_t n, int32_t fs, int32_t *freq, int32_t r,
			       int32_t *I);
static int32_t libsais_bwt_ctx(const void *ctx, const uint8_t *T, uint8_t *U,
			       int32_t *A, int32_t n, int32_t fs,
			       int32_t *freq);
static int32_t libsais_bwt_aux_ctx(const void *ctx, const uint8_t *T,
				   uint8_t *U, int32_t *A, int32_t n,
				   int32_t fs, int32_t *freq, int32_t r,
				   int32_t *I);
static void *libsais_unbwt_create_ctx(void);
static void libsais_unbwt_free_ctx(void *ctx);
static int32_t libsais_unbwt(const uint8_t *T, uint8_t *U, int32_t *A,
			     int32_t n, const int32_t *freq, int32_t i);
static int32_t libsais_unbwt_ctx(const void *ctx, const uint8_t *T, uint8_t *U,
				 int32_t *A, int32_t n, const int32_t *freq,
				 int32_t i);
static int32_t libsais_unbwt_aux(const uint8_t *T, uint8_t *U, int32_t *A,
				 int32_t n, const int32_t *freq, int32_t r,
				 const int32_t *I);
static int32_t libsais_unbwt_aux_ctx(const void *ctx, const uint8_t *T,
				     uint8_t *U, int32_t *A, int32_t n,
				     const int32_t *freq, int32_t r,
				     const int32_t *I);
static int32_t libsais_plcp(const uint8_t *T, const int32_t *SA, int32_t *PLCP,
			    int32_t n);
static int32_t libsais_plcp_gsa(const uint8_t *T, const int32_t *SA,
				int32_t *PLCP, int32_t n);
static int32_t libsais_plcp_int(const int32_t *T, const int32_t *SA,
				int32_t *PLCP, int32_t n);
static int32_t libsais_lcp(const int32_t *PLCP, const int32_t *SA, int32_t *LCP,
			   int32_t n);

/*
 * The allocator libsais sees. A test build defines BZNG_SAIS_MALLOC to one
 * that aborts, so that a block on which the free-space bound failed would
 * fail the test rather than return an internal error.
 */
#ifndef BZNG_SAIS_MALLOC
/* Gives libsais no memory: a sort that wanted some fails instead. */
static void *
bzng_sais_malloc(size_t size)
{
	(void)size;
	return NULL;
}
#  define BZNG_SAIS_MALLOC bzng_sais_malloc
#endif

/* libsais frees only what its malloc returned, so this is never reached. */
static void
bzng_sais_free(void *ptr)
{
	(void)ptr;
}

#define malloc(size) BZNG_SAIS_MALLOC(size)
#define free(ptr)    bzng_sais_free(ptr)

/*
 * libsais consults __has_builtin only where it is defined and otherwise knows
 * gcc's builtins by version. The stand-in that common_defs.h defines for
 * compilers without it (gcc before 10) answers 0 for __builtin_prefetch, and
 * libsais stops with an error without a prefetch, so it is withdrawn here.
 */
#ifdef BZNG_HAS_BUILTIN_STANDIN
#  undef __has_builtin
#endif

/*
 * Without the prefetches of libsais's recursion scans (patch 0005) on
 * AArch64, where the M4 and Graviton4 measured faster without them
 * (docs/spec.md section 4.3); x86-64 keeps them.
 */
#if defined(__aarch64__) || defined(_M_ARM64)
#  define LIBSAIS_RECURSION_PREFETCH 0
#endif

/* Most of libsais's functions are not used here. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4505) /* unreferenced static function */
#endif
/* The patched copy in the build directory, on this file's include path. */
#include "libsais.c"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif

#ifdef BZNG_HAS_BUILTIN_STANDIN
#  define __has_builtin(builtin) 0
#endif

#undef malloc
#undef free

/*
 * Ints of a context's bucket counters (the array libsais_create_ctx would
 * allocate).
 */
#define SAIS_CTX_BUCKET_INTS (8 * ALPHABET_SIZE)

/* Ints of work that bzng_bwt needs for n bytes (sort.h). */
size_t
bzng_bwt_work_ints(s32 n)
{
	/*
	 * libsais's n entries, at least floor(n/2) of free space, and the
	 * context's buckets.
	 */
	return (size_t)n + (size_t)n / 2 + SAIS_CTX_BUCKET_INTS;
}

/*
 * The cyclic BWT of the Lyndon word T[0..n) into L, its byte histogram into
 * freq, and the row of rotation q (sort.h).
 */
s32
bzng_bwt(const u8 *T, u8 *L, s32 n, s32 q, s32 *freq, s32 *work,
	 size_t work_ints)
{
	if (T == NULL || L == NULL || work == NULL || n < 1 || q < 0 ||
	    q >= n || work_ints < bzng_bwt_work_ints(n))
		return -1;
	if (n == 1) {
		if (freq != NULL) {
			memset(freq, 0, 256 * sizeof(freq[0]));
			freq[T[0]] = 1;
		}
		L[0] = T[0];
		return 0;
	}

	/*
	 * libsais's n entries at the start of work, the context's buckets at
	 * its end, and everything between them as libsais's free space, which
	 * satisfies the bound above; a single-thread context is no more than
	 * its bucket counters.
	 */
	size_t fs = work_ints - SAIS_CTX_BUCKET_INTS - (size_t)n;
	if (fs > (size_t)INT32_MAX - (size_t)n)
		fs = (size_t)INT32_MAX - (size_t)n;
	s32 *buckets = work + (work_ints - SAIS_CTX_BUCKET_INTS);
	LIBSAIS_CONTEXT ctx = {
		.buckets = buckets,
		.thread_state = NULL,
		.threads = 1,
	};
	/*
	 * libsais's BWT mode with patch 0002: the final induction leaves in
	 * each row's entry the byte before that row's suffix, stores the row of
	 * suffix q in row and returns the row of suffix 0.
	 */
	s32 *A = work;
	s32 row = -1;
	s32 first = libsais_main_ctx(&ctx, T, A, n,
				     LIBSAIS_FLAGS_BWT | LIBSAIS_FLAGS_BWT_ROW,
				     q, &row, (s32)fs, freq);
	if (first < 0 || row < 0)
		return -2;
	/* T is a Lyndon word exactly when suffix 0 is the least suffix. */
	if (first != 0)
		return -3;

	/*
	 * Suffix order is rotation order, so row 0, suffix 0's, ends with
	 * T[n - 1], and every other row's entry already holds its last byte.
	 * The induction has finished with T, so L may be T.
	 */
	L[0] = T[n - 1];
	libsais_bwt_copy_8u(L + 1, A + 1, n - 1);
	return row;
}
