/*
 * sort_libsais.c - the block sorter on libsais
 *
 * Part of bz2-ng; MIT licence (see LICENSE). This file compiles libsais
 * (third_party/libsais/, by Ilya Grebnov), which is licensed under
 * Apache-2.0 WITH LLVM-exception: see licenses/libsais-1a696e6.txt.
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
 * (docs/spec.md section 4.3): each level's free space minus its
 * length never decreases, and its alphabet is smaller than its length. Patch
 * 0001 keeps the LMS positions in that free space only where what it leaves
 * the recursion satisfies the same argument. The work array leaves at least
 * floor(n/2). libsais's malloc returns NULL here, so if that bound were ever
 * wrong a sort would fail with an internal error instead of allocating
 * behind the caller's bzalloc.
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sort.h"

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
	 * The suffix array, at least floor(n/2) of free space, and the
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
	 * The suffix array at the start of work, the context's buckets at its
	 * end, and everything between them as libsais's free space, which
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
	s32 *SA = work;
	if (libsais_ctx(&ctx, T, SA, n, (s32)fs, freq) != 0)
		return -2;
	/* T is a Lyndon word exactly when suffix 0 is the least suffix. */
	if (SA[0] != 0)
		return -3;

	/*
	 * Suffix order is rotation order, so row i ends with the byte before
	 * suffix SA[i]. The bytes go to the free space first, since L may be T.
	 */
	u8 *bwt = (u8 *)(SA + n);
	s32 row = 0;
	bwt[0] = T[n - 1];
	for (s32 i = 1; i < n; i++) {
		s32 s = SA[i];
		bwt[i] = T[s - 1];
		row = s == q ? i : row;
	}
	memcpy(L, bwt, (size_t)n);
	return row;
}
