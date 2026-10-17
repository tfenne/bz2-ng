/* Correctness of the encoder's sort stage on generated blocks (the sorter's
 * oracle tests).
 *
 * usage: sort_test [-q]
 *   -q  quick: skip the cases longer than 100,000 bytes (for sanitizer builds)
 *
 * Every block goes through the encoder's sort stage (bzng_sort_block: T.T,
 * least rotation, root, sorter, origPtr) and the BWT and origPtr are compared
 * with an independent oracle: cyclic prefix doubling with radix sorts (and,
 * up to 64 bytes, a naive rotation sort that cross-checks the doubling). The
 * BWT must match exactly; origPtr must be the last row of rotation 0's group
 * of equal rotations (its exact row when the block is primitive).
 *
 * Prints one line per family and a final "TEST ... failures=N" line; exits
 * non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "enc.h"
#include "sort.h"
#include "sort_oracle.h"
#include "test_tier.h"

#define MAX_N 1000000

/* ---- deterministic generator -----------------------------------------------
 */
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

/* ---- the check
 * ------------------------------------------------------------------ */
static bzng_enc_core core;
static long cases, failures;
static uint8_t *expect_L;

static void
report_failure(const char *family, const uint8_t *T, int32_t n, int sorter,
	       const char *what)
{
	failures++;
	if (failures > 20)
		return;
	fprintf(stderr, "FAIL family=%s n=%d sorter=%d: %s; block starts",
		family, (int)n, sorter, what);
	for (int32_t i = 0; i < n && i < 24; i++)
		fprintf(stderr, " %02x", T[i]);
	fprintf(stderr, "\n");
}

static void
check_block(const char *family, const uint8_t *T, int32_t n)
{
	cases++;
	oracle_sort(T, n);
	if (n <= 64) {
		int32_t idx[64];
		for (int32_t i = 0; i < n; i++)
			idx[i] = i;
		naive_T = T;
		naive_n = n;
		qsort(idx, (size_t)n, sizeof idx[0], naive_cmp);
		for (int32_t i = 0; i < n; i++)
			if (o_rank[idx[i]] != o_rank[o_sa[i]]) {
				report_failure(family, T, n, -1,
					       "oracles disagree");
				return;
			}
	}
	int32_t hi = 0;
	for (int32_t i = 0; i < n; i++) {
		int32_t j = o_sa[i];
		expect_L[i] = T[j == 0 ? n - 1 : j - 1];
		if (o_rank[j] == o_rank[0])
			hi = i;
	}
	memcpy(core.tt, T, (size_t)n);
	/*
	 * Exactly the work bzng_bwt requires for n, about what a full encoder
	 * block gets, so that the sorter runs with the least free space it is
	 * promised; a non-zero fill, so that nothing relies on zeroed memory.
	 */
	core.work_ints = bzng_bwt_work_ints(n);
	core.work = malloc(core.work_ints * sizeof(int32_t));
	memset(core.work, (cases & 1) ? 0xA5 : 0xFF,
	       core.work_ints * sizeof(int32_t));
	int32_t freq[256];
	uint8_t *L;
	int32_t orig = bzng_sort_block(&core, n, freq, &L);
	free(core.work);
	if (orig < 0)
		report_failure(family, T, n, 0, "sorter failed");
	else if (memcmp(L, expect_L, (size_t)n) != 0)
		report_failure(family, T, n, 0, "BWT differs");
	else if (orig != hi)
		report_failure(family, T, n, 0, "origPtr differs");
	int32_t cnt[256] = {0};
	for (int32_t i = 0; i < n; i++)
		cnt[T[i]]++;
	if (memcmp(cnt, freq, sizeof cnt) != 0)
		report_failure(family, T, n, 0, "freq differs");
}

static int quick;
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
	fflush(stdout);
}

/* ---- generators
 * -------------------------------------------------------------------- */
static uint8_t *buf;

static void
exhaustive(int sigma, int max_len, const char *name)
{
	family_begin();
	for (int len = 1; len <= max_len; len++) {
		long total = 1;
		for (int i = 0; i < len; i++)
			total *= sigma;
		for (long x = 0; x < total; x++) {
			long v = x;
			for (int i = 0; i < len; i++) {
				buf[i] = (uint8_t)('a' + v % sigma);
				v /= sigma;
			}
			check_block(name, buf, len);
		}
	}
	family_end(name);
}

static void
random_every_length(int sigma, int max_len, const char *name)
{
	family_begin();
	for (int len = 1; len <= max_len; len++) {
		for (int i = 0; i < len; i++)
			buf[i] = (uint8_t)(rnd((uint32_t)sigma));
		check_block(name, buf, len);
	}
	family_end(name);
}

static void
all_equal(void)
{
	family_begin();
	for (int len = 1; len <= 2000; len++) {
		memset(buf, 'z', (size_t)len);
		check_block("all_equal", buf, len);
	}
	if (!quick) {
		memset(buf, 0, 900000);
		check_block("all_equal", buf, 900000);
	}
	family_end("all_equal");
}

/* a^k b and similar single-exception strings. */
static void
one_exception(void)
{
	family_begin();
	for (int len = 2; len <= 1500; len++) {
		memset(buf, 'a', (size_t)len);
		buf[len - 1] = 'b';
		check_block("one_exception", buf, len);
		buf[len - 1] = 'a';
		buf[rnd((uint32_t)len)] = (uint8_t)(rnd(2) ? 'b' : 0);
		check_block("one_exception", buf, len);
	}
	family_end("one_exception");
}

/* Fibonacci word s_k (s_1 = a, s_2 = ab, s_k = s_{k-1} s_{k-2}), or the
 * longest one not exceeding max_len. s_{k-2} is a prefix of s_{k-1}, so the
 * word grows in place by appending its own prefix. */
static int32_t
fibonacci(uint8_t *out, int32_t max_len, int k)
{
	out[0] = 'a';
	out[1] = 'b';
	if (k <= 1)
		return 1;
	int32_t la = 2, lb = 1;
	for (int j = 3; j <= k; j++) {
		int32_t next = la + lb;
		if (next > max_len)
			break;
		memcpy(out + la, out, (size_t)lb);
		lb = la;
		la = next;
	}
	return la;
}

static void
fib_words(void)
{
	family_begin();
	for (int k = 1; k <= 32; k++) {
		int32_t n = fibonacci(buf, quick ? 100000 : MAX_N, k);
		check_block("fibonacci", buf, n);
		if (n > 2)
			check_block("fibonacci", buf, n - 1);
	}
	family_end("fibonacci");
}

static void
thue_morse(void)
{
	family_begin();
	int32_t lens[] = {1,	  2,	  3,	  5,	 7,    8,     15,
			  16,	  17,	  31,	  32,	 33,   100,   255,
			  256,	  1000,	  1024,	  4096,	 4097, 65535, 65536,
			  100000, 262144, 524288, 900000};
	for (size_t t = 0; t < sizeof lens / sizeof lens[0]; t++) {
		int32_t n = lens[t];
		if (quick && n > 100000)
			continue;
		for (int32_t i = 0; i < n; i++)
			buf[i] = (uint8_t)('a' + (popcount32((u32)i) & 1));
		check_block("thue_morse", buf, n);
	}
	family_end("thue_morse");
}

/* de Bruijn sequence B(k, m) by the FKM algorithm. */
static int32_t db_len;
static void
db_rec(int t, int p, int k, int m, uint8_t *a, uint8_t *out)
{
	if (t > m) {
		if (m % p == 0)
			for (int j = 1; j <= p; j++)
				out[db_len++] = a[j];
		return;
	}
	a[t] = a[t - p];
	db_rec(t + 1, p, k, m, a, out);
	for (int j = a[t - p] + 1; j < k; j++) {
		a[t] = (uint8_t)j;
		db_rec(t + 1, t, k, m, a, out);
	}
}
static void
de_bruijn(void)
{
	family_begin();
	int params[][2] = {{2, 1},  {2, 2},  {2, 3},  {2, 5},  {2, 8},	{2, 12},
			   {2, 16}, {2, 19}, {3, 4},  {3, 8},  {3, 12}, {4, 3},
			   {4, 6},  {4, 9},  {16, 2}, {16, 4}, {256, 2}};
	uint8_t a[64];
	for (size_t t = 0; t < sizeof params / sizeof params[0]; t++) {
		int k = params[t][0], m = params[t][1];
		double len = 1;
		for (int i = 0; i < m; i++)
			len *= k;
		if (len > MAX_N || (quick && len > 100000))
			continue;
		memset(a, 0, sizeof a);
		db_len = 0;
		db_rec(1, 1, k, m, a, buf);
		check_block("de_bruijn", buf, db_len);
	}
	family_end("de_bruijn");
}

static void
long_runs(void)
{
	family_begin();
	for (int c = 0; c < 60; c++) {
		int32_t n = (int32_t)(1 + rnd(quick ? 50000 : 300000)), i = 0;
		int sigma = 1 + (int)rnd(c < 30 ? 3 : 256);
		uint32_t max_run = c % 3 == 0 ? 10 : c % 3 == 1 ? 1000 : 20000;
		while (i < n) {
			int32_t run = (int32_t)(1 + rnd(max_run));
			uint8_t ch = (uint8_t)rnd((uint32_t)sigma);
			for (int32_t j = 0; j < run && i < n; j++)
				buf[i++] = ch;
		}
		check_block("long_runs", buf, n);
	}
	family_end("long_runs");
}

static void
every_alphabet(void)
{
	family_begin();
	for (int sigma = 1; sigma <= 256; sigma++) {
		int32_t n = 5000;
		for (int32_t i = 0; i < n; i++)
			buf[i] = (uint8_t)rnd((uint32_t)sigma);
		check_block("alphabet", buf, n);
		for (int32_t i = 0; i < n; i++)
			buf[i] = (uint8_t)(255 - rnd((uint32_t)sigma));
		check_block("alphabet", buf, n);
		/* Skewed: one dominant symbol. */
		for (int32_t i = 0; i < n; i++)
			buf[i] = (uint8_t)(rnd(8) ? 7 : rnd((uint32_t)sigma));
		check_block("alphabet", buf, n);
	}
	family_end("alphabet");
}

/* Text-like and repetitive generators for long random-length cases. */
static void
fill_mixed(uint8_t *out, int32_t n, int kind)
{
	switch (kind) {
	case 0:
		for (int32_t i = 0; i < n; i++)
			out[i] = (uint8_t)rng();
		break;
	case 1:
		for (int32_t i = 0; i < n; i++)
			out[i] = (uint8_t)"ACGT"[rnd(4)];
		break;
	case 2: { /* order-1 Markov over 27 symbols, text-ish */
		uint8_t prev = 'a';
		for (int32_t i = 0; i < n; i++) {
			uint32_t r = rnd(100);
			prev = r < 18 ? ' ' :
			       r < 60 ? (uint8_t)('a' +
						  (prev * 7 + rnd(5)) % 26) :
					(uint8_t)('a' + rnd(26));
			out[i] = prev;
		}
		break;
	}
	default: { /* repetitive: copies of earlier substrings with mutations */
		int32_t i = 0;
		while (i < n) {
			if (i < 64 || rnd(4) == 0) {
				out[i++] = (uint8_t)rnd(16);
			} else {
				int32_t src = (int32_t)rnd((uint32_t)i),
					len = (int32_t)(1 + rnd(5000));
				for (int32_t j = 0; j < len && i < n; j++)
					out[i++] = out[src + j];
				if (i < n)
					out[i++] = (uint8_t)rnd(16);
			}
		}
	}
	}
}

/* Zigzag strings (low, high, low, high, ...): an LMS suffix at every other
 * position, so the reduced problem has n/2 symbols and, with many distinct
 * (low, high, low) triples, nearly as many names: the worst case for the
 * recursion's bucket space. */
static void
zigzag(void)
{
	family_begin();
	int32_t lens[] = {10, 101, 1000, 4097, 65536, 300001, 900000};
	for (size_t t = 0; t < sizeof lens / sizeof lens[0]; t++) {
		int32_t n = lens[t];
		if (quick && n > 100000)
			continue;
		for (int lows = 1; lows <= 128; lows *= 8) {
			for (int32_t i = 0; i < n; i++)
				buf[i] = (uint8_t)(i & 1 ? 128 + rnd(128) :
							   rnd((uint32_t)lows));
			check_block("zigzag", buf, n);
		}
	}
	family_end("zigzag");
}

/* Blocks at the edge of the condition under which the sorter keeps the LMS
 * positions of its first pass for after the recursion (patch 0001 of
 * third_party/libsais/patches). With the free space bzng_bwt_work_ints(n)
 * gives, floor(n/2), it keeps them while m <= (floor(n/2) + n) / 4, and at
 * that m the first level of the recursion has free space equal to its
 * length. Types run in periods of eight, SSLSSLSL (three LMS positions a
 * period), with single periods made SLSLSLSL (one more) or SSLSSSSL (one
 * fewer) to reach m = edge - 1, edge and edge + 1. Bytes are random over
 * most of their range and chosen right to left with strict inequalities, so
 * that the types hold exactly and most LMS substrings are distinct: the
 * recursion's alphabet is large. T[0] is the block's only 0, so the block is
 * its own least rotation and reaches the sorter unrotated. */
static uint8_t edge_types[MAX_N]; /* 1 = S */

/* LMS positions of T[0..n) (T[n - 1] is L). */
static int32_t
count_lms(const uint8_t *T, int32_t n)
{
	int32_t m = 0;
	int next_s = 0;
	for (int32_t i = n - 2; i >= 0; i--) {
		int s = T[i] < T[i + 1] || (T[i] == T[i + 1] && next_s);
		m += next_s && !s;
		next_s = s;
	}
	return m;
}

static void
edge_block(uint8_t *out, int32_t n, int32_t target)
{
	int32_t periods = n / 8, m = 0;
	for (int32_t i = 0; i < n; i++)
		edge_types[i] = (uint8_t)("SSLSSLSL"[i % 8] == 'S');
	edge_types[n - 1] = 0;
	for (int32_t i = 1; i < n; i++)
		m += edge_types[i] && !edge_types[i - 1];
	/* A whole period other than the first and the last, changed, moves m by
	 * one; the changed periods are spread over the block. */
	int32_t d = target - m, todo = d < 0 ? -d : d;
	const char *swap = d > 0 ? "SLSLSLSL" : "SSLSSSSL";
	int32_t stride = (periods - 2) / (todo + 1);
	for (int32_t k = 1; k <= todo; k++) {
		uint8_t *period = edge_types + 8 * k * stride;
		for (int j = 0; j < 8; j++)
			period[j] = (uint8_t)(swap[j] == 'S');
	}
	/* Each S below the next byte and each L above it, leaving room for the
	 * run of S positions to its left, which takes values from 1 up. */
	out[n - 1] = (uint8_t)(16 + rnd(200));
	for (int32_t i = n - 2; i >= 1; i--) {
		int run = 0, next = out[i + 1], lo, hi;
		for (int32_t j = i - 1; j >= 1 && edge_types[j]; j--)
			run++;
		if (edge_types[i]) {
			lo = run + 1;
			hi = next - 1;
		} else {
			lo = next + 1 > run + 1 ? next + 1 : run + 1;
			hi = 255;
		}
		out[i] = (uint8_t)(lo + (int)rnd((uint32_t)(hi - lo + 1)));
	}
	out[0] = 0;
}

static void
lms_edge(void)
{
	family_begin();
	int32_t lens[] = {1000, 4099, 65537, 300001, 900000};
	for (size_t t = 0; t < sizeof lens / sizeof lens[0]; t++) {
		int32_t n = lens[t], edge = (n / 2 + n) / 4;
		if (quick && n > 100000)
			continue;
		for (int32_t target = edge - 1; target <= edge + 1; target++) {
			edge_block(buf, n, target);
			if (count_lms(buf, n) != target) {
				report_failure("lms_edge", buf, n, -1,
					       "LMS count missed");
				continue;
			}
			check_block("lms_edge", buf, n);
		}
	}
	family_end("lms_edge");
}

static void
random_long(void)
{
	family_begin();
	for (int c = 0; c < (quick ? 8 : 40); c++) {
		int32_t n = (int32_t)(1 + rnd(quick ? 100000 : 900000));
		fill_mixed(buf, n, c % 4);
		check_block("random_long", buf, n);
	}
	family_end("random_long");
}

static void
periodic(void)
{
	family_begin();
	for (int c = 0; c < 400; c++) {
		int32_t ulen = (int32_t)(1 + rnd(c < 300 ? 40 : 2000));
		int32_t k = (int32_t)(2 + rnd(c < 300 ? 100 : 30));
		int sigma = 1 + (int)rnd(c % 2 ? 3 : 256);
		for (int32_t i = 0; i < ulen; i++)
			buf[i] = (uint8_t)rnd((uint32_t)sigma);
		for (int32_t j = 1; j < k; j++)
			memcpy(buf + j * ulen, buf, (size_t)ulen);
		check_block("periodic", buf, ulen * k);
		/* Near-periodic: a partial extra copy. */
		int32_t extra = (int32_t)(1 + rnd((uint32_t)ulen));
		memcpy(buf + ulen * k, buf, (size_t)extra);
		check_block("periodic", buf, ulen * k + extra);
	}
	family_end("periodic");
}

int
main(int argc, char **argv)
{
	int opt;
	require_requested_tier();
	while ((opt = getopt(argc, argv, "q")) != -1) {
		if (opt == 'q')
			quick = 1;
		else
			return 2;
	}
	oracle_alloc(MAX_N);
	buf = malloc(2 * MAX_N + 64);
	expect_L = malloc(MAX_N);
	core.cap = MAX_N;
	core.tt = malloc(2 * (size_t)MAX_N + BZNG_ENC_PAD);

	exhaustive(2, 14, "exhaustive_sigma2");
	exhaustive(3, 9, "exhaustive_sigma3");
	exhaustive(4, 7, "exhaustive_sigma4");
	random_every_length(2, 3000, "every_length_sigma2");
	random_every_length(3, 3000, "every_length_sigma3");
	random_every_length(4, 3000, "every_length_sigma4");
	random_every_length(256, 3000, "every_length_sigma256");
	all_equal();
	one_exception();
	fib_words();
	thue_morse();
	de_bruijn();
	long_runs();
	every_alphabet();
	periodic();
	zigzag();
	lms_edge();
	random_long();

	printf("TEST\ttier=%s\tquick=%d\tcases=%ld\tfailures=%ld\n",
	       bzng_tier_name(), quick, cases, failures);
	free(core.tt);
	return failures != 0;
}
