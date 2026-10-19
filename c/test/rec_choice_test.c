/*
 * rec_choice_test.c - the sorter's recursion choice against its rule
 *
 * lib/sort_libsais.c compiled again so that its bzng_rec_repetitive, which
 * picks the recursion for a block's reduced string (patch 0003), can be
 * called on generated strings. The output of a sort is the same whichever
 * recursion runs, so no other test sees this choice; here it must equal the
 * rule written out with an array of types: at most one name per 8 entries,
 * and more than 0.42 of the first BZNG_REC_SAMPLE entries LMS positions,
 * the last of those entries L-type. Linked into the test program, the
 * file's definitions of bzng_bwt and bzng_bwt_work_ints take the place of
 * the library's.
 *
 * Prints one line per family and a final "TEST ... failures=N" line; exits
 * non-zero on any failure.
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>

#include "sort_libsais.c"

#define MAX_M (BZNG_REC_SAMPLE + 2000)

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t
rng(void)
{
	uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static uint32_t
rnd(uint32_t bound)
{
	return (uint32_t)(rng() % bound);
}

static s32 Tr[MAX_M];
static unsigned char is_s[MAX_M];
static long cases, failures;

/* The LMS positions of the window Tr[0..len), from its types. */
static s32
window_lms(s32 len)
{
	s32 lms = 0;
	is_s[len - 1] = 0;
	for (s32 i = len - 2; i >= 0; i--)
		is_s[i] = Tr[i] < Tr[i + 1] ||
			  (Tr[i] == Tr[i + 1] && is_s[i + 1]);
	for (s32 i = 1; i < len; i++)
		lms += is_s[i] && !is_s[i - 1];
	return lms;
}

static int
rule(s32 m, s32 names)
{
	if ((s64)names * 8 > m)
		return 0;
	s32 len = m < BZNG_REC_SAMPLE ? m : BZNG_REC_SAMPLE;
	return (s64)window_lms(len) * 50 > (s64)len * 21;
}

/*
 * Checks Tr[0..m) with names just inside and just outside one per 8, and
 * returns the rule's answer for the first.
 */
static int
check(const char *family, s32 m)
{
	int want = 0;
	for (s32 names = m / 8; names <= m / 8 + 1; names++) {
		int expect = rule(m, names);
		int got = bzng_rec_repetitive(Tr, m, names);
		if (names == m / 8)
			want = expect;
		cases++;
		if (got != expect) {
			failures++;
			if (failures <= 20)
				fprintf(stderr,
					"FAIL family=%s m=%d names=%d: "
					"chose %d, rule %d\n",
					family, m, names, got, expect);
		}
	}
	return want;
}

/*
 * Pieces whose mixture sets the window's LMS density and how far it lies
 * from the strict valleys and from the down steps not followed by a down
 * step: "2 0" (a strict valley), "2 0 0" (an LMS run that is no strict
 * valley), "3 1 1 0" (a run after a down step that is no LMS position, then
 * a strict valley) and "1 2 2 3 0" (a run after an up step, S-type
 * throughout, then a strict valley). Each piece starts above where the last
 * one ends.
 */
#define N_PIECES 4

static void
fill_pieces(s32 m, const uint32_t weight[N_PIECES])
{
	static const s32 piece[N_PIECES][5] = {
		{2, 0}, {2, 0, 0}, {3, 1, 1, 0}, {1, 2, 2, 3, 0}};
	static const s32 piece_len[N_PIECES] = {2, 3, 4, 5};
	uint32_t total = 0;
	for (int p = 0; p < N_PIECES; p++)
		total += weight[p];
	s32 i = 0;
	while (i < m) {
		uint32_t r = rnd(total);
		int p = 0;
		while (r >= weight[p])
			r -= weight[p++];
		for (s32 j = 0; j < piece_len[p] && i < m; j++)
			Tr[i++] = piece[p][j];
	}
}

/*
 * Fills Tr[0..m) with lms LMS positions: strict valleys "2 0", the first
 * runs of them instead "3 1 1 0" (kind 0: a run after a down step that is
 * no LMS position, then a strict valley) or "2 0 0" (kind 1: an LMS
 * position that is no strict valley), then a rising tail.
 */
static void
fill_threshold(s32 m, s32 lms, s32 runs, int kind)
{
	static const s32 run_piece[2][4] = {{3, 1, 1, 0}, {2, 0, 0}};
	static const s32 run_len[2] = {4, 3};
	s32 i = 0;
	for (s32 v = 0; v < lms; v++) {
		if (v < runs) {
			for (s32 j = 0; j < run_len[kind]; j++)
				Tr[i++] = run_piece[kind][j];
		} else {
			Tr[i++] = 2;
			Tr[i++] = 0;
		}
	}
	for (s32 rise = 1; i < m; rise++)
		Tr[i++] = rise;
}

static long family_start_cases, family_start_failures;

static void
family_begin(void)
{
	family_start_cases = cases;
	family_start_failures = failures;
}

static void
family_end(const char *name)
{
	printf("FAMILY\t%s\tcases=%ld\tfailures=%ld\n", name,
	       cases - family_start_cases, failures - family_start_failures);
}

int
main(void)
{
	/* Every string of up to 11 entries over 3 names. */
	family_begin();
	for (s32 m = 2; m <= 11; m++) {
		long total = 1;
		for (s32 i = 0; i < m; i++)
			total *= 3;
		for (long code = 0; code < total; code++) {
			long c = code;
			for (s32 i = 0; i < m; i++, c /= 3)
				Tr[i] = (s32)(c % 3);
			check("exhaustive", m);
		}
	}
	family_end("exhaustive");

	/* Random strings with runs, over 1 to 6 names, short and long. */
	family_begin();
	for (int rep = 0; rep < 4000; rep++) {
		s32 m = rep % 4 == 0 ? BZNG_REC_SAMPLE - 4 + (s32)rnd(9) :
				       2 + (s32)rnd(300);
		uint32_t alpha = 1 + rnd(6), stay = rnd(60);
		Tr[0] = (s32)rnd(alpha);
		for (s32 i = 1; i < m; i++)
			Tr[i] = rnd(100) < stay ? Tr[i - 1] : (s32)rnd(alpha);
		check("random", m);
	}
	family_end("random");

	/*
	 * Mixtures of the pieces, around the threshold, at lengths inside the
	 * window and beyond it; both answers must come up often.
	 */
	family_begin();
	long chosen[2] = {0, 0};
	for (int rep = 0; rep < 3000; rep++) {
		uint32_t weight[N_PIECES] = {4 + rnd(28), rnd(8), rnd(8),
					     rnd(4)};
		s32 m = rep % 3 == 0 ? BZNG_REC_SAMPLE + (s32)rnd(2000) :
				       2 + (s32)rnd(BZNG_REC_SAMPLE);
		fill_pieces(m, weight);
		chosen[check("pieces", m)]++;
	}
	if (chosen[0] < 300 || chosen[1] < 300) {
		failures++;
		fprintf(stderr, "FAIL family=pieces: answers %ld no, %ld yes\n",
			chosen[0], chosen[1]);
	}
	family_end("pieces");

	/*
	 * Windows of 50k entries with 21k - 1, 21k and 21k + 1 LMS positions,
	 * on and either side of the threshold, with no, one or k runs of each
	 * kind: one leaves the lower bound exact, the other the upper.
	 */
	family_begin();
	for (int kind = 0; kind < 2; kind++) {
		for (s32 k = 1; 50 * k < BZNG_REC_SAMPLE; k++) {
			for (s32 extra = -1; extra <= 1; extra++) {
				s32 runs[3] = {0, 1, k};
				for (int r = 0; r < 3; r++) {
					fill_threshold(50 * k, 21 * k + extra,
						       runs[r], kind);
					check("threshold", 50 * k);
				}
			}
		}
	}
	family_end("threshold");

	printf("TEST\tcases=%ld\tfailures=%ld\n", cases, failures);
	return failures != 0;
}
