/*
 * dec_difftest.c - differential tester for decompression
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */

/*
 * Drives stock libbz2 1.0.8 (test/reference, symbols renamed ref_*) and
 * bz2-ng's BZ2_bzDecompress* with identical call sequences and compares
 * everything observable after every call: the return code, next_in,
 * avail_in, next_out and avail_out (as offsets), total_in and total_out, and
 * the output buffers.  Both buffers start with the same pattern, and before
 * every call the bytes from next_out on get a pattern new to that call;
 * after it the delivered bytes and the bytes just past next_out, which
 * neither library may change, are compared, and after the last call the
 * whole buffers.  A sequence stops at the first return other than BZ_OK (the
 * manual allows only BZ2_bzDecompressEnd after it); only the api phase calls
 * again after that, and calls libbz2 again only where its state is idle.
 *
 * usage: dec_difftest [-q] [-s seed] [-n mutations] [-c chunk] [-l level]
 *                     phase... [-- file ...]
 *
 * phases:
 *   valid    generated streams at levels 1-9, plus any files (cut into
 *            streams of -c bytes and compressed by libbz2 at -l), under every
 *            schedule: whole buffers, every input split, every output split,
 *            fixed and random chunkings
 *   mutate   every bit flipped (small streams), truncation at every byte,
 *            targeted header damage, and -n random mutations
 *   read     concatenated streams and trailing garbage through BZ2_bzRead and
 *            BZ2_bzReadGetUnused; BZ2_bzBuffToBuffDecompress
 *   alloc    allocation failure injection
 *   api      parameter and sequence errors; calls after the end of a stream
 *            and after an error (docs/spec.md section 2.5)
 *   crafted  hand-built streams for libbz2's edge cases, under every input
 *            and output split
 *   slack    block endings that test that no byte past next_out is left
 *            modified, whole and in output pieces
 *   all      everything
 *
 * Every phase starts its generator from the seed, so a run is reproducible
 * from its arguments.  Counts are printed per phase; any mismatch is printed
 * and makes the exit status 1.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bzlib.h"
#include "compat.h"
#include "kern.h"
#include "reference_api.h"
#include "test_tier.h"

static int quiet;
static uint64_t n_runs, n_calls, n_fail, n_accept, n_reject;
/* Seed of the generator (-s); every phase restarts from it. */
static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t
rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return rs;
}

/* ------------------------------------------------------------ allocator */

typedef struct {
	long calls, fail_at; /* fail the fail_at-th call (1-based), 0 = never */
	size_t cur, peak;
} AllocCount;

static void *
count_alloc(void *opaque, int items, int size)
{
	AllocCount *a = opaque;
	a->calls++;
	if (a->fail_at && a->calls == a->fail_at)
		return NULL;
	size_t n = (size_t)items * (size_t)size;
	size_t *p = malloc(n + 16);
	if (!p)
		return NULL;
	p[0] = n;
	a->cur += n;
	if (a->cur > a->peak)
		a->peak = a->cur;
	return (char *)p + 16;
}

static void
count_free(void *opaque, void *addr)
{
	AllocCount *a = opaque;
	if (!addr)
		return;
	size_t *p = (size_t *)(void *)((char *)addr - 16);
	a->cur -= p[0];
	free(p);
}

/* ------------------------------------------------------------ schedules */

typedef enum {
	SCH_FIXED,
	SCH_SPLIT_IN,
	SCH_SPLIT_OUT,
	SCH_RANDOM
} SchKind;
typedef struct {
	SchKind kind;
	size_t a, b; /* FIXED: in/out chunk; SPLIT_*: split point; RANDOM: max
			chunks */
	uint64_t seed;
} Sched;

static void
sched_name(const Sched *s, char *buf, size_t n)
{
	switch (s->kind) {
	case SCH_FIXED:
		snprintf(buf, n, "fixed in=%zu out=%zu", s->a, s->b);
		break;
	case SCH_SPLIT_IN:
		snprintf(buf, n, "split-in at %zu", s->a);
		break;
	case SCH_SPLIT_OUT:
		snprintf(buf, n, "split-out at %zu", s->a);
		break;
	case SCH_RANDOM:
		snprintf(buf, n, "random max in=%zu out=%zu seed=%llu", s->a,
			 s->b, (unsigned long long)s->seed);
		break;
	}
}

static size_t
rand_chunk(uint64_t *st, size_t max)
{
	*st ^= *st << 13;
	*st ^= *st >> 7;
	*st ^= *st << 17;
	/* mostly small, sometimes large */
	const uint64_t r = *st;
	size_t c;
	switch (r & 3) {
	case 0:
		c = 1 + (r >> 8) % 4;
		break;
	case 1:
		c = 1 + (r >> 8) % 64;
		break;
	case 2:
		c = 1 + (r >> 8) % 5000;
		break;
	default:
		c = 1 + (r >> 8) % (max ? max : 1);
		break;
	}
	return c;
}

/* ------------------------------------------------------------ one run */

typedef struct {
	const char *name;
	const uint8_t *in;
	size_t in_len;
	size_t out_cap;
	int small;
} Case;

/*
 * The two output buffers.  Each has OUT_SLACK bytes beyond the largest
 * avail_out ever offered, so that a write past the offered space would land
 * in memory the tester owns and is seen by the whole-buffer comparison.
 */
#define OUT_SLACK 512
/*
 * Bytes from next_out that get a fresh pattern before every call, and bytes
 * past the new next_out compared after it.  The bulk un-RLE1 kernel's stores
 * reach at most 268 bytes past what it delivers.
 */
#define OUT_WINDOW 320

static uint8_t *out_a, *out_b;
static size_t out_buf_cap;

/* Makes out_a and out_b hold at least cap + OUT_SLACK bytes. */
static void
ensure_out(size_t cap)
{
	if (cap <= out_buf_cap)
		return;
	free(out_a);
	free(out_b);
	out_a = malloc(cap + OUT_SLACK);
	out_b = malloc(cap + OUT_SLACK);
	if (out_a == NULL || out_b == NULL) {
		fprintf(stderr, "out of memory (%zu-byte output buffers)\n",
			cap + OUT_SLACK);
		exit(2);
	}
	out_buf_cap = cap;
}

/*
 * Fills a[0..n) and b[0..n) with the same pseudo-random bytes from
 * pattern_seed, so that any byte a decoder writes but libbz2 does not (or the
 * reverse) shows up when the buffers are compared.
 */
static void
fill_pattern(uint8_t *a, uint8_t *b, size_t n, uint64_t pattern_seed)
{
	uint64_t x = pattern_seed * 0x9E3779B97F4A7C15ull | 1;
	size_t i = 0;

	for (; i + 8 <= n; i += 8) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		memcpy(a + i, &x, 8);
	}
	for (; i < n; i++)
		a[i] = (uint8_t)(x >> (8 * (i & 7)));
	memcpy(b, a, n);
}

/* First index in [from, to) where a and b differ, or to if none. */
static size_t
first_difference(const uint8_t *a, const uint8_t *b, size_t from, size_t to)
{
	if (to <= from || memcmp(a + from, b + from, to - from) == 0)
		return to;
	while (a[from] == b[from])
		from++;
	return from;
}

static void
report(const Case *c, const Sched *sc, long call, const char *what, long va,
       long vb)
{
	n_fail++;
	if (n_fail > 30)
		return;
	char sn[128];
	sched_name(sc, sn, sizeof sn);
	fprintf(stderr,
		"MISMATCH %s (in %zu bytes, small=%d) [%s] call %ld: %s "
		"ref=%ld ng=%ld\n",
		c->name, c->in_len, c->small, sn, call, what, va, vb);
}

/*
 * Runs both decoders over one input with one schedule; returns the final
 * return code (from the reference) or 1000 for a mismatch.
 */
static int
run_case(const Case *c, const Sched *sc)
{
	ensure_out(c->out_cap);
	AllocCount ca = {0, 0, 0, 0}, cb = {0, 0, 0, 0};
	bz_stream a, b;
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	a.bzalloc = count_alloc;
	a.bzfree = count_free;
	a.opaque = &ca;
	b.bzalloc = count_alloc;
	b.bzfree = count_free;
	b.opaque = &cb;
	n_runs++;
	int ra = ref_BZ2_bzDecompressInit(&a, 0, c->small);
	int rb = BZ2_bzDecompressInit(&b, 0, c->small);
	if (ra != rb) {
		report(c, sc, -1, "init", ra, rb);
		return 1000;
	}
	const size_t buf_len = c->out_cap + OUT_SLACK;
	fill_pattern(out_a, out_b, buf_len, n_runs << 32);
	size_t in_off = 0, out_off = 0;
	uint64_t rst = sc->seed | 1;
	long call = 0;
	int stalls = 0;
	int ret = BZ_OK, mismatch = 0;
	for (;; call++) {
		size_t ci, co;
		switch (sc->kind) {
		case SCH_FIXED:
			ci = sc->a;
			co = sc->b;
			break;
		case SCH_SPLIT_IN:
			ci = call == 0 ? sc->a : SIZE_MAX;
			co = SIZE_MAX;
			break;
		case SCH_SPLIT_OUT:
			ci = SIZE_MAX;
			co = call == 0 ? sc->a : SIZE_MAX;
			break;
		default:
			ci = rand_chunk(&rst, sc->a);
			co = rand_chunk(&rst, sc->b);
			break;
		}
		if (ci > c->in_len - in_off)
			ci = c->in_len - in_off;
		if (co > c->out_cap - out_off)
			co = c->out_cap - out_off;
		a.next_in = b.next_in = (char *)c->in + in_off;
		a.avail_in = b.avail_in = (unsigned)ci;
		a.next_out = (char *)out_a + out_off;
		b.next_out = (char *)out_b + out_off;
		a.avail_out = b.avail_out = (unsigned)co;
		/*
		 * A pattern new to this call where the decoders write next, so
		 * that neither can rely on what an earlier call left there
		 */
		const size_t fresh = buf_len - out_off < OUT_WINDOW ?
					     buf_len - out_off :
					     OUT_WINDOW;
		fill_pattern(out_a + out_off, out_b + out_off, fresh,
			     n_runs << 32 | (uint64_t)(call + 1));
		ra = ref_BZ2_bzDecompress(&a);
		rb = BZ2_bzDecompress(&b);
		n_calls++;
		const size_t ia = (size_t)((const uint8_t *)a.next_in - c->in);
		const size_t ib = (size_t)((const uint8_t *)b.next_in - c->in);
		const size_t oa = (size_t)((uint8_t *)a.next_out - out_a);
		const size_t ob = (size_t)((uint8_t *)b.next_out - out_b);
		if (ra != rb) {
			report(c, sc, call, "return code", ra, rb);
			mismatch = 1;
		} else if (ia != ib) {
			report(c, sc, call, "next_in offset", (long)ia,
			       (long)ib);
			mismatch = 1;
		} else if (a.avail_in != b.avail_in) {
			report(c, sc, call, "avail_in", a.avail_in, b.avail_in);
			mismatch = 1;
		} else if (oa != ob) {
			report(c, sc, call, "next_out offset", (long)oa,
			       (long)ob);
			mismatch = 1;
		} else if (a.avail_out != b.avail_out) {
			report(c, sc, call, "avail_out", a.avail_out,
			       b.avail_out);
			mismatch = 1;
		} else if (a.total_in_lo32 != b.total_in_lo32 ||
			   a.total_in_hi32 != b.total_in_hi32) {
			report(c, sc, call, "total_in", a.total_in_lo32,
			       b.total_in_lo32);
			mismatch = 1;
		} else if (a.total_out_lo32 != b.total_out_lo32 ||
			   a.total_out_hi32 != b.total_out_hi32) {
			report(c, sc, call, "total_out", a.total_out_lo32,
			       b.total_out_lo32);
			mismatch = 1;
		} else {
			/*
			 * The delivered bytes, and the bytes just past
			 * next_out, which neither library may have changed
			 */
			const size_t end = oa + OUT_WINDOW < buf_len ?
						   oa + OUT_WINDOW :
						   buf_len;
			const size_t k =
				first_difference(out_a, out_b, out_off, end);
			if (k < end) {
				report(c, sc, call,
				       k < oa ? "output byte at" :
						"buffer byte past next_out at",
				       (long)k, (long)oa);
				mismatch = 1;
			}
		}
		if (mismatch)
			break;
		const int progress = ia != in_off || oa != out_off;
		in_off = ia;
		out_off = oa;
		ret = ra;
		if (ra != BZ_OK)
			break;
		if (!progress &&
		    (in_off == c->in_len || out_off == c->out_cap || ci == 0 ||
		     co == 0)) {
			if (++stalls >= 2)
				break;
		} else {
			stalls = 0;
		}
	}
	if (!mismatch) {
		const size_t k = first_difference(out_a, out_b, 0, buf_len);
		if (k < buf_len) {
			report(c, sc, call, "buffer byte at (end of run)",
			       (long)k, (long)out_off);
			mismatch = 1;
		}
	}
	ra = ref_BZ2_bzDecompressEnd(&a);
	rb = BZ2_bzDecompressEnd(&b);
	if (!mismatch && ra != rb) {
		report(c, sc, call, "end", ra, rb);
		mismatch = 1;
	}
	if (!mismatch && (ca.cur != 0 || cb.cur != 0)) {
		report(c, sc, call, "leak", (long)ca.cur, (long)cb.cur);
		mismatch = 1;
	}
	if (!mismatch && ca.calls != cb.calls) {
		report(c, sc, call, "bzalloc calls", ca.calls, cb.calls);
		mismatch = 1;
	}
	if (mismatch)
		return 1000;
	if (ret == BZ_STREAM_END)
		n_accept++;
	else
		n_reject++;
	return ret;
}

/* ------------------------------------------------------------ inputs */

typedef struct {
	char name[96];
	uint8_t *raw, *comp;
	size_t raw_len, comp_len;
} Stream;

static Stream *streams;
static size_t n_streams, cap_streams;

static void
add_stream(const char *name, const uint8_t *raw, size_t raw_len, int level,
	   int randomise)
{
	unsigned cl = (unsigned)(raw_len + raw_len / 50 + 1000);
	uint8_t *comp = malloc(cl);
	int r = (randomise ? rand_BZ2_bzBuffToBuffCompress :
			     ref_BZ2_bzBuffToBuffCompress)(
		(char *)comp, &cl, (char *)raw, (unsigned)raw_len, level, 0,
		30);
	if (r != BZ_OK) {
		fprintf(stderr, "compress failed %s: %d\n", name, r);
		exit(2);
	}
	if (n_streams == cap_streams) {
		cap_streams = cap_streams ? 2 * cap_streams : 64;
		streams = realloc(streams, cap_streams * sizeof *streams);
	}
	Stream *s = &streams[n_streams++];
	snprintf(s->name, sizeof s->name, "%s/L%d%s", name, level,
		 randomise ? "/rand" : "");
	s->raw = malloc(raw_len + 1);
	memcpy(s->raw, raw, raw_len);
	s->raw_len = raw_len;
	s->comp = comp;
	s->comp_len = cl;
}

/* Adds a stream given in compressed form, with the bytes it decodes to. */
static void
add_raw_stream(const char *name, const uint8_t *raw, size_t raw_len,
	       const uint8_t *comp, size_t comp_len)
{
	if (n_streams == cap_streams) {
		cap_streams = cap_streams ? 2 * cap_streams : 64;
		streams = realloc(streams, cap_streams * sizeof *streams);
	}
	Stream *st = &streams[n_streams++];
	snprintf(st->name, sizeof st->name, "%s", name);
	st->raw = malloc(raw_len + 1);
	memcpy(st->raw, raw, raw_len);
	st->raw_len = raw_len;
	st->comp = malloc(comp_len + 1);
	memcpy(st->comp, comp, comp_len);
	st->comp_len = comp_len;
}

static void
gen_inputs(void)
{
	const size_t N = 2100000;
	uint8_t *b = calloc(N, 1);
	size_t n;
	/* empty and one byte */
	for (int lv = 1; lv <= 9; lv += 8) {
		add_stream("empty", b, 0, lv, 0);
		b[0] = 'A';
		add_stream("one_byte", b, 1, lv, 0);
	}
	/* all 256 byte values, ascending, then descending */
	for (int i = 0; i < 512; i++)
		b[i] = (uint8_t)(i < 256 ? i : 511 - i);
	add_stream("all256", b, 512, 9, 0);
	/*
	 * runs at every RLE1 boundary, each followed by a different byte, and a
	 * trailing run of each length
	 */
	n = 0;
	static const int lens[] = {1,	2,   3,	  4,	5,    6,   7,
				   8,	9,   254, 255,	256,  257, 258,
				   259, 260, 261, 262,	263,  264, 265,
				   517, 518, 519, 1000, 4095, 4096};
	for (size_t k = 0; k < sizeof lens / sizeof lens[0]; k++) {
		for (int j = 0; j < lens[k]; j++)
			b[n++] = (uint8_t)('a' + k % 20);
		b[n++] = 'Z';
	}
	add_stream("runs_mixed", b, n, 9, 0);
	add_stream("runs_mixed", b, n, 1, 1);
	for (size_t k = 0; k < sizeof lens / sizeof lens[0]; k++) {
		n = 0;
		for (int j = 0; j < 37; j++)
			b[n++] = (uint8_t)("xyz"[j % 3]);
		for (int j = 0; j < lens[k]; j++)
			b[n++] = 'q';
		char nm[32];
		snprintf(nm, sizeof nm, "tail_run_%d", lens[k]);
		add_stream(nm, b, n, 9, 0);
	}
	/* one symbol, 1 MB: a block of runs */
	memset(b, 'x', 1000000);
	add_stream("one_symbol_1M", b, 1000000, 9, 0);
	add_stream("one_symbol_1M", b, 1000000, 2, 1);
	/* random bytes (incompressible), 2 MB: three level-9 blocks */
	for (size_t i = 0; i < 2000000; i++)
		b[i] = (uint8_t)rnd();
	add_stream("random_2M", b, 2000000, 9, 0);
	add_stream("random_300k", b, 300000, 1, 0);
	add_stream("random_300k", b, 300000, 1, 1);
	/* geometric distribution: long Huffman codes */
	for (size_t i = 0; i < 2000000; i++) {
		uint64_t r = rnd();
		int c = 0;
		while ((r & 1) && c < 200) {
			c++;
			r >>= 1;
			if (!r)
				r = rnd();
		}
		b[i] = (uint8_t)c;
	}
	add_stream("skewed_2M", b, 2000000, 9, 0);
	add_stream("skewed_200k", b, 200000, 2, 1);
	/* text-like data with a small dictionary */
	static const char *words[] = {"the ",	"quick ", "brown ",   "fox ",
				      "jumps ", "over ",  "lazy ",    "dog ",
				      "bzip2 ", "block ", "sorting ", "\n"};
	n = 0;
	while (n < 950000) {
		const char *w = words[rnd() % 12];
		size_t l = strlen(w);
		memcpy(b + n, w, l);
		n += l;
	}
	add_stream("text_950k", b, 950000, 9,
		   0); /* one maximum-size block plus a little */
	add_stream("text_950k", b, 950000, 1, 0); /* ten blocks */
	add_stream("text_120k", b, 120000, 1, 1);
	/* a block of 1-4 Ki rows: the walk's 8-lane tier */
	add_stream("text_3k", b, 3000, 9, 0);
	/* exactly periodic blocks: several periods, and u^k filling one block
	 */
	static const size_t periods[] = {1, 2, 3, 7, 251, 1000, 4096, 65537};
	for (size_t k = 0; k < sizeof periods / sizeof periods[0]; k++) {
		uint8_t pat[65537] = {0};
		for (size_t i = 0; i < periods[k]; i++) {
			do
				pat[i] = (uint8_t)('a' + rnd() % 26);
			while (i > 0 && pat[i] == pat[i - 1]);
		}
		if (periods[k] > 1 && pat[periods[k] - 1] == pat[0])
			pat[periods[k] - 1] = pat[0] == 'z' ? 'y' : 'z';
		n = periods[k] == 65537 ? 2000003 : 300007;
		for (size_t i = 0; i < n; i++)
			b[i] = pat[i % periods[k]];
		char nm[32];
		snprintf(nm, sizeof nm, "period_%zu", periods[k]);
		add_stream(nm, b, n, periods[k] == 65537 ? 9 : 3, 0);
	}
	for (size_t i = 0; i < 899981; i++)
		b[i] = (uint8_t)("abcdefghi"[i % 9]);
	add_stream("power_p9", b, 899981, 9, 0);
	for (size_t i = 0; i < 100; i++)
		b[i] = (uint8_t)("ab"[i % 2]);
	add_stream("ab_x50", b, 100, 9, 0);
	/* every level on a 1 MB mixed input */
	for (size_t i = 0; i < 1000000; i++)
		b[i] = (uint8_t)((rnd() % 7 == 0) ?
					 rnd() :
					 (uint64_t)(uint8_t)"ACGT"[(i / 3) %
								   4]);
	for (int lv = 1; lv <= 9; lv++)
		add_stream("mixed_1M", b, 1000000, lv, 0);
	/*
	 * bytes drawn evenly from n symbols, one block each: MTF moves from
	 * every depth of the list, either side of the vector tiers'
	 * MTF_FULL_TAIL_MAX
	 */
	static const int n_used[] = {17, 33, 100, 160, 161, 256};
	for (size_t k = 0; k < sizeof n_used / sizeof n_used[0]; k++) {
		for (size_t i = 0; i < 150000; i++)
			b[i] = (uint8_t)(rnd() % (uint64_t)n_used[k]);
		char nm[32];
		snprintf(nm, sizeof nm, "even_%d", n_used[k]);
		add_stream(nm, b, 150000, 2, 0);
	}
	free(b);
}

/*
 * Adds the file at path, cut into streams of chunk bytes (the whole file if
 * chunk is 0), each compressed by libbz2 at level.
 */
static void
load_file(const char *path, size_t chunk, int level)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		perror(path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = malloc(len > 0 ? (size_t)len : 1);
	if (len > 0 && fread(data, 1, (size_t)len, f) != (size_t)len) {
		perror(path);
		exit(2);
	}
	fclose(f);
	if (chunk == 0)
		chunk = len > 0 ? (size_t)len : 1;
	for (size_t off = 0; off < (size_t)len; off += chunk) {
		size_t n =
			(size_t)len - off < chunk ? (size_t)len - off : chunk;
		char nm[96];
		snprintf(nm, sizeof nm, "%.70s@%zu", path, off);
		add_stream(nm, data + off, n, level, 0);
	}
	free(data);
}

/* ------------------------------------------------------------ phases */

static void
run_all_modes(const Case *c0, const Sched *sc)
{
	Case c = *c0;
	for (int small = 0; small <= 1; small++) {
		c.small = small;
		run_case(&c, sc);
	}
}

/* ------------------------------------------------------- crafted streams */

/*
 * Streams a correct encoder never writes, built bit by bit: the edge cases of
 * libbz2's decoder that bz2-ng reproduces deliberately.  Each block is given
 * as its RLE1 text (or directly as MTF symbols), coded with explicit code
 * lengths and selectors; the block and stream CRCs are filled in from what
 * libbz2 decodes, so the only error in a stream is the one it was built for.
 */

/* MSB-first bit writer over a growing buffer. */
typedef struct {
	uint8_t *p;
	size_t cap, nbits;
} BitBuf;

static void
bb_put(BitBuf *b, int n, uint32_t v)
{
	for (int i = n - 1; i >= 0; i--) {
		if (b->nbits / 8 + 1 >= b->cap) {
			b->cap = b->cap ? 2 * b->cap : 4096;
			b->p = realloc(b->p, b->cap);
			if (b->p == NULL) {
				fprintf(stderr, "out of memory\n");
				exit(2);
			}
		}
		const size_t byte = b->nbits / 8;
		const int shift = 7 - (int)(b->nbits % 8);
		if (shift == 7)
			b->p[byte] = 0;
		b->p[byte] |= (uint8_t)(((v >> i) & 1) << shift);
		b->nbits++;
	}
}

/* Overwrites n bits at bit position pos. */
static void
bb_set(BitBuf *b, size_t pos, int n, uint32_t v)
{
	for (int i = n - 1; i >= 0; i--, pos++) {
		const uint8_t m = (uint8_t)(0x80u >> (pos % 8));
		if ((v >> i) & 1)
			b->p[pos / 8] |= m;
		else
			b->p[pos / 8] &= (uint8_t)~m;
	}
}

/* One block, as it is written. */
typedef struct {
	uint8_t in_use[256];
	uint32_t orig_ptr;
	int randomised;
	int n_groups;
	int n_selectors;
	const uint8_t *selectors; /* n_selectors group numbers */
	uint8_t len[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE];
	const uint16_t *syms; /* MTF / RUNA / RUNB symbols, EOB included */
	size_t n_syms;
	const uint8_t *raw_tail; /* bits written after the symbols, or NULL */
	int raw_tail_bits;
} CraftBlock;

/* bzip2's CRC, bit by bit (independent of the library's kernels). */
static uint32_t
crc_bits(uint32_t crc, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		crc ^= (uint32_t)p[i] << 24;
		for (int k = 0; k < 8; k++)
			crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u :
						    crc << 1;
	}
	return crc;
}

/*
 * Writes one block; returns the bit position of its CRC field.  Codes are
 * assigned as libbz2's encoder assigns them (by length, then symbol); for an
 * oversubscribed set a code is cut to its length, as a broken encoder would
 * write it.
 */
static size_t
craft_block(BitBuf *b, const CraftBlock *k)
{
	int n_in_use = 0;
	for (int c = 0; c < 256; c++)
		n_in_use += k->in_use[c] != 0;
	const int alpha = n_in_use + 2;
	bb_put(b, 24, 0x314159);
	bb_put(b, 24, 0x265359);
	const size_t crc_pos = b->nbits;
	bb_put(b, 32, 0);
	bb_put(b, 1, (uint32_t)k->randomised);
	bb_put(b, 24, k->orig_ptr);
	uint32_t used16 = 0;
	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 16; j++)
			if (k->in_use[i * 16 + j])
				used16 |= 0x8000u >> i;
	bb_put(b, 16, used16);
	for (int i = 0; i < 16; i++) {
		if (!(used16 & (0x8000u >> i)))
			continue;
		uint32_t bits = 0;
		for (int j = 0; j < 16; j++)
			if (k->in_use[i * 16 + j])
				bits |= 0x8000u >> j;
		bb_put(b, 16, bits);
	}
	bb_put(b, 3, (uint32_t)k->n_groups);
	bb_put(b, 15, (uint32_t)k->n_selectors);
	uint8_t pos[BZ_N_GROUPS] = {0, 1, 2, 3, 4, 5};
	for (int i = 0; i < k->n_selectors; i++) {
		const uint8_t g = k->selectors[i];
		int j = 0;
		while (pos[j] != g)
			j++;
		for (int m = j; m > 0; m--)
			pos[m] = pos[m - 1];
		pos[0] = g;
		for (int m = 0; m < j; m++)
			bb_put(b, 1, 1);
		bb_put(b, 1, 0);
	}
	uint32_t code[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE];
	for (int t = 0; t < k->n_groups; t++) {
		int curr = k->len[t][0];
		bb_put(b, 5, (uint32_t)curr);
		for (int i = 0; i < alpha; i++) {
			while (curr < k->len[t][i]) {
				bb_put(b, 2, 2);
				curr++;
			}
			while (curr > k->len[t][i]) {
				bb_put(b, 2, 3);
				curr--;
			}
			bb_put(b, 1, 0);
		}
		uint32_t vec = 0;
		for (int l = 1; l <= 20; l++) {
			for (int i = 0; i < alpha; i++)
				if (k->len[t][i] == l)
					code[t][i] = vec++;
			vec <<= 1;
		}
	}
	for (size_t i = 0; i < k->n_syms; i++) {
		const int g = k->selectors[i / BZ_G_SIZE];
		const uint16_t sym = k->syms[i];
		const int l = k->len[g][sym];
		bb_put(b, l, code[g][sym] & ((1u << l) - 1));
	}
	for (int i = 0; i < k->raw_tail_bits; i++)
		bb_put(b, 1, (k->raw_tail[i / 8] >> (7 - i % 8)) & 1);
	return crc_pos;
}

/* Rotations of craft_text[0..craft_n), compared cyclically (for qsort). */
static const uint8_t *craft_text;
static size_t craft_n;

static int
rotation_cmp(const void *pa, const void *pb)
{
	const size_t a = *(const size_t *)pa, b = *(const size_t *)pb;
	for (size_t k = 0; k < craft_n; k++) {
		const uint8_t x = craft_text[(a + k) % craft_n],
			      y = craft_text[(b + k) % craft_n];
		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

/*
 * Fills k's symbol map, origPtr and symbols for the block whose last column
 * is l[0..n) (n >= 1), with origPtr op: MTF and RUNA/RUNB coding.  syms needs
 * room for n + 2 symbols.
 */
static void
craft_from_l(CraftBlock *k, const uint8_t *l, size_t n, uint32_t op,
	     uint16_t *syms)
{
	memset(k->in_use, 0, sizeof k->in_use);
	for (size_t i = 0; i < n; i++)
		k->in_use[l[i]] = 1;
	k->orig_ptr = op;
	uint8_t list[256];
	int n_in_use = 0;
	for (int c = 0; c < 256; c++)
		if (k->in_use[c])
			list[n_in_use++] = (uint8_t)c;
	size_t ns = 0;
	uint32_t zeros = 0;
	for (size_t i = 0; i < n; i++) {
		const uint8_t c = l[i];
		int r = 0;
		while (list[r] != c)
			r++;
		if (r == 0) {
			zeros++;
			continue;
		}
		for (uint32_t z = zeros + 1; z > 1; z >>= 1)
			syms[ns++] = (uint16_t)(z & 1); /* RUNA 0, RUNB 1 */
		zeros = 0;
		memmove(list + 1, list, (size_t)r);
		list[0] = c;
		syms[ns++] = (uint16_t)(r + 1);
	}
	for (uint32_t z = zeros + 1; z > 1; z >>= 1)
		syms[ns++] = (uint16_t)(z & 1);
	syms[ns++] = (uint16_t)(n_in_use + 1);
	k->syms = syms;
	k->n_syms = ns;
}

/*
 * Fills k for the RLE1 text t[0..n) (n >= 1): bzip2's BWT by sorting
 * rotations, then craft_from_l.  syms needs room for n + 2 symbols.
 */
static void
craft_from_text(CraftBlock *k, const uint8_t *t, size_t n, uint16_t *syms)
{
	size_t *rot = malloc(n * sizeof(size_t));
	uint8_t *l = malloc(n);
	uint32_t op = 0;
	for (size_t i = 0; i < n; i++)
		rot[i] = i;
	craft_text = t;
	craft_n = n;
	qsort(rot, n, sizeof(size_t), rotation_cmp);
	for (size_t i = 0; i < n; i++) {
		if (rot[i] == 0)
			op = (uint32_t)i;
		l[i] = t[(rot[i] + n - 1) % n];
	}
	craft_from_l(k, l, n, op, syms);
	free(l);
	free(rot);
}

/*
 * The n + 1 bytes libbz2's walk gives for last column l[0..n) from origPtr
 * op: the block's text and then the position the walk goes on to.
 */
static void
craft_walk(const uint8_t *l, size_t n, uint32_t op, uint8_t *text)
{
	uint32_t cf[256] = {0};
	uint32_t *tt = malloc(n * sizeof *tt);
	for (size_t i = 0; i < n; i++)
		cf[l[i]]++;
	for (uint32_t c = 0, sum = 0; c < 256; c++) {
		const uint32_t m = cf[c];
		cf[c] = sum;
		sum += m;
	}
	for (size_t i = 0; i < n; i++)
		tt[cf[l[i]]++] = (uint32_t)i;
	uint32_t p = tt[op];
	for (size_t j = 0; j <= n; j++) {
		text[j] = l[p];
		p = tt[p];
	}
	free(tt);
}

/* Whether RLE1 text t[0..n) ends with four equal bytes and no count byte. */
static int
ends_without_count(const uint8_t *t, size_t n)
{
	uint8_t last = 0;
	int eq = 0;
	for (size_t i = 0; i < n; i++) {
		if (eq && t[i] == last) {
			eq++;
		} else {
			last = t[i];
			eq = 1;
		}
		if (eq == 4) {
			if (i + 1 == n)
				return 1;
			i++; /* the count byte */
			eq = 0;
		}
	}
	return 0;
}

/* Code lengths that give every symbol of alpha a code (a complete code
 * when alpha is a power of two). */
static void
craft_flat_lengths(CraftBlock *k, int alpha)
{
	int l = 1;
	while ((1 << l) < alpha)
		l++;
	for (int t = 0; t < BZ_N_GROUPS; t++)
		for (int i = 0; i < alpha; i++)
			k->len[t][i] = (uint8_t)l;
}

/*
 * Writes the stream 'BZh<digit>', the blocks and the trailer, then fills in
 * the CRCs of the first n_good blocks (and the stream CRC if all are good)
 * from libbz2's decoding: each block's CRC field is set from the bytes libbz2
 * delivers before it reports that block's CRC as wrong.
 */
static void
craft_stream(const char *name, int digit, const CraftBlock *blocks,
	     int n_blocks, int n_good)
{
	BitBuf b = {NULL, 0, 0};
	size_t crc_pos[8];
	bb_put(&b, 8, 'B');
	bb_put(&b, 8, 'Z');
	bb_put(&b, 8, 'h');
	bb_put(&b, 8, (uint32_t)('0' + digit));
	for (int i = 0; i < n_blocks; i++)
		crc_pos[i] = craft_block(&b, &blocks[i]);
	bb_put(&b, 24, 0x177245);
	bb_put(&b, 24, 0x385090);
	const size_t stream_crc_pos = b.nbits;
	bb_put(&b, 32, 0);
	while (b.nbits % 8)
		bb_put(&b, 1, 0);
	const size_t len = b.nbits / 8;
	const size_t out_cap = 1 << 20;
	uint8_t *out = malloc(out_cap);
	uint32_t combined = 0;
	size_t start = 0;
	for (int i = 0; i < n_good; i++) {
		bz_stream z;
		memset(&z, 0, sizeof z);
		ref_BZ2_bzDecompressInit(&z, 0, 0);
		z.next_in = (char *)b.p;
		z.avail_in = (unsigned)len;
		z.next_out = (char *)out;
		z.avail_out = (unsigned)out_cap;
		ref_BZ2_bzDecompress(&z);
		const size_t end = out_cap - z.avail_out;
		ref_BZ2_bzDecompressEnd(&z);
		const uint32_t crc =
			~crc_bits(0xffffffffu, out + start, end - start);
		bb_set(&b, crc_pos[i], 32, crc);
		combined = ((combined << 1) | (combined >> 31)) ^ crc;
		start = end;
	}
	if (n_good == n_blocks)
		bb_set(&b, stream_crc_pos, 32, combined);
	/* What libbz2 delivers, which sizes the output splits. */
	bz_stream z;
	memset(&z, 0, sizeof z);
	ref_BZ2_bzDecompressInit(&z, 0, 0);
	z.next_in = (char *)b.p;
	z.avail_in = (unsigned)len;
	z.next_out = (char *)out;
	z.avail_out = (unsigned)out_cap;
	ref_BZ2_bzDecompress(&z);
	ref_BZ2_bzDecompressEnd(&z);
	add_raw_stream(name, out, out_cap - z.avail_out, b.p, len);
	free(out);
	free(b.p);
}

/* The crafted streams; each is then run under every schedule. */
static void
gen_crafted(void)
{
	static uint16_t syms[3][8000];
	static uint8_t sel_one[1] = {0};
	static uint8_t zero_sels[200];
	static uint8_t sels[32767];
	CraftBlock k;
	char name[64];

	/*
	 * RLE1 text ending in four equal bytes with no count byte (libbz2's
	 * one corrupt-output case): short, and long enough for the bulk
	 * un-RLE1 path, plain and randomised
	 */
	static const size_t text_lens[] = {4, 7, 40, 3000};
	for (size_t v = 0; v < 4; v++) {
		uint8_t t[3004];
		const size_t n = text_lens[v];
		for (size_t i = 0; i + 4 < n; i++)
			t[i] = (uint8_t)("abcdefg"[(i * 5 + i / 7) % 7]);
		memset(t + (n >= 4 ? n - 4 : 0), 'q', 4);
		for (int randomised = 0; randomised <= 1; randomised++) {
			memset(&k, 0, sizeof k);
			craft_from_text(&k, t, n, syms[0]);
			k.randomised = randomised;
			k.n_groups = 2;
			k.n_selectors = (int)((k.n_syms + 49) / 50);
			memset(sels, 0, sizeof sels);
			k.selectors = sels;
			int alpha = 2;
			for (int c = 0; c < 256; c++)
				alpha += k.in_use[c];
			craft_flat_lengths(&k, alpha);
			snprintf(name, sizeof name,
				 "crafted/four_equal_tail_%zu%s", n,
				 randomised ? "_rand" : "");
			craft_stream(name, 1, &k, 1, 0);
		}
	}

	/*
	 * Last columns that are the BWT of no text: their rows form more than
	 * one cycle, and the walk from origPtr goes round a cycle shorter than
	 * the block, on past where it started.  Each is drawn until its text
	 * ends with four equal bytes and no count byte, and libbz2 takes the
	 * count from where the walk goes on, which is not the text's first
	 * byte.  Short, and long enough for the bulk un-RLE1 path, plain and
	 * randomised.
	 */
	static const size_t cycle_lens[] = {6, 9, 600, 3000};
	uint64_t cs = 0x2545F4914F6CDD1Dull;
	for (size_t v = 0; v < 4; v++) {
		uint8_t l[3000], t[3001];
		const size_t n = cycle_lens[v];
		uint32_t op;
		do {
			for (size_t i = 0; i < n; i++) {
				cs ^= cs << 13;
				cs ^= cs >> 7;
				cs ^= cs << 17;
				l[i] = (uint8_t)("qqqab"[cs % 5]);
			}
			op = (uint32_t)((cs >> 32) % n);
			craft_walk(l, n, op, t);
		} while (!ends_without_count(t, n) || t[n] == t[0]);
		for (int randomised = 0; randomised <= 1; randomised++) {
			memset(&k, 0, sizeof k);
			craft_from_l(&k, l, n, op, syms[0]);
			k.randomised = randomised;
			k.n_groups = 2;
			k.n_selectors = (int)((k.n_syms + 49) / 50);
			memset(sels, 0, sizeof sels);
			k.selectors = sels;
			int alpha = 2;
			for (int c = 0; c < 256; c++)
				alpha += k.in_use[c];
			craft_flat_lengths(&k, alpha);
			snprintf(name, sizeof name, "crafted/cycles_tail_%zu%s",
				 n, randomised ? "_rand" : "");
			craft_stream(name, 1, &k, 1, 0);
		}
	}

	/*
	 * Runs of RUNA/RUNB symbols around libbz2's limit (an error once the
	 * run's weight reaches 2^21) and past the block size
	 */
	static const int run_lens[] = {18, 19, 20, 21, 22, 25};
	for (size_t v = 0; v < 6; v++)
		for (int kind = 0; kind < 3; kind++) {
			memset(&k, 0, sizeof k);
			k.in_use[0] = 1; /* runs of 0: count bytes 0 */
			size_t ns = 0;
			for (int i = 0; i < run_lens[v]; i++)
				syms[0][ns++] =
					(uint16_t)(kind == 2 ? (i & 1) : kind);
			syms[0][ns++] = 2; /* EOB */
			k.syms = syms[0];
			k.n_syms = ns;
			k.n_groups = 2;
			k.n_selectors = 1;
			k.selectors = sel_one;
			craft_flat_lengths(&k, 3);
			snprintf(name, sizeof name, "crafted/run_%d_%s",
				 run_lens[v],
				 kind == 0 ? "runa" :
				 kind == 1 ? "runb" :
					     "mixed");
			craft_stream(name, 9, &k, 1, 0);
		}

	/*
	 * More selectors than libbz2 1.0.8 keeps (18,002; the rest are read
	 * and ignored), then a second, ordinary block
	 */
	static const int n_sels[] = {18001, 18002, 18003, 18010, 32767};
	for (size_t v = 0; v < 5; v++) {
		CraftBlock two[2];
		uint8_t t1[500], t2[300];
		for (size_t i = 0; i < sizeof t1; i++)
			t1[i] = (uint8_t)("hello, world "[i % 13]);
		for (size_t i = 0; i < sizeof t2; i++)
			t2[i] = (uint8_t)('A' + (i * 7) % 26);
		memset(two, 0, sizeof two);
		craft_from_text(&two[0], t1, sizeof t1, syms[0]);
		craft_from_text(&two[1], t2, sizeof t2, syms[1]);
		for (int i = 0; i < n_sels[v]; i++)
			sels[i] = (uint8_t)(i < 20 ? i % 3 : (i * 7) % 3);
		two[0].n_groups = 3;
		two[0].n_selectors = n_sels[v];
		two[0].selectors = sels;
		int alpha = 2;
		for (int c = 0; c < 256; c++)
			alpha += two[0].in_use[c];
		craft_flat_lengths(&two[0], alpha);
		two[1].n_groups = 2;
		two[1].n_selectors = (int)((two[1].n_syms + 49) / 50);
		two[1].selectors = zero_sels;
		alpha = 2;
		for (int c = 0; c < 256; c++)
			alpha += two[1].in_use[c];
		craft_flat_lengths(&two[1], alpha);
		snprintf(name, sizeof name, "crafted/selectors_%d", n_sels[v]);
		craft_stream(name, 9, two, 2, 2);
	}

	/*
	 * Code-length sets that are not complete prefix codes: every symbol at
	 * length 1 (oversubscribed), a set with one pattern unused
	 * (incomplete), both with and without a pattern that is no code
	 */
	{
		uint8_t t[200];
		for (size_t i = 0; i < sizeof t; i++)
			t[i] = (uint8_t)("ab"[(i * i / 3) % 2]);
		for (int variant = 0; variant < 5; variant++) {
			memset(&k, 0, sizeof k);
			craft_from_text(&k, t, sizeof t, syms[0]);
			k.n_groups = 2;
			k.n_selectors = (int)((k.n_syms + 49) / 50);
			memset(sels, 0, sizeof sels);
			k.selectors = sels;
			const int alpha = 4; /* RUNA RUNB 2 EOB */
			static const uint8_t no_code[2] = {0xff, 0xff};
			const char *what;
			switch (variant) {
			case 0: /* all length 1: oversubscribed */
				for (int i = 0; i < alpha; i++)
					k.len[0][i] = k.len[1][i] = 1;
				what = "oversubscribed_len1";
				break;
			case 1: /* lengths 1, 2, 2, 2: oversubscribed */
				k.len[0][0] = k.len[1][0] = 1;
				for (int i = 1; i < alpha; i++)
					k.len[0][i] = k.len[1][i] = 2;
				what = "oversubscribed_122";
				break;
			case 2: /* 2, 2, 2, 3: '111' unused */
				for (int i = 0; i < alpha; i++)
					k.len[0][i] = k.len[1][i] =
						(uint8_t)(i < 3 ? 2 : 3);
				what = "incomplete";
				break;
			case 3: /* as 2, then the unused pattern */
				for (int i = 0; i < alpha; i++)
					k.len[0][i] = k.len[1][i] =
						(uint8_t)(i < 3 ? 2 : 3);
				k.n_syms = 10;
				k.raw_tail = no_code;
				k.raw_tail_bits = 16;
				what = "incomplete_no_code";
				break;
			default: /* lengths 1 and 20 */
				k.len[0][0] = k.len[1][0] = 1;
				for (int i = 1; i < alpha; i++)
					k.len[0][i] = k.len[1][i] = 20;
				what = "lengths_1_and_20";
				break;
			}
			snprintf(name, sizeof name, "crafted/%s", what);
			craft_stream(name, 1, &k, 1,
				     variant == 2 || variant == 4);
		}
	}

	/*
	 * Code-length sets in long runs of equal lengths: every byte value in
	 * use (258 symbols), six groups of which the selectors name only two
	 * (one of the others oversubscribed), and lengths that leave 1..20 in
	 * the middle of a set, going down and going up
	 */
	{
		uint8_t t[3000];
		for (size_t i = 0; i < sizeof t; i++)
			t[i] = (uint8_t)(i * 7 + i / 256);
		for (int variant = 0; variant < 3; variant++) {
			memset(&k, 0, sizeof k);
			craft_from_text(&k, t, sizeof t, syms[0]);
			int alpha = 2;
			for (int c = 0; c < 256; c++)
				alpha += k.in_use[c];
			k.n_groups = 6;
			k.n_selectors = (int)((k.n_syms + 49) / 50);
			for (int i = 0; i < k.n_selectors; i++)
				sels[i] = (uint8_t)(i % 3 == 0 ? 5 : 2);
			k.selectors = sels;
			for (int g = 0; g < BZ_N_GROUPS; g++)
				for (int i = 0; i < alpha; i++)
					k.len[g][i] = 9;
			/* group 5: runs of 1, 2, 3, ... lengths of 9 to 12 */
			for (int i = 0, r = 1; i < alpha; r++)
				for (int m = 0; m < r && i < alpha; m++, i++)
					k.len[5][i] = (uint8_t)(9 + r % 4);
			for (int i = 0; i < alpha; i++)
				k.len[1][i] = 1;
			const char *what = "length_runs_unused_groups";
			if (variant == 1) {
				k.len[3][200] = 0;
				what = "length_below_1";
			} else if (variant == 2) {
				k.len[4][130] = 21;
				what = "length_above_20";
			}
			snprintf(name, sizeof name, "crafted/%s", what);
			craft_stream(name, 9, &k, 1, variant == 0);
		}
	}
}

/* Every input split, every output split and the fixed chunkings, both
 * small settings, for the crafted streams. */
static void
phase_crafted(void)
{
	uint64_t r0 = n_runs, f0 = n_fail, c0 = n_calls;
	rs = seed;
	const size_t first = n_streams;
	gen_crafted();
	for (size_t i = first; i < n_streams; i++) {
		const Stream *st = &streams[i];
		const size_t out_cap = st->raw_len * 2 + 8192;
		Case c = {st->name, st->comp, st->comp_len, out_cap, 0};
		static const size_t chunks[] = {1, 3, 64, SIZE_MAX};
		for (size_t x = 0; x < 4; x++)
			for (size_t y = 0; y < 4; y++) {
				if (chunks[y] < 64 && st->raw_len > 20000)
					continue;
				Sched f = {SCH_FIXED, chunks[x], chunks[y], 0};
				run_all_modes(&c, &f);
			}
		if (!quiet) {
			/* what libbz2 makes of the stream */
			Sched whole = {SCH_FIXED, SIZE_MAX, SIZE_MAX, 0};
			c.small = 0;
			const uint64_t calls0 = n_calls;
			const int rc = run_case(&c, &whole);
			printf("  %-34s %6zu bytes -> %7zu delivered, final rc "
			       "%d "
			       "(%llu calls)\n",
			       st->name, st->comp_len, st->raw_len, rc,
			       (unsigned long long)(n_calls - calls0));
		}
		for (size_t k = 0; k <= st->comp_len; k++) {
			Sched f = {SCH_SPLIT_IN, k, 0, 0};
			run_all_modes(&c, &f);
		}
		/*
		 * Every output split, or about 400 spread over the output when
		 * it is long (some crafted runs expand to hundreds of
		 * kilobytes)
		 */
		const size_t out_end = st->raw_len + 300 < out_cap ?
					       st->raw_len + 300 :
					       out_cap;
		const size_t step = out_end > 5000 ? out_end / 400 : 1;
		for (size_t k = 0; k <= out_end; k += step) {
			Sched f = {SCH_SPLIT_OUT, k, 0, 0};
			run_all_modes(&c, &f);
		}
	}
	printf("crafted: %zu streams, %llu runs, %llu calls, %llu "
	       "mismatches\n",
	       n_streams - first, (unsigned long long)(n_runs - r0),
	       (unsigned long long)(n_calls - c0),
	       (unsigned long long)(n_fail - f0));
}

/* ------------------------------------------------------- output slack */

/*
 * Streams whose blocks end where the bulk un-RLE1 kernel's whole-vector
 * stores could leave bytes modified past what is delivered: literals that put
 * a run at every offset of a kernel step, a run of every length from 4 to 40
 * (so every remainder of the vector fill), then zero to three runs of four
 * (five text bytes, four output bytes each) before the block ends.  Then
 * random texts of literals and runs of 4 to 43.  Each is decoded whole and
 * in output pieces of several sizes, both small settings; the buffer
 * comparison fails on any byte left modified past next_out.
 */
static void
phase_slack(void)
{
	uint64_t r0 = n_runs, f0 = n_fail, c0 = n_calls;
	rs = seed;
	const size_t first = n_streams;
	uint8_t t[3200];
	char name[64];

	for (int lit = 16; lit < 32; lit++)
		for (int run = 4; run <= 40; run++)
			for (int fours = 0; fours <= 3; fours++) {
				size_t n = 0;
				for (int i = 0; i < lit; i++)
					t[n++] = (uint8_t)('a' + i % 23);
				memset(t + n, 'X', (size_t)run);
				n += (size_t)run;
				for (int f = 0; f < fours; f++) {
					memset(t + n, 'p' + f, 4);
					n += 4;
				}
				snprintf(name, sizeof name,
					 "slack/lit%d_run%d_fours%d", lit, run,
					 fours);
				add_stream(name, t, n, 1, 0);
			}
	for (int k = 0; k < 300; k++) {
		const size_t len = 50 + (size_t)(rnd() % 3000);
		size_t n = 0;
		while (n < len) {
			const uint64_t r = rnd();
			size_t m = r & 1 ? 1 : 4 + (size_t)((r >> 8) % 40);
			if (m > len - n)
				m = len - n;
			memset(t + n, 'a' + (int)((r >> 16) % 6), m);
			n += m;
		}
		snprintf(name, sizeof name, "slack/random_%d", k);
		add_stream(name, t, n, 1, 0);
	}

	for (size_t i = first; i < n_streams; i++) {
		const Stream *st = &streams[i];
		Case c = {st->name, st->comp, st->comp_len, st->raw_len + 64,
			  0};
		static const size_t outs[] = {SIZE_MAX, 1,  5,	 16,
					      17,	33, 100, 297};
		for (size_t y = 0; y < sizeof outs / sizeof outs[0]; y++) {
			Sched f = {SCH_FIXED, SIZE_MAX, outs[y], 0};
			run_all_modes(&c, &f);
		}
		for (int k = 0; k < 2; k++) {
			Sched f = {SCH_RANDOM, st->comp_len, 400, rnd()};
			run_all_modes(&c, &f);
		}
	}
	printf("slack: %zu streams, %llu runs, %llu calls, %llu "
	       "mismatches\n",
	       n_streams - first, (unsigned long long)(n_runs - r0),
	       (unsigned long long)(n_calls - c0),
	       (unsigned long long)(n_fail - f0));
}

static void
phase_valid(void)
{
	uint64_t r0 = n_runs, f0 = n_fail, c0 = n_calls;
	rs = seed;
	for (size_t i = 0; i < n_streams; i++) {
		const Stream *s = &streams[i];
		Case c = {s->name, s->comp, s->comp_len, s->raw_len + 64, 0};
		Sched sc = {SCH_FIXED, SIZE_MAX, SIZE_MAX, 0};
		run_all_modes(&c, &sc);
		/* exact-size and short output buffers */
		Case cs = c;
		cs.out_cap = s->raw_len;
		run_all_modes(&cs, &sc);
		if (s->raw_len > 0) {
			cs.out_cap = s->raw_len - 1;
			run_all_modes(&cs, &sc);
		}
		static const size_t chunks[] = {1, 2, 3, 7, 64, 5000};
		const int big = s->comp_len > 300000 || s->raw_len > 3000000;
		for (size_t x = 0; x < 6; x++)
			for (size_t y = 0; y < 6; y++) {
				if (big && chunks[x] < 64 && chunks[y] < 64 &&
				    !(x == 0 && y == 5))
					continue;
				Sched f = {SCH_FIXED, chunks[x], chunks[y], 0};
				c.small = (int)((x + y) & 1);
				run_case(&c, &f);
			}
		for (int k = 0; k < (big ? 2 : 8); k++) {
			Sched f = {SCH_RANDOM, s->comp_len, s->raw_len + 1,
				   rnd()};
			run_all_modes(&c, &f);
		}
		/*
		 * every input split point for small streams, every output split
		 * for small outputs
		 */
		if (s->comp_len <= 4000) {
			for (size_t k = 0; k <= s->comp_len; k++) {
				Sched f = {SCH_SPLIT_IN, k, 0, 0};
				c.small = (int)(k & 1);
				run_case(&c, &f);
			}
		}
		if (s->raw_len <= 20000) {
			for (size_t k = 0; k <= s->raw_len; k++) {
				Sched f = {SCH_SPLIT_OUT, k, 0, 0};
				c.small = (int)(k & 1);
				run_case(&c, &f);
			}
		}
	}
	printf("valid: %llu runs, %llu calls, %llu mismatches\n",
	       (unsigned long long)(n_runs - r0),
	       (unsigned long long)(n_calls - c0),
	       (unsigned long long)(n_fail - f0));
}

/* Decodes a mutated input with a few schedules and both modes. */
static void
mutated_case(const char *name, const uint8_t *in, size_t len, size_t out_cap)
{
	Case c = {name, in, len, out_cap, 0};
	Sched whole = {SCH_FIXED, SIZE_MAX, SIZE_MAX, 0};
	Sched bytewise = {SCH_FIXED, 1, SIZE_MAX, 0};
	Sched outbyte = {SCH_FIXED, SIZE_MAX, 7, 0};
	Sched rnd_s = {SCH_RANDOM, len, out_cap, rnd()};
	run_all_modes(&c, &whole);
	c.small = (int)(rnd() & 1);
	run_case(&c, (rnd() & 1) ? &bytewise : &outbyte);
	c.small = (int)(rnd() & 1);
	run_case(&c, &rnd_s);
}

static void
phase_mutate(unsigned long n_random)
{
	uint64_t r0 = n_runs, f0 = n_fail, a0 = n_accept, j0 = n_reject;
	rs = seed;
	uint8_t *buf = malloc(4 << 20);
	/* small seeds: every bit flipped, every truncation */
	size_t n_small = 0;
	uint64_t flips = 0, truncs = 0, targeted = 0, randoms = 0;
	for (size_t i = 0; i < n_streams; i++) {
		const Stream *s = &streams[i];
		if (s->comp_len > 700)
			continue;
		n_small++;
		for (size_t bit = 0; bit < s->comp_len * 8; bit++) {
			memcpy(buf, s->comp, s->comp_len);
			buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
			mutated_case(s->name, buf, s->comp_len,
				     s->raw_len * 2 + 4096);
			flips++;
		}
	}
	for (size_t i = 0; i < n_streams; i++) {
		const Stream *s = &streams[i];
		if (s->comp_len > 20000)
			continue;
		for (size_t k = 0; k < s->comp_len; k++) {
			Case c = {s->name, s->comp, k, s->raw_len + 64, 0};
			Sched whole = {SCH_FIXED, SIZE_MAX, SIZE_MAX, 0};
			c.small = (int)(k & 1);
			run_case(&c, &whole);
			truncs++;
		}
	}
	/*
	 * targeted: overwrite each of the first 64 bytes (headers, CRC,
	 * origPtr, bitmap, selectors) and the last 12 (end marker and combined
	 * CRC) with a few values
	 */
	for (size_t i = 0; i < n_streams; i++) {
		const Stream *s = &streams[i];
		if (s->comp_len > 3000000)
			continue;
		static const uint8_t vals[] = {0x00, 0xff, 0x80, 0x01, 0x7f};
		for (size_t pos = 0; pos < s->comp_len && pos < 64; pos++)
			for (size_t v = 0; v < sizeof vals; v++) {
				memcpy(buf, s->comp,
				       s->comp_len < (4u << 20) ? s->comp_len :
								  0);
				if (s->comp_len > (4u << 20))
					break;
				buf[pos] = vals[v];
				Case c = {s->name, buf, s->comp_len,
					  s->raw_len + 64, (int)(v & 1)};
				Sched whole = {SCH_FIXED, SIZE_MAX, SIZE_MAX,
					       0};
				run_case(&c, &whole);
				targeted++;
			}
		for (size_t back = 1; back <= 12 && back <= s->comp_len;
		     back++) {
			if (s->comp_len > (4u << 20))
				break;
			memcpy(buf, s->comp, s->comp_len);
			buf[s->comp_len - back] ^= 0x5a;
			Case c = {s->name, buf, s->comp_len, s->raw_len + 64,
				  (int)(back & 1)};
			Sched whole = {SCH_FIXED, SIZE_MAX, SIZE_MAX, 0};
			run_case(&c, &whole);
			targeted++;
		}
	}
	/* n_random random mutations */
	while (randoms < n_random) {
		{
			const Stream *s = &streams[rnd() % n_streams];
			if (s->comp_len > 400000 || s->comp_len < 4)
				continue;
			size_t len = s->comp_len;
			memcpy(buf, s->comp, len);
			const int nm = 1 + (int)(rnd() % 4);
			for (int m = 0; m < nm; m++) {
				const size_t pos = rnd() % len;
				switch (rnd() % 7) {
				case 0:
					buf[pos] ^=
						(uint8_t)(1u << (rnd() % 8));
					break;
				case 1:
					buf[pos] = (uint8_t)rnd();
					break;
				case 2:
					len = pos + 1;
					break;
				case 3:
					if (len + 1 < (4u << 20)) {
						memmove(buf + pos + 1,
							buf + pos, len - pos);
						buf[pos] = (uint8_t)rnd();
						len++;
					}
					break;
				case 4:
					if (len > 1) {
						memmove(buf + pos,
							buf + pos + 1,
							len - pos - 1);
						len--;
					}
					break;
				case 5: {
					const size_t run = 1 + rnd() % 16;
					for (size_t k = 0;
					     k < run && pos + k < len; k++)
						buf[pos + k] =
							(rnd() & 1) ? 0 : 0xff;
					break;
				}
				default: {
					const Stream *o =
						&streams[rnd() % n_streams];
					const size_t from =
						rnd() %
						(o->comp_len ? o->comp_len : 1);
					size_t cnt = 1 + rnd() % 64;
					if (from + cnt > o->comp_len)
						cnt = o->comp_len - from;
					if (pos + cnt > len)
						cnt = len - pos;
					memcpy(buf + pos, o->comp + from, cnt);
					break;
				}
				}
			}
			mutated_case(s->name, buf, len, s->raw_len * 2 + 65536);
			randoms++;
		}
	}
	free(buf);
	printf("mutate: %zu small seeds; %llu bit flips, %llu truncations, "
	       "%llu targeted, %llu random inputs; "
	       "%llu runs (%llu accepted, %llu rejected), %llu mismatches\n",
	       n_small, (unsigned long long)flips, (unsigned long long)truncs,
	       (unsigned long long)targeted, (unsigned long long)randoms,
	       (unsigned long long)(n_runs - r0),
	       (unsigned long long)(n_accept - a0),
	       (unsigned long long)(n_reject - j0),
	       (unsigned long long)(n_fail - f0));
}

/*
 * Reads a file of concatenated streams the way bzip2.c's uncompressStream
 * does (BZ2_bzRead in 5000-byte requests; at each stream end the unused
 * bytes are carried into the next BZ2_bzReadOpen) and records every
 * observable result.
 */
typedef struct {
	uint8_t *out;
	size_t out_len;
	int final_err;
	int streams;
	char log[4096];
	size_t log_len;
} ReadResult;

#define READ_IMPL(PFX, NAME)                                                  \
	static void NAME(FILE *f, int small, ReadResult *rr)                  \
	{                                                                     \
		uint8_t unused[BZ_MAX_UNUSED];                                \
		int n_unused = 0, bzerr = 0;                                  \
		rr->out_len = 0;                                              \
		rr->streams = 0;                                              \
		rr->log_len = 0;                                              \
		for (;;) {                                                    \
			BZFILE *bz = PFX##BZ2_bzReadOpen(&bzerr, f, 0, small, \
							 unused, n_unused);   \
			if (bz == NULL || bzerr != BZ_OK)                     \
				break;                                        \
			rr->streams++;                                        \
			while (bzerr == BZ_OK) {                              \
				const int nb = PFX##BZ2_bzRead(               \
					&bzerr, bz, rr->out + rr->out_len,    \
					5000);                                \
				if (rr->log_len < sizeof rr->log - 32)        \
					rr->log_len += (size_t)snprintf(      \
						rr->log + rr->log_len,        \
						sizeof rr->log - rr->log_len, \
						"%d:%d ", nb, bzerr);         \
				if ((bzerr == BZ_OK ||                        \
				     bzerr == BZ_STREAM_END) &&               \
				    nb > 0)                                   \
					rr->out_len += (size_t)nb;            \
			}                                                     \
			if (bzerr != BZ_STREAM_END) {                         \
				int e2;                                       \
				PFX##BZ2_bzReadClose(&e2, bz);                \
				break;                                        \
			}                                                     \
			void *u;                                              \
			PFX##BZ2_bzReadGetUnused(&bzerr, bz, &u, &n_unused);  \
			if (bzerr != BZ_OK)                                   \
				break;                                        \
			memcpy(unused, u, (size_t)n_unused);                  \
			PFX##BZ2_bzReadClose(&bzerr, bz);                     \
			if (n_unused == 0 && feof(f)) {                       \
				bzerr = BZ_STREAM_END;                        \
				break;                                        \
			}                                                     \
			if (n_unused == 0) {                                  \
				int ch = fgetc(f);                            \
				if (ch == EOF) {                              \
					bzerr = BZ_STREAM_END;                \
					break;                                \
				}                                             \
				ungetc(ch, f);                                \
			}                                                     \
		}                                                             \
		rr->final_err = bzerr;                                        \
	}

READ_IMPL(ref_, read_ref)
READ_IMPL(, read_ng)

static void
phase_read(void)
{
	uint64_t f0 = n_fail, cases = 0;
	rs = seed;
#ifdef _WIN32
	/* Windows has no /tmp; TEMP is set for every process. */
	const char *dir = getenv("TEMP");
	if (dir == NULL)
		dir = ".";
#else
	const char *dir = "/tmp";
#endif
	char path[1024];
	snprintf(path, sizeof path, "%s/bzng_difftest_XXXXXX", dir);
	int fd = mkstemp(path);
	if (fd < 0) {
		perror("mkstemp");
		exit(2);
	}
	close(fd);
	size_t maxraw = 0;
	for (size_t i = 0; i < n_streams; i++)
		if (streams[i].raw_len > maxraw)
			maxraw = streams[i].raw_len;
	ReadResult A, Bq;
	const size_t cap = 4 * maxraw + 10000000;
	A.out = malloc(cap);
	Bq.out = malloc(cap);
	if (A.out == NULL || Bq.out == NULL) {
		fprintf(stderr, "out of memory (read phase)\n");
		exit(2);
	}
	for (int iter = 0; iter < 400; iter++) {
		/*
		 * 1-4 streams concatenated, optional trailing garbage or a
		 * truncated last stream
		 */
		FILE *f = fopen(path, "wb");
		const int ns = 1 + (int)(rnd() % 4);
		size_t total_raw = 0;
		for (int k = 0; k < ns; k++) {
			const Stream *s = &streams[rnd() % n_streams];
			if (s->raw_len > 2500000) {
				k--;
				continue;
			}
			total_raw += s->raw_len;
			fwrite(s->comp, 1, s->comp_len, f);
		}
		const int tail = (int)(rnd() % 4);
		if (tail == 1) {
			const char *g = "trailing garbage";
			fwrite(g, 1, strlen(g), f);
		}
		if (tail == 2) {
			const Stream *s = &streams[rnd() % n_streams];
			fwrite(s->comp, 1, s->comp_len / 2, f);
		}
		fclose(f);
		if (total_raw * 2 + 100000 > cap)
			continue;
		const int small = (int)(rnd() & 1);
		/*
		 * Everything BZ2_bzRead can write: the streams, a truncated
		 * last stream's output and the last 5,000-byte request
		 */
		const size_t used = total_raw + maxraw + 10000;
		fill_pattern(A.out, Bq.out, used, (uint64_t)iter);
		f = fopen(path, "rb");
		read_ref(f, small, &A);
		fclose(f);
		f = fopen(path, "rb");
		read_ng(f, small, &Bq);
		fclose(f);
		cases++;
		if (A.final_err != Bq.final_err || A.streams != Bq.streams ||
		    A.out_len != Bq.out_len ||
		    first_difference(A.out, Bq.out, 0, used) < used ||
		    A.log_len != Bq.log_len ||
		    memcmp(A.log, Bq.log, A.log_len) != 0) {
			n_fail++;
			fprintf(stderr,
				"bzRead MISMATCH iter %d: err %d/%d streams "
				"%d/%d out %zu/%zu\n",
				iter, A.final_err, Bq.final_err, A.streams,
				Bq.streams, A.out_len, Bq.out_len);
		}
	}
	/*
	 * BZ2_bzBuffToBuffDecompress with exact, short and zero output sizes,
	 * and on inputs with trailing bytes
	 */
	uint64_t btb = 0;
	for (size_t i = 0; i < n_streams; i++) {
		const Stream *s = &streams[i];
		const size_t sizes[4] = {s->raw_len,
					 s->raw_len ? s->raw_len - 1 : 0, 0,
					 s->raw_len + 100};
		for (int k = 0; k < 4; k++)
			for (int small = 0; small <= 1; small++) {
				unsigned la = (unsigned)sizes[k],
					 lb = (unsigned)sizes[k];
				const size_t region = sizes[k] + OUT_SLACK;
				fill_pattern(A.out, Bq.out, region, btb);
				const int ra = ref_BZ2_bzBuffToBuffDecompress(
					(char *)A.out, &la, (char *)s->comp,
					(unsigned)s->comp_len, small, 0);
				const int rb = BZ2_bzBuffToBuffDecompress(
					(char *)Bq.out, &lb, (char *)s->comp,
					(unsigned)s->comp_len, small, 0);
				btb++;
				if (ra != rb || (ra == BZ_OK && la != lb) ||
				    first_difference(A.out, Bq.out, 0, region) <
					    region) {
					n_fail++;
					fprintf(stderr,
						"BuffToBuff MISMATCH %s size "
						"%zu small %d: %d/%d\n",
						s->name, sizes[k], small, ra,
						rb);
				}
			}
	}
	free(A.out);
	free(Bq.out);
	unlink(path);
	printf("read: %llu concatenated-file cases through BZ2_bzRead, %llu "
	       "BZ2_bzBuffToBuffDecompress calls, %llu mismatches\n",
	       (unsigned long long)cases, (unsigned long long)btb,
	       (unsigned long long)(n_fail - f0));
}

static void
phase_alloc(void)
{
	uint64_t f0 = n_fail, cases = 0;
	rs = seed;
	for (size_t i = 0; i < n_streams && i < 40; i++) {
		const Stream *s = &streams[i];
		for (int small = 0; small <= 1; small++)
			for (long fail_at = 1; fail_at <= 4; fail_at++) {
				AllocCount ca = {0, fail_at, 0, 0},
					   cb = {0, fail_at, 0, 0};
				bz_stream a, b;
				memset(&a, 0, sizeof a);
				memset(&b, 0, sizeof b);
				a.bzalloc = b.bzalloc = count_alloc;
				a.bzfree = b.bzfree = count_free;
				a.opaque = &ca;
				b.opaque = &cb;
				ensure_out(s->raw_len + 64);
				int ra = ref_BZ2_bzDecompressInit(&a, 0, small);
				int rb = BZ2_bzDecompressInit(&b, 0, small);
				cases++;
				if (ra != rb) {
					n_fail++;
					fprintf(stderr,
						"alloc MISMATCH init %s "
						"fail_at %ld: %d/%d\n",
						s->name, fail_at, ra, rb);
					continue;
				}
				if (ra != BZ_OK)
					continue;
				a.next_in = b.next_in = (char *)s->comp;
				a.avail_in = b.avail_in = (unsigned)s->comp_len;
				a.next_out = (char *)out_a;
				b.next_out = (char *)out_b;
				a.avail_out = b.avail_out =
					(unsigned)(s->raw_len + 64);
				fill_pattern(out_a, out_b,
					     s->raw_len + 64 + OUT_SLACK,
					     (uint64_t)fail_at);
				ra = ref_BZ2_bzDecompress(&a);
				rb = BZ2_bzDecompress(&b);
				if (ra != rb || a.avail_in != b.avail_in ||
				    a.avail_out != b.avail_out ||
				    first_difference(out_a, out_b, 0,
						     s->raw_len + 64 +
							     OUT_SLACK) <
					    s->raw_len + 64 + OUT_SLACK) {
					n_fail++;
					fprintf(stderr,
						"alloc MISMATCH %s fail_at "
						"%ld: %d/%d\n",
						s->name, fail_at, ra, rb);
				}
				ref_BZ2_bzDecompressEnd(&a);
				BZ2_bzDecompressEnd(&b);
				if (ca.cur || cb.cur) {
					n_fail++;
					fprintf(stderr, "alloc leak %s\n",
						s->name);
				}
			}
	}
	printf("alloc: %llu failure-injection cases, %llu mismatches\n",
	       (unsigned long long)cases, (unsigned long long)(n_fail - f0));
}

/* n <= 64 bits of p from bit position pos, most significant bit first. */
static uint64_t
read_bits(const uint8_t *p, size_t pos, int n)
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++, pos++)
		v = v << 1 | (uint64_t)(p[pos / 8] >> (7 - pos % 8) & 1);
	return v;
}

/*
 * Bit position of the stored combined CRC in a whole stream, which ends with
 * the 48-bit end-of-stream magic, the 32-bit CRC and zero to seven zero bits
 * of padding.  Exits if the trailer is not found exactly once.
 */
static size_t
combined_crc_bit(const Stream *s)
{
	const size_t nbits = s->comp_len * 8;
	size_t found = SIZE_MAX;
	int matches = 0;
	for (size_t pad = 0; pad < 8; pad++) {
		/* the 32-bit stream header comes first */
		if (nbits < 32 + 80 + pad)
			break;
		const size_t crc = nbits - pad - 32;
		if (read_bits(s->comp, crc - 48, 48) == 0x177245385090ull &&
		    read_bits(s->comp, crc + 32, (int)pad) == 0) {
			found = crc;
			matches++;
		}
	}
	if (matches != 1) {
		fprintf(stderr, "%s: no unique end-of-stream trailer\n",
			s->name);
		exit(2);
	}
	return found;
}

/*
 * The first of the fields a decompression call may change in which x and y
 * differ, or NULL; the two values go to *xv and *yv.  next_in is compared as
 * an offset from in, and next_out as an offset from x_out and y_out.
 */
static const char *
stream_difference(const bz_stream *x, const uint8_t *x_out, const bz_stream *y,
		  const uint8_t *y_out, const uint8_t *in, long long *xv,
		  long long *yv)
{
	/*
	 * long long: the totals are 64-bit, and long is 32-bit on Windows and
	 * on 32-bit targets.
	 */
	const long long field[6][2] = {
		{(long long)((const uint8_t *)x->next_in - in),
		 (long long)((const uint8_t *)y->next_in - in)},
		{(long long)x->avail_in, (long long)y->avail_in},
		{(long long)((const uint8_t *)x->next_out - x_out),
		 (long long)((const uint8_t *)y->next_out - y_out)},
		{(long long)x->avail_out, (long long)y->avail_out},
		{(long long)((uint64_t)x->total_in_hi32 << 32 |
			     x->total_in_lo32),
		 (long long)((uint64_t)y->total_in_hi32 << 32 |
			     y->total_in_lo32)},
		{(long long)((uint64_t)x->total_out_hi32 << 32 |
			     x->total_out_lo32),
		 (long long)((uint64_t)y->total_out_hi32 << 32 |
			     y->total_out_lo32)},
	};
	static const char *const name[6] = {"next_in",	"avail_in",
					    "next_out", "avail_out",
					    "total_in", "total_out"};
	for (int f = 0; f < 6; f++)
		if (field[f][0] != field[f][1]) {
			*xv = field[f][0];
			*yv = field[f][1];
			return name[f];
		}
	return NULL;
}

/* Counts and reports one api-phase failure (printf-style message). */
static void
api_fail(const char *name, int variant, int call, const char *fmt, ...)
{
	n_fail++;
	if (n_fail > 30)
		return;
	va_list ap;
	fprintf(stderr, "api MISMATCH %s variant %d call %d: ", name, variant,
		call);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * Checks that a call left every field of z as it was in z0 and wrote none of
 * the OUT_WINDOW bytes at out, which held pattern.  Reports and returns 1 if
 * it did not.
 */
static int
api_changed(const char *name, int variant, int call, const char *who,
	    const bz_stream *z0, const bz_stream *z, const uint8_t *out,
	    const uint8_t *in, const uint8_t *pattern)
{
	long long v0, v;
	const char *d = stream_difference(z0, out, z, out, in, &v0, &v);
	if (d != NULL) {
		api_fail(name, variant, call, "%s changed %s from %lld to %lld",
			 who, d, v0, v);
		return 1;
	}
	const size_t k = first_difference(pattern, out, 0, OUT_WINDOW);
	if (k < OUT_WINDOW) {
		api_fail(name, variant, call, "%s wrote output byte %zu", who,
			 k);
		return 1;
	}
	return 0;
}

typedef struct {
	uint64_t checks;    /* calls checked */
	uint64_t ended;	    /* sequences whose first return is BZ_STREAM_END */
	uint64_t crc;	    /* ... a combined-CRC mismatch */
	uint64_t errors;    /* ... any other error */
	uint64_t compared;  /* later calls made to both libraries */
	uint64_t spec_only; /* later calls made to bz2-ng alone */
} ApiCounts;

/*
 * Decodes in (s->comp_len bytes, a variant of stream s) with one call to each
 * library, compares the first return, then calls again five times, each time
 * offering the whole input and output afresh so that a decoder that went on
 * could consume and write.
 *
 * After BZ_STREAM_END, and after the combined-CRC mismatch that a flip of a
 * stored combined-CRC bit (crc_flipped) produces, libbz2 is idle: it returns
 * BZ_SEQUENCE_ERROR before touching anything (reference/bzlib.c:818), so both
 * libraries are called and must do that.  After any other error libbz2 would
 * resume its state machine, which the manual does not allow and which reaches
 * undefined behaviour inside libbz2 itself, so bz2-ng is called alone and held
 * to docs/spec.md section 2.5: the same error again, every stream field as the
 * caller set it, nothing written.
 */
static void
api_after_first_return(const Stream *s, const uint8_t *in, int variant,
		       int small, int crc_flipped, ApiCounts *counts)
{
	const size_t out_cap = s->raw_len * 2 + 4096;
	ensure_out(out_cap);
	bz_stream a, b;
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	ref_BZ2_bzDecompressInit(&a, 0, small);
	BZ2_bzDecompressInit(&b, 0, small);
	int first = BZ_OK;
	for (int call = 0; call < 6; call++) {
		a.next_in = b.next_in = (char *)in;
		a.avail_in = b.avail_in = (unsigned)s->comp_len;
		a.next_out = (char *)out_a;
		b.next_out = (char *)out_b;
		a.avail_out = b.avail_out = (unsigned)out_cap;
		counts->checks++;
		if (call == 0) {
			const int ra = ref_BZ2_bzDecompress(&a);
			const int rb = BZ2_bzDecompress(&b);
			long long va, vb;
			const char *d = stream_difference(&a, out_a, &b, out_b,
							  in, &va, &vb);
			if (ra != rb) {
				api_fail(s->name, variant, call,
					 "first return ref %d ng %d", ra, rb);
				break;
			}
			if (d != NULL) {
				api_fail(s->name, variant, call,
					 "%s ref %lld ng %lld", d, va, vb);
				break;
			}
			/* only then is libbz2 known to be idle */
			if (crc_flipped && ra != BZ_DATA_ERROR) {
				api_fail(s->name, variant, call,
					 "return %d after a combined-CRC flip",
					 ra);
				break;
			}
			if (ra == BZ_OK)
				break;
			first = ra;
			if (ra == BZ_STREAM_END)
				counts->ended++;
			else if (crc_flipped)
				counts->crc++;
			else
				counts->errors++;
			continue;
		}
		const int idle = first == BZ_STREAM_END || crc_flipped;
		const int expected = idle ? BZ_SEQUENCE_ERROR : first;
		const bz_stream a0 = a, b0 = b;
		uint8_t pattern[OUT_WINDOW];
		fill_pattern(out_a, pattern, OUT_WINDOW,
			     (uint64_t)variant << 8 | (uint64_t)call);
		memcpy(out_b, pattern, OUT_WINDOW);
		if (idle) {
			counts->compared++;
			const int ra = ref_BZ2_bzDecompress(&a);
			if (ra != expected) {
				api_fail(s->name, variant, call,
					 "libbz2 returned %d, expected %d", ra,
					 expected);
				break;
			}
			if (api_changed(s->name, variant, call, "libbz2", &a0,
					&a, out_a, in, pattern))
				break;
		} else {
			counts->spec_only++;
		}
		const int rb = BZ2_bzDecompress(&b);
		if (rb != expected) {
			api_fail(s->name, variant, call,
				 "bz2-ng returned %d, expected %d", rb,
				 expected);
			break;
		}
		if (api_changed(s->name, variant, call, "bz2-ng", &b0, &b,
				out_b, in, pattern))
			break;
	}
	ref_BZ2_bzDecompressEnd(&a);
	BZ2_bzDecompressEnd(&b);
}

/*
 * Parameter and sequence checks, and the calls after a first return other
 * than BZ_OK (api_after_first_return).
 */
static void
phase_api(void)
{
	uint64_t f0 = n_fail, cases = 0;
	rs = seed;
#define API_CHECK(expr_ref, expr_ng, what)                                    \
	do {                                                                  \
		const int x_ = (expr_ref), y_ = (expr_ng);                    \
		cases++;                                                      \
		if (x_ != y_) {                                               \
			n_fail++;                                             \
			fprintf(stderr, "api MISMATCH %s: %d/%d\n", what, x_, \
				y_);                                          \
		}                                                             \
	} while (0)
	bz_stream a, b;
	API_CHECK(ref_BZ2_bzDecompressInit(NULL, 0, 0),
		  BZ2_bzDecompressInit(NULL, 0, 0), "init NULL");
	for (int small = -1; small <= 2; small++)
		for (int verb = -1; verb <= 5; verb++) {
			memset(&a, 0, sizeof a);
			memset(&b, 0, sizeof b);
			const int ra =
				ref_BZ2_bzDecompressInit(&a, verb, small);
			const int rb = BZ2_bzDecompressInit(&b, verb, small);
			API_CHECK(ra, rb, "init small/verbosity");
			if (ra == BZ_OK)
				ref_BZ2_bzDecompressEnd(&a);
			if (rb == BZ_OK)
				BZ2_bzDecompressEnd(&b);
		}
	API_CHECK(ref_BZ2_bzDecompress(NULL), BZ2_bzDecompress(NULL),
		  "decompress NULL");
	API_CHECK(ref_BZ2_bzDecompressEnd(NULL), BZ2_bzDecompressEnd(NULL),
		  "end NULL");
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	API_CHECK(ref_BZ2_bzDecompress(&a), BZ2_bzDecompress(&b),
		  "decompress before init");
	API_CHECK(ref_BZ2_bzDecompressEnd(&a), BZ2_bzDecompressEnd(&b),
		  "end before init");
	/* a moved bz_stream (state->strm != strm) */
	ref_BZ2_bzDecompressInit(&a, 0, 0);
	BZ2_bzDecompressInit(&b, 0, 0);
	bz_stream a2 = a, b2 = b;
	API_CHECK(ref_BZ2_bzDecompress(&a2), BZ2_bzDecompress(&b2),
		  "decompress moved stream");
	API_CHECK(ref_BZ2_bzDecompressEnd(&a2), BZ2_bzDecompressEnd(&b2),
		  "end moved stream");
	ref_BZ2_bzDecompressEnd(&a);
	BZ2_bzDecompressEnd(&b);
	API_CHECK(ref_BZ2_bzDecompressEnd(&a), BZ2_bzDecompressEnd(&b),
		  "end twice");
	/*
	 * After BZ_STREAM_END, and after errors: each stream as it is (variant
	 * 0), with one random bit flipped (1-39), and with one bit of the
	 * stored combined CRC flipped (40).
	 */
	ApiCounts counts = {0, 0, 0, 0, 0, 0};
	for (size_t i = 0; i < n_streams; i++) {
		const Stream *s = &streams[i];
		if (s->comp_len > 100000)
			continue;
		const size_t crc_bit = combined_crc_bit(s);
		uint8_t *buf = malloc(s->comp_len);
		for (int k = 0; k <= 40; k++) {
			memcpy(buf, s->comp, s->comp_len);
			/* bit position in the stream, most significant first */
			size_t flip = SIZE_MAX;
			if (k == 40) {
				flip = crc_bit + i % 32;
			} else if (k > 0) {
				const size_t byte = rnd() % s->comp_len;
				flip = byte * 8 + 7 - rnd() % 8;
			}
			if (flip != SIZE_MAX)
				buf[flip / 8] ^= (uint8_t)(0x80u >> (flip % 8));
			const int crc_flipped =
				flip >= crc_bit && flip < crc_bit + 32;
			const int small = k == 40 ? (int)(i & 1) : k & 1;
			api_after_first_return(s, buf, k, small, crc_flipped,
					       &counts);
		}
		free(buf);
	}
	printf("api: %llu checks, %llu mismatches; sequences ending in "
	       "BZ_STREAM_END %llu, in a combined-CRC mismatch %llu, in "
	       "another "
	       "error %llu; later calls compared with libbz2 (idle) %llu, "
	       "checked against the specification (bz2-ng alone) %llu\n",
	       (unsigned long long)(cases + counts.checks),
	       (unsigned long long)(n_fail - f0),
	       (unsigned long long)counts.ended, (unsigned long long)counts.crc,
	       (unsigned long long)counts.errors,
	       (unsigned long long)counts.compared,
	       (unsigned long long)counts.spec_only);
}

int
main(int argc, char **argv)
{
	unsigned long n_random = 1500;
	size_t chunk = 0;
	int file_level = 9;
	int opt;
	require_requested_tier();
	while ((opt = getopt(argc, argv, "qs:n:c:l:")) != -1) {
		switch (opt) {
		case 'q':
			quiet = 1;
			break;
		case 'c':
			chunk = (size_t)strtoull(optarg, NULL, 0);
			break;
		case 'l':
			file_level = atoi(optarg);
			break;
		case 's':
			seed = strtoull(optarg, NULL, 0) | 1;
			break;
		case 'n':
			n_random = strtoul(optarg, NULL, 0);
			break;
		default:
			fprintf(stderr,
				"usage: dec_difftest [-q] [-s seed] [-n "
				"mutations] [-c chunk] [-l level] "
				"phase... [-- file ...]\n");
			return 2;
		}
	}
	int do_valid = 0, do_mutate = 0, do_read = 0, do_alloc = 0, do_api = 0,
	    do_crafted = 0, do_slack = 0;
	int i = optind;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (!strcmp(argv[i], "valid"))
			do_valid = 1;
		else if (!strcmp(argv[i], "mutate"))
			do_mutate = 1;
		else if (!strcmp(argv[i], "read"))
			do_read = 1;
		else if (!strcmp(argv[i], "alloc"))
			do_alloc = 1;
		else if (!strcmp(argv[i], "api"))
			do_api = 1;
		else if (!strcmp(argv[i], "crafted"))
			do_crafted = 1;
		else if (!strcmp(argv[i], "slack"))
			do_slack = 1;
		else if (!strcmp(argv[i], "all"))
			do_valid = do_mutate = do_read = do_alloc = do_api =
				do_crafted = do_slack = 1;
		else {
			fprintf(stderr, "unknown phase %s\n", argv[i]);
			return 2;
		}
	}
	rs = seed;
	gen_inputs();
	for (; i < argc; i++)
		load_file(argv[i], chunk, file_level);
	/*
	 * Unbuffered, so that every line is out before anything after it can
	 * crash (Microsoft's C runtime has no line buffering: _IOLBF is
	 * _IOFBF there).
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("tier %s: %zu streams (generated + files)\n", bzng_tier_name(),
	       n_streams);
	if (do_valid)
		phase_valid();
	if (do_read)
		phase_read();
	if (do_alloc)
		phase_alloc();
	if (do_api)
		phase_api();
	if (do_mutate)
		phase_mutate(n_random);
	if (do_crafted)
		phase_crafted();
	if (do_slack)
		phase_slack();
	printf("TOTAL: %llu runs, %llu calls, %llu mismatches\n",
	       (unsigned long long)n_runs, (unsigned long long)n_calls,
	       (unsigned long long)n_fail);
	return n_fail ? 1 : 0;
}
