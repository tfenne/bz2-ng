/*
 * sort.h - the block sorter's interface
 *
 * The block sorter: the one interface between the encoder and the suffix
 * sorter, libsais (sort_libsais.c).
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */
#ifndef LIB_SORT_H
#define LIB_SORT_H

#include "lib_common.h"

/*
 * Ints of work array that bzng_bwt needs for a block of n bytes:
 * n + n/2 + 2048, sufficient for any input. They hold the suffix array, at
 * least floor(n/2) ints of free space for the sorter's recursion, and its
 * bucket counters.
 */
size_t bzng_bwt_work_ints(s32 n);

/*
 * Cyclic BWT of a Lyndon word.
 *
 * T[0..n) must be a Lyndon word (strictly smaller than each of its proper
 * rotations; the encoder guarantees this by rotating a primitive block to its
 * least rotation). For a Lyndon word the suffix order equals the rotation
 * order, so a suffix sorter with an implicit sentinel yields bzip2's rotation
 * order directly.
 *
 * Outputs: L[0..n), the cyclic BWT (L may equal T); freq[0..256), the byte
 * histogram of T (may be NULL); the return value, the row of rotation q
 * (0 <= q < n). work holds work_ints >= bzng_bwt_work_ints(n) ints of
 * scratch. No allocation and no global state.
 *
 * Errors: -1 bad arguments or work array too small, -2 the sorter failed
 * because it would have needed memory beyond work (cannot happen with the
 * advertised work size).
 */
s32 bzng_bwt(const u8 *T, u8 *L, s32 n, s32 q, s32 *freq, s32 *work,
	     size_t work_ints);

#endif /* LIB_SORT_H */
