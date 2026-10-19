/*
 * sort_recursion.h - the sorter's recursion below a repetitive block
 *
 * Derived from sais.c for sais-lite
 * Copyright (c) 2008-2010 Yuta Mori All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * ALTERED for bz2-ng: sais-lite 2.4.1's recursion (SA-IS by Nong, Zhang and
 * Chan) on a string of 32-bit names, for sort_libsais.c, which includes this
 * file before libsais and whose patched libsais calls bzng_rec_sort_main for
 * the reduced string of a repetitive block (patches/0003). Changes from
 * sais-lite: no allocation (all scratch is the free space after SA); the
 * symbol width is fixed; each scan keeps the current bucket's pointer in a
 * register and writes it back when the bucket changes; suffix
 * classification, LMS compaction and naming and the LMS-substring lengths
 * select instead of branching; LMS substrings are named during their
 * induction where the free space allows; the list of LMS positions is kept
 * through the recursion when there is room. This is the recursion of the
 * project's own earlier block sorter, for 32-bit names and register bucket
 * pointers only.
 *
 * Free space: a level of length n with alphabet k needs k <= fs, and every
 * level has fs >= n - 1 >= k. The first level has it because libsais gives
 * its own recursion that much (docs/spec.md section 4.3). A level with m LMS
 * positions (m <= n/2) passes its recursion, of length m, either
 * fs + n - 2m, so that the recursion's fs - n is (fs - n) + (2n - 3m), no
 * less than the level's own, or, when it keeps its LMS list (sais_reduce),
 * fs + n - used_tail - 3m, which it does only when that is at least m.
 */
#ifndef LIB_SORT_RECURSION_H
#define LIB_SORT_RECURSION_H

#include <string.h>

#include "lib_common.h"

/*
 * Forms chosen per compiler and target, measured on Apple M4, Zen 5, Granite
 * Rapids and Neoverse V2. The output is the same either way.
 *
 * BZNG_REC_BRANCHLESS_NAMING: the naming scans select and store
 * unconditionally; faster on Apple's cores, slower on the servers.
 * BZNG_REC_MASK_SELECTS: selects as mask arithmetic, which gcc otherwise
 * compiles as branches. BZNG_REC_SUFFIX0_APART: the final scans take the
 * entry of suffix 1 on a path of its own, so the common path reads the
 * predecessor unguarded.
 */
#if GCC_PREREQ(1, 0)
#  define BZNG_REC_GCC 1
#else
#  define BZNG_REC_GCC 0
#endif
#ifndef BZNG_REC_BRANCHLESS_NAMING
#  if defined(__APPLE__) && defined(ARCH_ARM64)
#    define BZNG_REC_BRANCHLESS_NAMING 1
#  else
#    define BZNG_REC_BRANCHLESS_NAMING 0
#  endif
#endif
#ifndef BZNG_REC_MASK_SELECTS
#  define BZNG_REC_MASK_SELECTS BZNG_REC_GCC
#endif
#ifndef BZNG_REC_SUFFIX0_APART
#  if defined(__APPLE__) && defined(ARCH_ARM64) && !BZNG_REC_GCC
#    define BZNG_REC_SUFFIX0_APART 0
#  else
#    define BZNG_REC_SUFFIX0_APART 1
#  endif
#endif

/*
 * REC_SEL(cond, a, b) is a when cond (0 or 1) is set, else b; REC_MARK_IF
 * complements v when cond is set.
 */
#if BZNG_REC_MASK_SELECTS
#  define REC_SEL(cond, a, b)  ((b) ^ (((a) ^ (b)) & -(s32)(cond)))
#  define REC_MARK_IF(cond, v) ((v) ^ -(s32)(cond))
#else
#  define REC_SEL(cond, a, b)  ((cond) ? (a) : (b))
#  define REC_MARK_IF(cond, v) ((cond) ? ~(v) : (v))
#endif

/*
 * The current bucket's pointer b lives in a register and is written back to
 * B when the scan moves to another bucket (a branch per step, mispredicted
 * at every change, so it suits repetitive strings).
 */
#define REC_BUCKET_BEGIN() (b = SA + B[c1])
#define REC_BUCKET_SWITCH(e)                   \
	do {                                   \
		if ((e) != c1) {               \
			B[c1] = (s32)(b - SA); \
			b = SA + B[c1 = c0];   \
		}                              \
	} while (0)
#define REC_PUT_INC(v) (*b++ = (v))
#define REC_PUT_DEC(v) (*--b = (v))

/*
 * Name-group bookkeeping of the naming scans: an entry j >= n starts a new
 * group of equal LMS substrings (REC_GROUP_START), and the first suffix a
 * group writes to a bucket from a source of type t gets the +n mark
 * (REC_GROUP_FIRST).
 */
#if BZNG_REC_BRANCHLESS_NAMING
#  define REC_GROUP_START(j, d)             \
	  do {                              \
		  const s32 f_ = n <= (j);  \
		  (d) += f_;                \
		  (j) -= REC_SEL(f_, n, 0); \
	  } while (0)
#  define REC_GROUP_FIRST(j, t, d)                   \
	  do {                                       \
		  (j) += REC_SEL(D[t] != (d), n, 0); \
		  D[t] = (d);                        \
	  } while (0)
#else
#  define REC_GROUP_START(j, d)     \
	  do {                      \
		  if (n <= (j)) {   \
			  (d) += 1; \
			  (j) -= n; \
		  }                 \
	  } while (0)
#  define REC_GROUP_FIRST(j, t, d)    \
	  do {                        \
		  if (D[t] != (d)) {  \
			  (j) += n;   \
			  D[t] = (d); \
		  }                   \
	  } while (0)
#endif

static s32 bzng_rec_sort_main(const s32 *T, s32 *SA, s32 fs, s32 n, s32 k);

/* Symbol counts of T[0..n) over [0, k) into C. */
static void
bzng_rec_get_counts(const s32 *T, s32 *C, s32 n, s32 k)
{
	for (s32 i = 0; i < k; ++i)
		C[i] = 0;
	for (s32 i = 0; i < n; ++i)
		++C[T[i]];
}

/*
 * Symbol counts of T[0..n) over [0, k) into C, as get_counts, with the
 * increments spread over C and three more tables in scratch[0..3k): a
 * repetitive string increments one counter at nearby positions, and with a
 * single table each such increment waits for the store of the one before.
 */
static void
bzng_rec_get_counts4(const s32 *T, s32 *C, s32 *scratch, s32 n, s32 k)
{
	s32 *C1 = scratch, *C2 = scratch + k, *C3 = scratch + 2 * k;
	s32 i;

	memset(C, 0, (size_t)k * sizeof(s32));
	memset(scratch, 0, 3 * (size_t)k * sizeof(s32));
	for (i = 0; i + 4 <= n; i += 4) {
		++C[T[i]];
		++C1[T[i + 1]];
		++C2[T[i + 2]];
		++C3[T[i + 3]];
	}
	for (; i < n; ++i)
		++C[T[i]];
	for (i = 0; i < k; ++i)
		C[i] += C1[i] + C2[i] + C3[i];
}

/* Bucket starts (end = 0) or ends (end = 1) from the counts C[0..k). */
static void
bzng_rec_get_buckets(const s32 *C, s32 *B, s32 k, int end)
{
	s32 sum = 0;
	if (end) {
		for (s32 i = 0; i < k; ++i) {
			sum += C[i];
			B[i] = sum;
		}
	} else {
		for (s32 i = 0; i < k; ++i) {
			sum += C[i];
			B[i] = sum - C[i];
		}
	}
}

/*
 * Sorts the LMS substrings by induction, naming them in the same passes:
 * lms_sort2_l is the L-scan plus moving the group marks to the other end of
 * each group, lms_sort2_s the S-scan. D[2c + t] holds the pass counter d of
 * the last suffix written to bucket c from a source of type t, and an entry
 * gets +n when it starts a new name group. Seeds must be in place with the
 * group starts already marked. Returns the pass counter for the S-scan.
 */
static s32
bzng_rec_lms_sort2_l(const s32 *T, s32 *SA, const s32 *C, s32 *B, s32 *D, s32 n,
		     s32 k)
{
	s32 *b = NULL, i, j, t, d;
	s32 c0, c1;

	bzng_rec_get_buckets(C, B, k, 0);
	j = n - 1;
	c1 = T[j];
	REC_BUCKET_BEGIN();
	--j;
	t = (T[j] < c1);
	j += n;
	REC_PUT_INC((t & 1) ? ~j : j);
	for (i = 0, d = 0; i < n; ++i) {
		if (0 < (j = SA[i])) {
			REC_GROUP_START(j, d);
			REC_BUCKET_SWITCH(c0 = T[j]);
			--j;
			t = c0;
			t = (t << 1) | (T[j] < c1);
			REC_GROUP_FIRST(j, t, d);
			REC_PUT_INC(REC_MARK_IF(t & 1, j));
			SA[i] = 0;
		} else if (j < 0) {
			SA[i] = ~j;
		}
	}
	for (i = n - 1; 0 <= i; --i) {
		if (0 < SA[i]) {
			if (SA[i] < n) {
				SA[i] += n;
				for (j = i - 1; SA[j] < n; --j) {
				}
				SA[j] -= n;
				i = j;
			}
		}
	}
	return d;
}

static void
bzng_rec_lms_sort2_s(const s32 *T, s32 *SA, const s32 *C, s32 *B, s32 *D, s32 n,
		     s32 k, s32 d)
{
	s32 *b = NULL, i, j, t;
	s32 c0, c1;

	bzng_rec_get_buckets(C, B, k, 1);
	c1 = 0;
	REC_BUCKET_BEGIN();
	for (i = n - 1, d += 1; 0 <= i; --i) {
		if (0 < (j = SA[i])) {
			REC_GROUP_START(j, d);
			REC_BUCKET_SWITCH(c0 = T[j]);
			--j;
			t = c0;
			t = (t << 1) | (T[j] > c1);
			REC_GROUP_FIRST(j, t, d);
			REC_PUT_DEC(REC_SEL(t & 1, ~(j + 1), j));
			SA[i] = 0;
		}
	}
}

/*
 * Compacts the sorted LMS substrings into SA[0..m) and, when names are not
 * unique, stores name + 1 of the LMS substring at p in SA[m + p/2]. Returns
 * the number of names. After lms_sort2 the slots hold 0 or a marked
 * (negative) LMS substring; a non-negative slot writes a placeholder to SA[d]
 * that the next marked one overwrites, so the loop has no data-dependent
 * branch.
 */
static s32
bzng_rec_lms_postproc2(s32 *SA, s32 n, s32 m)
{
	s32 i, j, d, name;

	for (i = 0, d = 0, name = 0; d < m; ++i) {
		s32 v = SA[i], neg = v < 0;
		j = ~v;
		name += neg & (j >= n);
		SA[i] = 0;
		SA[d] = j;
		d += neg;
	}
	if (name < m) {
		for (i = m - 1, d = name + 1; 0 <= i; --i) {
			j = SA[i];
			s32 f = j >= n;
			j -= f ? n : 0;
			d -= f;
			SA[m + (j >> 1)] = d;
		}
	} else {
		for (i = 0; i < m; ++i) {
			j = SA[i];
			SA[i] = j >= n ? j - n : j;
		}
	}
	return name;
}

/*
 * Sorts the LMS substrings by induction without naming them (used when the
 * free space has no room for lms_sort2's D array).
 */
static void
bzng_rec_lms_sort1(const s32 *T, s32 *SA, s32 *C, s32 *B, s32 n, s32 k)
{
	s32 *b = NULL, i, j;
	s32 c0, c1;

	if (C == B)
		bzng_rec_get_counts(T, C, n, k);
	bzng_rec_get_buckets(C, B, k, 0);
	j = n - 1;
	c1 = T[j];
	REC_BUCKET_BEGIN();
	--j;
	REC_PUT_INC((T[j] < c1) ? ~j : j);
	for (i = 0; i < n; ++i) {
		if (0 < (j = SA[i])) {
			REC_BUCKET_SWITCH(c0 = T[j]);
			--j;
			REC_PUT_INC((T[j] < c1) ? ~j : j);
			SA[i] = 0;
		} else if (j < 0) {
			SA[i] = ~j;
		}
	}
	if (C == B)
		bzng_rec_get_counts(T, C, n, k);
	bzng_rec_get_buckets(C, B, k, 1);
	c1 = 0;
	REC_BUCKET_BEGIN();
	for (i = n - 1; 0 <= i; --i) {
		if (0 < (j = SA[i])) {
			REC_BUCKET_SWITCH(c0 = T[j]);
			--j;
			REC_PUT_DEC((T[j] > c1) ? ~(j + 1) : j);
			SA[i] = 0;
		}
	}
}

/*
 * Names the LMS substrings sorted by lms_sort1 by comparing neighbours.
 * Returns the number of names.
 */
static s32
bzng_rec_lms_postproc1(const s32 *T, s32 *SA, s32 n, s32 m)
{
	s32 i, j, p, q, plen, qlen, name;
	s32 c0, c1;
	int diff;

	for (i = 0; (p = SA[i]) < 0; ++i)
		SA[i] = ~p;
	if (i < m) {
		for (j = i, ++i;; ++i) {
			if ((p = SA[i]) < 0) {
				SA[j++] = ~p;
				SA[i] = 0;
				if (j == m)
					break;
			}
		}
	}
	/*
	 * Length of each LMS substring (from its LMS position to the next one,
	 * inclusive) into SA[m + p/2], one position per iteration with the type
	 * recurrence. A pair of positions 2k, 2k+1 holds at most one LMS
	 * position, so the slot is read back and rewritten unchanged for the
	 * other one.
	 */
	{
		s32 s1 = 0;
		j = n - 1;
		c1 = T[n - 1];
		for (i = n - 2; 0 <= i; --i) {
			c0 = T[i];
			s32 s0 = (c0 < c1) | ((c0 == c1) & s1);
			s32 lms = s1 & (s0 ^ 1);
			s32 *slot = SA + m + ((i + 1) >> 1);
			*slot = lms ? j - i : *slot;
			j = lms ? i + 1 : j;
			s1 = s0;
			c1 = c0;
		}
	}
	for (i = 0, name = 0, q = n, qlen = 0; i < m; ++i) {
		p = SA[i], plen = SA[m + (p >> 1)], diff = 1;
		if ((plen == qlen) && ((q + plen) < n)) {
			for (j = 0; (j < plen) && (T[p + j] == T[q + j]); ++j) {
			}
			if (j == plen)
				diff = 0;
		}
		if (diff != 0) {
			++name, q = p, qlen = plen;
		}
		SA[m + (p >> 1)] = name;
	}
	return name;
}

/* The full suffix array from the sorted LMS suffixes seeded in SA. */
static void
bzng_rec_induce_sa(const s32 *T, s32 *SA, s32 *C, s32 *B, s32 n, s32 k)
{
	s32 *b = NULL, i, j;
	s32 c0, c1;

	if (C == B)
		bzng_rec_get_counts(T, C, n, k);
	bzng_rec_get_buckets(C, B, k, 0);
	j = n - 1;
	c1 = T[j];
	REC_BUCKET_BEGIN();
	REC_PUT_INC(((0 < j) && (T[j - 1] < c1)) ? ~j : j);
	for (i = 0; i < n; ++i) {
		j = SA[i], SA[i] = ~j;
#if BZNG_REC_SUFFIX0_APART
		/*
		 * Entry 1 induces suffix 0, which has no predecessor; every
		 * other inducing entry reads T[j - 1] unguarded.
		 */
		if (1 < j) {
			--j;
			REC_BUCKET_SWITCH(c0 = T[j]);
			REC_PUT_INC(REC_MARK_IF(T[j - 1] < c1, j));
		} else if (j == 1) {
			REC_BUCKET_SWITCH(c0 = T[0]);
			REC_PUT_INC(0);
		}
#else
		if (0 < j) {
			--j;
			REC_BUCKET_SWITCH(c0 = T[j]);
			/* T[j - 1], or T[0] when j = 0 (then unused) */
			const s32 pv = T[j - (j > 0)];
			REC_PUT_INC(REC_MARK_IF((j > 0) & (pv < c1), j));
		}
#endif
	}
	if (C == B)
		bzng_rec_get_counts(T, C, n, k);
	bzng_rec_get_buckets(C, B, k, 1);
	c1 = 0;
	REC_BUCKET_BEGIN();
	for (i = n - 1; 0 <= i; --i) {
#if BZNG_REC_SUFFIX0_APART
		if (1 < (j = SA[i])) {
			--j;
			REC_BUCKET_SWITCH(c0 = T[j]);
			REC_PUT_DEC(REC_MARK_IF(T[j - 1] > c1, j));
		} else if (j == 1) {
			REC_BUCKET_SWITCH(c0 = T[0]);
			REC_PUT_DEC(~0);
		} else {
			SA[i] = ~j;
		}
#else
		if (0 < (j = SA[i])) {
			--j;
			REC_BUCKET_SWITCH(c0 = T[j]);
			const s32 pv = T[j - (j > 0)];
			REC_PUT_DEC(REC_MARK_IF((j == 0) | (pv > c1), j));
		} else {
			SA[i] = ~j;
		}
#endif
	}
}

/*
 * Stages 1 and 2 of SA-IS, then the sorted LMS suffixes placed at the ends
 * of their buckets (stage 3 seeding); the caller runs the final induction
 * with the returned C and B (equal pointers mean the counts must be
 * recomputed there). SA has n + fs entries, and the bucket arrays come from
 * the last fs. Unless it keeps its LMS list, a level hands its recursion
 * all the free space, buckets included, and recounts them afterwards.
 * Returns 0, or -2 if a level's buckets do not fit, which the free-space
 * argument at the top of this file rules out.
 */
static s32
bzng_rec_sais_reduce(const s32 *T, s32 *SA, s32 fs, s32 n, s32 k, s32 **Cp,
		     s32 **Bp)
{
	s32 *C, *B, *D, *RA, *b;
	s32 i, j, m, p, t, name, newfs;
	s32 c0, c1;
	int recount, lmssort2;
	s32 *lms_list = NULL; /* LMS positions in ascending order, if listed */
	s32 used_tail = 0;    /* ints of bucket arrays at the end of SA */

	if (2 * k <= fs) {
		C = SA + (n + fs - k);
		B = C - k;
		recount = 0;
	} else if (k <= fs) {
		C = B = SA + (n + fs - k);
		recount = 1;
	} else {
		return -2;
	}
	lmssort2 = !recount && (2 <= (n / k)) && (4 * k < fs);

	/*
	 * SA[0..n) is free until it is cleared below, and C and B lie beyond
	 * it, so a level with at least 8 entries per name counts with spare
	 * tables there.
	 */
	if (8 <= n / k)
		bzng_rec_get_counts4(T, C, SA, n, k);
	else
		bzng_rec_get_counts(T, C, n, k);
	bzng_rec_get_buckets(C, B, k, 1);
	memset(SA, 0, (size_t)n * sizeof(s32));
	/*
	 * Each LMS position i+1 reserves the next slot from the end of bucket
	 * T[i+1]; the slot reserved by the previous (right-hand) LMS receives
	 * j, the L-type position before that LMS. Suffix types follow the
	 * recurrence s(i) = T[i] < T[i+1] or (T[i] = T[i+1] and s(i+1)), one
	 * position per iteration with selects instead of branches.
	 */
	used_tail = lmssort2 ? 4 * k : recount ? k : 2 * k;
	if (fs - used_tail >= n / 2 + 1) {
		/*
		 * Two passes: the LMS positions into the free space just below
		 * the bucket arrays (found right to left, stored downwards, so
		 * they end up in ascending order), then the bucket slots for
		 * them, rightmost first.
		 */
		s32 *list_end = SA + n + fs - used_tail, s1 = 0;
		m = 0;
		c1 = T[n - 1];
		for (i = n - 2; 0 <= i; --i) {
			c0 = T[i];
			s32 s0 = (c0 < c1) | ((c0 == c1) & s1);
			list_end[-1 - m] = i + 1;
			m += s1 & (s0 ^ 1);
			s1 = s0;
			c1 = c0;
		}
		lms_list = list_end - m;
		b = &t;
		j = n;
		for (i = m - 1; 0 <= i; --i) {
			p = lms_list[i];
			*b = j;
			b = SA + --B[T[p]];
			j = p - 1;
		}
	} else {
		s32 dummy, s1 = 0;
		b = &t;
		j = n;
		m = 0;
		c1 = T[n - 1];
		for (i = n - 2; 0 <= i; --i) {
			c0 = T[i];
			s32 s0 = (c0 < c1) | ((c0 == c1) & s1);
			s32 lms = s1 & (s0 ^ 1);
			s32 *w = lms ? b : &dummy;
			*w = j;
			s32 nb = B[c1] - lms;
			B[c1] = nb;
			b = lms ? SA + nb : b;
			j = lms ? i : j;
			m += lms;
			s1 = s0;
			c1 = c0;
		}
	}

	if (1 < m) {
		if (lmssort2) {
			D = B - k * 2;
			++B[T[j + 1]];
			for (i = 0, j = 0; i < k; ++i) {
				j += C[i];
				if (B[i] != j)
					SA[B[i]] += n;
				D[i] = D[i + k] = 0;
			}
			s32 d = bzng_rec_lms_sort2_l(T, SA, C, B, D, n, k);
			bzng_rec_lms_sort2_s(T, SA, C, B, D, n, k, d);
			name = bzng_rec_lms_postproc2(SA, n, m);
		} else {
			bzng_rec_lms_sort1(T, SA, C, B, n, k);
			name = bzng_rec_lms_postproc1(T, SA, n, m);
		}
	} else if (m == 1) {
		*b = j + 1;
		name = 1;
	} else {
		name = 0;
	}

	/*
	 * Keep the LMS list through the recursion when the recursion still gets
	 * free space >= m (the first condition) and the reduced string, placed
	 * just below the list, stays clear of the names in SA[m, m + n/2) it is
	 * compacted from (the second). Otherwise the LMS positions are found
	 * again after the recursion, and the buckets, which lay in the
	 * recursion's space, are counted again.
	 */
	const int keep_list = lms_list != NULL &&
			      (s64)n + fs - used_tail >= 4 * (s64)m &&
			      (s64)n / 2 + fs - used_tail >= 3 * (s64)m;
	if (name < m) {
		if (keep_list) {
			/*
			 * [0, m + newfs) recursion | reduced string |
			 * LMS list | buckets
			 */
			newfs = n + fs - used_tail - 3 * m;
		} else {
			newfs = (n + fs) - (m * 2);
			recount = 2;
		}
		RA = SA + m + newfs;
		/*
		 * Unconditional stores: a slot is rewritten until a name lands
		 * in it.
		 */
		for (i = m + (n >> 1) - 1, j = m - 1; m <= i && 0 <= j; --i) {
			s32 v = SA[i];
			RA[j] = v - 1;
			j -= v != 0;
		}
		if (bzng_rec_sort_main(RA, SA, newfs, m, name) != 0)
			return -2;
		if (keep_list) {
			for (i = 0; i < m; ++i)
				SA[i] = lms_list[SA[i]];
		} else {
			/* RA[0..m) = the LMS positions, left to right. */
			s32 s1 = 0;
			j = m - 1;
			c1 = T[n - 1];
			for (i = n - 2; 0 <= i && 0 <= j; --i) {
				c0 = T[i];
				s32 s0 = (c0 < c1) | ((c0 == c1) & s1);
				RA[j] = i + 1;
				j -= s1 & (s0 ^ 1);
				s1 = s0;
				c1 = c0;
			}
			for (i = 0; i < m; ++i)
				SA[i] = RA[SA[i]];
		}
	}

	if (recount != 0)
		bzng_rec_get_counts(T, C, n, k);
	if (1 < m) {
		s32 q2;
		bzng_rec_get_buckets(C, B, k, 1);
		i = m - 1, j = n, p = SA[m - 1], c1 = T[p];
		do {
			q2 = B[c0 = c1];
			while (q2 < j)
				SA[--j] = 0;
			do {
				SA[--j] = p;
				if (--i < 0)
					break;
				p = SA[i];
			} while ((c1 = T[p]) == c0);
		} while (0 <= i);
		while (0 < j)
			SA[--j] = 0;
	}
	*Cp = C;
	*Bp = B;
	return 0;
}

/*
 * Suffix array of the name string T[0..n) over [0, k) into SA[0..n), with
 * SA[n..n + fs) as scratch and T untouched (one recursion level and
 * everything below it). Returns 0, or -2 if a level does not fit, which
 * the free-space argument above rules out.
 */
static s32
bzng_rec_sort_main(const s32 *T, s32 *SA, s32 fs, s32 n, s32 k)
{
	s32 *C, *B;
	if (bzng_rec_sais_reduce(T, SA, fs, n, k, &C, &B) != 0)
		return -2;
	bzng_rec_induce_sa(T, SA, C, B, n, k);
	return 0;
}

#undef REC_SEL
#undef REC_MARK_IF
#undef REC_BUCKET_BEGIN
#undef REC_BUCKET_SWITCH
#undef REC_PUT_INC
#undef REC_PUT_DEC
#undef REC_GROUP_START
#undef REC_GROUP_FIRST

#endif /* LIB_SORT_RECURSION_H */
