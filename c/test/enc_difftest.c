/*
 * Differential tester for compression: stock libbzip2 1.0.8 (test/reference,
 * every symbol renamed ref_*) against the bz2-ng compression API, driven with
 * identical call sequences. Runs on whichever kernel tier the library
 * selects (BZ2NG_TIER forces one).
 *
 * At every BZ2_bzCompress call it compares the return code, how far next_in
 * and next_out moved, avail_in, avail_out and the 64-bit totals. At the end
 * of each stream it compares the bytes (after patching libbz2's origPtr into
 * exactly periodic blocks, where several values are valid and libbz2's choice
 * depends on which of its sorters ran) and decodes the bz2-ng stream with
 * stock libbz2.
 *
 * usage: enc_difftest [-s seed] [-r rounds] [-v] suite [files...]
 * suites: api      parameter, sequence and allocator cases
 * edge     generated inputs x split points, schedules and levels
 * random   random inputs x random call sequences
 * capacity inputs at and around the block capacity
 * wrappers BZ2_bzBuffToBuffCompress and BZ2_bzWrite* on edge inputs
 * corpus   the files given (whole file, cut into streams of -c bytes)
 * Prints one summary line per suite: SUITE name cases=.. calls=.. ...
 */
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <errno.h>
#  include <fcntl.h>
#  include <io.h>
#  include <process.h>
#  include <sys/stat.h>
#endif

#include "bzlib.h"
#include "compat.h"
#include "kern.h"
#include "reference_api.h"
#include "test_tier.h"

/* ---- counters and failure reporting
 * ------------------------------------------ */

static struct {
	long cases, calls, streams, identical, patched, periodic_blocks,
		decoded, failures;
	long seq_errors, param_errors;
	unsigned long long bytes_in;
	long total_carries; /* streams whose total_in or total_out crossed 2^32
			     */
} stats;
static int verbose;
static const char *current_case = "";

static void
fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL [%s]: ", current_case);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	stats.failures++;
	if (stats.failures > 20) {
		fprintf(stderr, "too many failures\n");
		exit(1);
	}
}

/* ---- RNG (splitmix64) -------------------------------------------------------
 */

static uint64_t rng_state = 1;
static uint64_t
rnd(void)
{
	uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}
static size_t
rnd_below(size_t n)
{
	return n ? (size_t)(rnd() % n) : 0;
}

/* ---- the two implementations --------------------------------------------- */

typedef struct {
	int(BZ_API(*init))(bz_stream *, int, int, int);
	int(BZ_API(*compress))(bz_stream *, int);
	int(BZ_API(*end))(bz_stream *);
} api;

static const api REF = {ref_BZ2_bzCompressInit, ref_BZ2_bzCompress,
			ref_BZ2_bzCompressEnd};
static const api NG = {BZ2_bzCompressInit, BZ2_bzCompress, BZ2_bzCompressEnd};

/* One side of a differential run: a stream and its growing output. */
typedef struct {
	bz_stream s;
	uint8_t *out;
	size_t out_cap;
} side;

/*
 * Bytes past the space offered to a call that get the call's pattern before
 * it and are checked after it.
 */
#define OUT_WINDOW 256

/*
 * The byte the tester places at offset i of both output buffers before a
 * library may write there, in the pattern numbered salt: both sides start
 * identical, so any byte one library changes shows up when they are
 * compared.  Every call gets a pattern of its own, so that neither library
 * can rely on what an earlier call left in the buffer.
 */
static uint8_t
pattern_byte(size_t i, uint64_t salt)
{
	return (uint8_t)(((uint64_t)i * 0x9E3779B97F4A7C15ull +
			  salt * 0xD1B54A32D192ED03ull) >>
			 56);
}

/*
 * Grows sd's output buffer to at least need bytes, filling the new part with
 * pattern 0.
 */
static void
side_reserve(side *sd, size_t need)
{
	if (sd->out_cap >= need)
		return;
	size_t cap = sd->out_cap ? sd->out_cap : 4096;
	while (cap < need)
		cap *= 2;
	sd->out = realloc(sd->out, cap);
	if (sd->out == NULL) {
		fprintf(stderr, "out of memory (%zu-byte output buffer)\n",
			cap);
		exit(2);
	}
	for (size_t i = sd->out_cap; i < cap; i++)
		sd->out[i] = pattern_byte(i, 0);
	sd->out_cap = cap;
}

/* Puts pattern salt into both buffers' bytes [from, to). */
static void
prefill(side *a, side *b, size_t from, size_t to, uint64_t salt)
{
	for (size_t i = from; i < to; i++)
		a->out[i] = b->out[i] = pattern_byte(i, salt);
}

/*
 * Checks that neither library changed a byte of [from, to), which held
 * pattern salt before the call (bytes it was offered but did not deliver, and
 * bytes past the offered space).
 */
static int
check_untouched(const char *what, const side *a, const side *b, size_t from,
		size_t to, uint64_t salt)
{
	for (size_t i = from; i < to; i++) {
		const uint8_t want = pattern_byte(i, salt);
		if (a->out[i] != want || b->out[i] != want) {
			fail("%s: buffer byte %zu past next_out changed "
			     "(libbz2 %s, bz2-ng %s)",
			     what, i, a->out[i] != want ? "yes" : "no",
			     b->out[i] != want ? "yes" : "no");
			return 1;
		}
	}
	return 0;
}

/*
 * Checks that the two buffers are equal in [from, to) (everything past the
 * stream, at its end).
 */
static void
check_same_tail(const side *a, const side *b, size_t from, size_t to)
{
	for (size_t i = from; i < to; i++) {
		if (a->out[i] != b->out[i]) {
			fail("buffer byte %zu past the stream differs "
			     "(libbz2 %u, bz2-ng %u)",
			     i, a->out[i], b->out[i]);
			return;
		}
	}
}

static uint64_t
total_in(const bz_stream *s)
{
	return (uint64_t)s->total_in_hi32 << 32 | s->total_in_lo32;
}
static uint64_t
total_out(const bz_stream *s)
{
	return (uint64_t)s->total_out_hi32 << 32 | s->total_out_lo32;
}

/* Compares everything observable after one call. */
static int
compare_call(const char *what, int rc_a, int rc_b, const side *a, const side *b,
	     const uint8_t *in)
{
	stats.calls++;
	const bz_stream *x = &a->s, *y = &b->s;
	if (rc_a != rc_b || x->next_in != y->next_in ||
	    x->avail_in != y->avail_in ||
	    (const uint8_t *)x->next_out - a->out !=
		    (const uint8_t *)y->next_out - b->out ||
	    x->avail_out != y->avail_out ||
	    x->total_in_lo32 != y->total_in_lo32 ||
	    x->total_in_hi32 != y->total_in_hi32 ||
	    x->total_out_lo32 != y->total_out_lo32 ||
	    x->total_out_hi32 != y->total_out_hi32) {
		fail("%s: stock rc=%d in=%td/%u out=%td/%u tot=%llu/%llu | bzn "
		     "rc=%d in=%td/%u out=%td/%u tot=%llu/%llu",
		     what, rc_a, (const uint8_t *)x->next_in - in, x->avail_in,
		     (const uint8_t *)x->next_out - a->out, x->avail_out,
		     (unsigned long long)total_in(x),
		     (unsigned long long)total_out(x), rc_b,
		     (const uint8_t *)y->next_in - in, y->avail_in,
		     (const uint8_t *)y->next_out - b->out, y->avail_out,
		     (unsigned long long)total_in(y),
		     (unsigned long long)total_out(y));
		return 1;
	}
	if (rc_a == BZ_SEQUENCE_ERROR)
		stats.seq_errors++;
	if (rc_a == BZ_PARAM_ERROR)
		stats.param_errors++;
	return 0;
}

static uint32_t
get_bits(const uint8_t *p, uint64_t bit, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++, bit++)
		v = (v << 1) | ((p[bit >> 3] >> (7 - (bit & 7))) & 1);
	return v;
}

static void
set_bits(uint8_t *p, uint64_t bit, int n, uint32_t v)
{
	for (int i = n - 1; i >= 0; i--, bit++) {
		uint8_t m = (uint8_t)(0x80 >> (bit & 7));
		if ((v >> i) & 1)
			p[bit >> 3] |= m;
		else
			p[bit >> 3] &= (uint8_t)~m;
	}
}

/*
 * Locates the 24-bit origPtr field that contains bit `bit` of both streams:
 * a field preceded, in both, by the 48-bit block magic, the 32-bit block CRC
 * and the randomised bit. Returns the field's first bit, or UINT64_MAX.
 */
static uint64_t
origptr_field(const uint8_t *sa, const uint8_t *sb, uint64_t nbits,
	      uint64_t bit)
{
	for (uint64_t f = bit >= 23 ? bit - 23 : 0; f <= bit; f++) {
		if (f < 81 || f + 24 > nbits)
			continue;
		if (get_bits(sa, f - 81, 24) == 0x314159 &&
		    get_bits(sa, f - 57, 24) == 0x265359 &&
		    get_bits(sb, f - 81, 24) == 0x314159 &&
		    get_bits(sb, f - 57, 24) == 0x265359)
			return f;
	}
	return UINT64_MAX;
}

/*
 * Final checks on two complete streams: identity, and decoding of bz2-ng's
 * stream by libbz2 to in[0..n). The one allowed difference is the origPtr of
 * exactly periodic blocks (several rows are valid there, and libbz2's choice
 * depends on which of its sorters ran): every differing bit must lie in an
 * origPtr field, and with libbz2's values patched in the streams must be
 * identical; the decode check then shows bz2-ng's value is a valid one.
 */
static void
check_streams(const uint8_t *sa, const uint8_t *sb, size_t len_a, size_t len_b,
	      const uint8_t *in, size_t n)
{
	stats.streams++;
	if (len_a != len_b) {
		fail("stream lengths differ: libbz2 %zu bz2-ng %zu", len_a,
		     len_b);
		return;
	}
	if (memcmp(sa, sb, len_a) == 0) {
		stats.identical++;
	} else {
		uint8_t *pb = malloc(len_b ? len_b : 1);
		memcpy(pb, sb, len_b);
		const uint64_t nbits = (uint64_t)len_a * 8;
		int unexplained = 0;
		for (size_t byte = 0; byte < len_a && !unexplained; byte++) {
			if (sa[byte] == pb[byte])
				continue;
			for (uint64_t bit = (uint64_t)byte * 8;
			     bit < (uint64_t)byte * 8 + 8; bit++) {
				if (get_bits(sa, bit, 1) ==
				    get_bits(pb, bit, 1))
					continue;
				uint64_t f = origptr_field(sa, pb, nbits, bit);
				if (f == UINT64_MAX) {
					unexplained = 1;
					break;
				}
				set_bits(pb, f, 24, get_bits(sa, f, 24));
				stats.periodic_blocks++;
			}
		}
		if (unexplained || memcmp(sa, pb, len_a) != 0) {
			size_t d = 0;
			while (d < len_a && sa[d] == sb[d])
				d++;
			fail("streams differ at byte %zu of %zu (not explained "
			     "by periodic origPtr)",
			     d, len_a);
		} else {
			stats.patched++;
		}
		free(pb);
	}
	unsigned int dlen = (unsigned int)n + 1;
	uint8_t *dec = malloc(n + 1);
	int rc = ref_BZ2_bzBuffToBuffDecompress((char *)dec, &dlen, (char *)sb,
						(unsigned int)len_b, 0, 0);
	if (rc != BZ_OK || dlen != n || memcmp(dec, in, n) != 0)
		fail("libbz2 does not decode the stream (rc %d)", rc);
	else
		stats.decoded++;
	free(dec);
}

/* ---- call schedules
 * ------------------------------------------------------------ */

/* How a schedule picks the next call. */
typedef struct {
	int kind;	  /* SCHED_* */
	size_t in_chunk;  /* fixed: RUN chunk size (0 = everything in the FINISH
			     call) */
	size_t out_chunk; /* fixed: avail_out per call (0 = unlimited) */
	size_t split;	  /* split: first RUN call takes in[0..split) */
	int split_action; /* split: action for the first part (BZ_RUN or
			     BZ_FLUSH) */
	/* random: maxima for chunk sizes and probabilities in 1/1000 */
	size_t in_max, out_max;
	int p_flush, p_finish_partial, p_err, p_zero_out;
	/*
	 * Start the 64-bit totals just below 2^32 (callers may write them;
	 * libbz2 only increments them), to test the carry into the high word.
	 */
	int preset_totals;
} schedule;

enum {
	SCHED_FIXED,
	SCHED_SPLIT,
	SCHED_RANDOM
};

static size_t
pick_size(size_t max)
{
	switch (rnd_below(8)) {
	case 0:
		return 0;
	case 1:
		return 1;
	case 2:
		return 1 + rnd_below(8);
	case 3:
		return 1 + rnd_below(64);
	case 4:
		return 1 + rnd_below(5000);
	default:
		return 1 + rnd_below(max ? max : 1);
	}
}

/* Runs one stream through both libraries with the given schedule. */
static void
run_stream(const uint8_t *in, size_t n, int level, int work_factor,
	   const schedule *sc)
{
	side a = {0}, b = {0};
	stats.cases++;
	int ra = REF.init(&a.s, level, 0, work_factor);
	int rb = NG.init(&b.s, level, 0, work_factor);
	if (ra != rb || ra != BZ_OK) {
		fail("init rc stock %d bzn %d", ra, rb);
		return;
	}
	if (sc->preset_totals) {
		a.s.total_in_lo32 = b.s.total_in_lo32 =
			0xffffffffu - (unsigned)rnd_below(n + 1);
		a.s.total_out_lo32 = b.s.total_out_lo32 =
			0xffffffffu - (unsigned)rnd_below(n / 8 + 64);
	}

	size_t pos = 0, out_pos = 0, cur_avail = 0, max_out_seen = 0;
	int phase = BZ_RUN, done = 0, no_progress = 0, step = 0;
	const char *what = "";
	while (!done) {
		if (++step > 50000000) {
			fail("schedule did not terminate");
			break;
		}
		int action;
		size_t avail, avail_out;
		int inject = 0;
		if (sc->kind == SCHED_FIXED) {
			if (phase == BZ_RUN && sc->in_chunk && pos < n) {
				action = BZ_RUN;
				avail = n - pos < sc->in_chunk ? n - pos :
								 sc->in_chunk;
			} else {
				action = BZ_FINISH;
				avail = phase == BZ_FINISH ? cur_avail :
							     n - pos;
			}
			avail_out = sc->out_chunk ? sc->out_chunk :
						    (n + n / 50 + 4096);
		} else if (sc->kind == SCHED_SPLIT) {
			if (phase == BZ_RUN && step == 1) {
				action = sc->split_action;
				avail = sc->split;
			} else if (phase == BZ_FLUSH) {
				action = BZ_FLUSH;
				avail = cur_avail;
			} else {
				action = BZ_FINISH;
				avail = phase == BZ_FINISH ? cur_avail :
							     n - pos;
			}
			avail_out = sc->out_chunk ? sc->out_chunk :
						    (n + n / 50 + 4096);
		} else {
			size_t remaining = n - pos;
			if (phase == BZ_RUN) {
				uint64_t r = rnd_below(1000);
				if (remaining == 0 || r < 3) {
					action = BZ_FINISH;
					avail = (int)rnd_below(1000) <
								sc->p_finish_partial ?
							rnd_below(remaining +
								  1) :
							remaining;
				} else if ((int)r < 3 + sc->p_flush) {
					action = BZ_FLUSH;
					avail = pick_size(sc->in_max);
					if (avail > remaining)
						avail = remaining;
				} else {
					action = BZ_RUN;
					avail = pick_size(sc->in_max);
					if (avail > remaining)
						avail = remaining;
					if (rnd_below(1000) < 5)
						action = 7; /* invalid action */
				}
			} else {
				action = phase;
				avail = cur_avail;
				if ((int)rnd_below(1000) < sc->p_err) {
					inject = 1;
					switch (rnd_below(3)) {
					case 0:
						action = phase == BZ_FLUSH ?
								 BZ_FINISH :
								 BZ_FLUSH;
						break;
					case 1:
						action = BZ_RUN;
						break;
					default:
						if (avail > 0 && rnd_below(2))
							avail--;
						else if (pos + avail < n)
							avail++;
						else
							action = BZ_RUN;
					}
				}
			}
			avail_out = (int)rnd_below(1000) < sc->p_zero_out ?
					    0 :
					    pick_size(sc->out_max);
			if (no_progress >= 2)
				avail_out = 1 + rnd_below(sc->out_max ?
								  sc->out_max :
								  1);
		}
		const size_t fill_end = out_pos + avail_out + OUT_WINDOW;
		side_reserve(&a, fill_end);
		side_reserve(&b, fill_end);
		prefill(&a, &b, out_pos, fill_end, (uint64_t)step);
		a.s.next_in = b.s.next_in = (char *)in + pos;
		a.s.avail_in = b.s.avail_in = (unsigned int)avail;
		a.s.next_out = (char *)a.out + out_pos;
		b.s.next_out = (char *)b.out + out_pos;
		a.s.avail_out = b.s.avail_out = (unsigned int)avail_out;
		ra = REF.compress(&a.s, action);
		rb = NG.compress(&b.s, action);
		what = action == BZ_RUN	   ? "RUN" :
		       action == BZ_FLUSH  ? "FLUSH" :
		       action == BZ_FINISH ? "FINISH" :
					     "BAD";
		char desc[160];
		snprintf(desc, sizeof desc,
			 "step %d %s avail_in=%zu avail_out=%zu%s", step, what,
			 avail, avail_out, inject ? " (injected)" : "");
		if (compare_call(desc, ra, rb, &a, &b, in))
			break;
		size_t new_pos = (size_t)((const uint8_t *)a.s.next_in - in);
		size_t new_out = (size_t)((uint8_t *)a.s.next_out - a.out);
		if (check_untouched(desc, &a, &b, new_out, fill_end,
				    (uint64_t)step))
			break;
		no_progress = (new_pos == pos && new_out == out_pos) ?
				      no_progress + 1 :
				      0;
		pos = new_pos;
		out_pos = new_out;
		if (out_pos > max_out_seen)
			max_out_seen = out_pos;
		/*
		 * An injected error leaves the library unchanged; the caller
		 * goes on with the avail_in it had.
		 */
		if (inject)
			continue;
		cur_avail = a.s.avail_in;
		/* FLUSH and FINISH switch the library's mode before anything
		 * can fail. */
		if (phase == BZ_RUN &&
		    (action == BZ_FLUSH || action == BZ_FINISH))
			phase = action;
		if (phase == BZ_FLUSH && ra == BZ_RUN_OK)
			phase = BZ_RUN;
		if (ra == BZ_STREAM_END)
			done = 1;
	}
	if (done) {
		/* Calls after the end are sequence errors in both. */
		int act = (int)rnd_below(3);
		const size_t fill_end = out_pos + 16 + OUT_WINDOW;
		side_reserve(&a, fill_end);
		side_reserve(&b, fill_end);
		prefill(&a, &b, out_pos, fill_end, (uint64_t)step + 1);
		a.s.avail_out = b.s.avail_out = 16;
		ra = REF.compress(&a.s, act);
		rb = NG.compress(&b.s, act);
		compare_call("after STREAM_END", ra, rb, &a, &b, in);
		check_untouched("after STREAM_END", &a, &b, out_pos, fill_end,
				(uint64_t)step + 1);
		check_same_tail(&a, &b, out_pos,
				a.out_cap < b.out_cap ? a.out_cap : b.out_cap);
		stats.bytes_in += pos;
		if (b.s.total_in_hi32 || b.s.total_out_hi32)
			stats.total_carries++;
		check_streams(a.out, b.out, out_pos, out_pos, in, pos);
	}
	ra = REF.end(&a.s);
	rb = NG.end(&b.s);
	if (ra != rb || a.s.state != NULL || b.s.state != NULL)
		fail("end rc stock %d bzn %d", ra, rb);
	free(a.out);
	free(b.out);
	(void)what;
}

/* ---- inputs
 * ------------------------------------------------------------------ */

typedef struct {
	char name[64];
	uint8_t *data;
	size_t n;
} input;

typedef struct {
	input *v;
	size_t n, cap;
} input_list;

static uint8_t *
add_input(input_list *l, size_t n, const char *fmt, ...)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? 2 * l->cap : 64;
		l->v = realloc(l->v, l->cap * sizeof *l->v);
	}
	input *x = &l->v[l->n++];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(x->name, sizeof x->name, fmt, ap);
	va_end(ap);
	x->data = malloc(n ? n : 1);
	x->n = n;
	return x->data;
}

/* Small inputs covering the RLE1 unit boundaries. */
static void
edge_inputs(input_list *l)
{
	add_input(l, 0, "empty");
	for (int c = 0; c < 256; c += 51)
		*add_input(l, 1, "byte_%d", c) = (uint8_t)c;
	uint8_t *p = add_input(l, 256, "all_bytes");
	for (int i = 0; i < 256; i++)
		p[i] = (uint8_t)i;
	p = add_input(l, 512, "all_bytes_desc_x2");
	for (int i = 0; i < 512; i++)
		p[i] = (uint8_t)(255 - (i & 255));
	static const int run_lens[] = {1,   2,	 3,   4,   5,	6,   254,
				       255, 256, 257, 258, 259, 260, 509,
				       510, 511, 512, 765, 1000};
	for (size_t k = 0; k < sizeof run_lens / sizeof run_lens[0]; k++) {
		int L = run_lens[k];
		memset(add_input(l, (size_t)L, "run_%d", L), 'a', (size_t)L);
		p = add_input(l, (size_t)L + 2, "x_run_%d_y", L);
		p[0] = 'x';
		memset(p + 1, 'a', (size_t)L);
		p[L + 1] = 'y';
	}
	/* Alternating runs of lengths 1..8 and 250..260. */
	p = add_input(l, 2000, "mixed_short_runs");
	for (size_t i = 0, r = 0; i < 2000; r++) {
		size_t L = 1 + r % 8;
		for (size_t j = 0; j < L && i < 2000; j++)
			p[i++] = (uint8_t)('a' + r % 3);
	}
	p = add_input(l, 6000, "mixed_long_runs");
	for (size_t i = 0, r = 0; i < 6000; r++) {
		size_t L = 250 + r % 11;
		for (size_t j = 0; j < L && i < 6000; j++)
			p[i++] = (uint8_t)(r & 1 ? 'b' : 'c');
	}
	p = add_input(l, 3000, "random_3000");
	for (size_t i = 0; i < 3000; i++)
		p[i] = (uint8_t)rnd();
	p = add_input(l, 3000, "random_ab_3000");
	for (size_t i = 0; i < 3000; i++)
		p[i] = (uint8_t)('a' + (rnd() & 1));
	p = add_input(l, 4096, "text");
	static const char *words[] = {"the ",	"quick ", "brown ", "fox ",
				      "jumps ", "over ",  "lazy ",  "dog\n"};
	for (size_t i = 0; i < 4096;) {
		const char *w = words[rnd_below(8)];
		for (size_t j = 0; w[j] && i < 4096; j++)
			p[i++] = (uint8_t)w[j];
	}
	memset(add_input(l, 3000, "zeros_3000"), 0, 3000);
	p = add_input(l, 3000, "period_7");
	for (size_t i = 0; i < 3000; i++)
		p[i] = (uint8_t)("abcabdx"[i % 7]);
}

/* ---- suites
 * ------------------------------------------------------------------ */

/* Clears the counters for a suite. */
static void
suite_header(void)
{
	memset(&stats, 0, sizeof stats);
}

/* Prints the suite's counters (and the kernel tier that ran) on one line. */
static void
suite_report(const char *name)
{
	printf("SUITE %s tier=%s cases=%ld calls=%ld streams=%ld bytes_in=%llu "
	       "identical=%ld identical_after_origptr_patch=%ld "
	       "periodic_blocks=%ld decoded=%ld seq_errors=%ld "
	       "param_errors=%ld total_carries=%ld failures=%ld\n",
	       name, bzng_tier_name(), stats.cases, stats.calls, stats.streams,
	       stats.bytes_in, stats.identical, stats.patched,
	       stats.periodic_blocks, stats.decoded, stats.seq_errors,
	       stats.param_errors, stats.total_carries, stats.failures);
	fflush(stdout);
}

/*
 * Every split point of every edge input, as RUN|FINISH and FLUSH|FINISH,
 * with unlimited and one-byte output, at three levels; plus fixed chunkings.
 */
static void
suite_edge(void)
{
	input_list l = {0};
	edge_inputs(&l);
	static const int levels[] = {1, 5, 9};
	for (size_t i = 0; i < l.n; i++) {
		input *x = &l.v[i];
		for (size_t li = 0; li < 3; li++) {
			int level = levels[li];
			current_case = x->name;
			size_t step = x->n > 600 ? x->n / 97 + 1 : 1;
			for (size_t k = 0; k <= x->n; k += step)
				for (int act = 0; act < 2; act++)
					for (int o = 0; o < 2; o++) {
						schedule sc = {
							.kind = SCHED_SPLIT,
							.split = k,
							.split_action =
								act ? BZ_FLUSH :
								      BZ_RUN,
							.out_chunk = o ? 1 : 0};
						run_stream(x->data, x->n, level,
							   0, &sc);
					}
			static const size_t chunks[] = {1, 2,  3,   4,	 5,
							7, 64, 255, 256, 5000};
			for (size_t c = 0; c < sizeof chunks / sizeof chunks[0];
			     c++) {
				schedule sc = {.kind = SCHED_FIXED,
					       .in_chunk = chunks[c],
					       .out_chunk = chunks[c] % 7 + 1};
				run_stream(x->data, x->n, level, 0, &sc);
			}
		}
	}
	suite_report("edge");
}

/*
 * Random inputs: a mixture of literals and runs, literal-heavy, mixed or
 * run-heavy, over an alphabet of 1 to 256 bytes.
 */
static void
fill_random_input(uint8_t *p, size_t n)
{
	int alpha = 1 + (int)rnd_below(rnd_below(2) ? 4 : 256);
	int mode = (int)rnd_below(3);
	for (size_t i = 0; i < n;) {
		uint64_t r = rnd_below(100);
		size_t L;
		if (mode == 0)
			L = r < 95 ? 1 : 1 + rnd_below(6);
		else if (mode == 1)
			L = r < 70 ? 1 :
			    r < 95 ? 1 + rnd_below(6) :
				     250 + rnd_below(12);
		else if (r < 60)
			L = 1;
		else if (r < 80)
			L = 1 + rnd_below(6);
		else if (r < 95)
			L = 250 + rnd_below(12);
		else
			L = 1 + rnd_below(2000);
		uint8_t c = (uint8_t)rnd_below((size_t)alpha);
		for (size_t j = 0; j < L && i < n; j++)
			p[i++] = c;
	}
}

static void
suite_random(long rounds)
{
	for (long r = 0; r < rounds; r++) {
		size_t n = rnd_below(4) == 0 ? rnd_below(300000) :
					       rnd_below(20000);
		uint8_t *p = malloc(n ? n : 1);
		fill_random_input(p, n);
		int level = 1 + (int)rnd_below(9);
		if (n > 100000 && level > 3)
			level = 1 + (int)rnd_below(3);
		schedule sc = {
			.kind = SCHED_RANDOM,
			.in_max = 1 + rnd_below(rnd_below(2) ? 100 : 70000),
			.out_max = 1 + rnd_below(rnd_below(2) ? 50 : 70000),
			.p_flush = (int)rnd_below(200),
			.p_finish_partial = 100,
			.p_err = 30,
			.p_zero_out = 50};
		char name[96];
		snprintf(name, sizeof name, "random r=%ld n=%zu level=%d", r, n,
			 level);
		current_case = name;
		sc.preset_totals = rnd_below(10) == 0;
		run_stream(p, n, level, (int)rnd_below(251), &sc);
		free(p);
	}
	suite_report("random");
}

/*
 * Inputs whose RLE1 length lands at and just around a block's capacity, with
 * the crossing unit at the end of the input, so the single-FINISH and the
 * RUN+FINISH block boundaries differ.
 */
static void
suite_capacity(void)
{
	static const int levels[] = {1, 2};
	for (size_t li = 0; li < 2; li++) {
		int level = levels[li];
		size_t nmax = 100000 * (size_t)level - 19;
		for (int tail_kind = 0; tail_kind < 4; tail_kind++)
			for (long d = -6; d <= 6; d++) {
				/* Literal prefix with no runs, then a tail unit
				 * of 1, 3, 4 or 255 bytes. */
				static const size_t tail_len[] = {1, 3, 4, 255};
				size_t tl = tail_len[tail_kind];
				size_t unit_bytes = tl >= 4 ? 5 : tl;
				size_t prefix =
					(size_t)((long)nmax + d) - unit_bytes;
				size_t n = prefix + tl + 3;
				uint8_t *p = malloc(n);
				for (size_t i = 0; i < prefix; i++)
					p[i] = (uint8_t)(i % 251);
				memset(p + prefix, 0xff, tl);
				p[prefix + tl] = 0x7f;
				p[prefix + tl + 1] = 0x7e;
				p[prefix + tl + 2] = 0x7d;
				char name[96];
				snprintf(name, sizeof name,
					 "capacity level=%d tail=%zu d=%ld",
					 level, tl, d);
				current_case = name;
				for (size_t cut = n - 4; cut <= n; cut++) {
					schedule f = {.kind = SCHED_FIXED};
					run_stream(p, cut, level, 0, &f);
					schedule s1 = {.kind = SCHED_SPLIT,
						       .split = cut,
						       .split_action = BZ_RUN};
					run_stream(p, cut, level, 0, &s1);
					schedule s2 = {.kind = SCHED_SPLIT,
						       .split = cut - 1,
						       .split_action = BZ_RUN,
						       .out_chunk = 3};
					run_stream(p, cut, level, 0, &s2);
					schedule s3 = {.kind = SCHED_SPLIT,
						       .split = cut,
						       .split_action =
							       BZ_FLUSH};
					run_stream(p, cut, level, 0, &s3);
					schedule ch = {.kind = SCHED_FIXED,
						       .in_chunk = 5000,
						       .out_chunk = 5000};
					run_stream(p, cut, level, 0, &ch);
					schedule tiny = {.kind = SCHED_FIXED,
							 .in_chunk =
								 1 + cut % 7,
							 .out_chunk = 64};
					run_stream(p, cut, level, 0, &tiny);
				}
				free(p);
			}
	}
	/*
	 * A run that crosses the capacity of a level-1 block: literals ending
	 * 0 to 40 bytes short of it, then 2,000 bytes of one value, then
	 * literals; whole, in a call that ends inside the run, and with a
	 * flush inside the run.
	 */
	size_t nmax = 100000 - 19;
	for (size_t d = 0; d <= 40; d++) {
		size_t prefix = nmax - d, n = prefix + 2000 + 300;
		uint8_t *p = malloc(n);
		for (size_t i = 0; i < n; i++)
			p[i] = (uint8_t)(i % 251);
		memset(p + prefix, 0xff, 2000);
		char name[96];
		snprintf(name, sizeof name, "capacity long run d=%zu", d);
		current_case = name;
		schedule f = {.kind = SCHED_FIXED};
		run_stream(p, n, 1, 0, &f);
		schedule s1 = {.kind = SCHED_SPLIT,
			       .split = prefix + 300,
			       .split_action = BZ_RUN};
		run_stream(p, n, 1, 0, &s1);
		schedule s2 = {.kind = SCHED_SPLIT,
			       .split = prefix + 600,
			       .split_action = BZ_FLUSH};
		run_stream(p, n, 1, 0, &s2);
		free(p);
	}
	suite_report("capacity");
}

/* ---- API cases
 * ------------------------------------------------------------------ */

typedef struct {
	size_t in_use, peak, calls;
	long fail_at; /* fail the n-th allocation (1-based), 0 = never */
	void *expect_opaque;
	int bad_opaque;
} alloc_ctx;

static void *
count_alloc(void *opaque, int items, int size)
{
	alloc_ctx *c = opaque;
	c->calls++;
	if (c->fail_at && (long)c->calls == c->fail_at)
		return NULL;
	size_t n = (size_t)items * (size_t)size;
	size_t *p = malloc(n + 16);
	if (!p)
		return NULL;
	p[0] = n;
	c->in_use += n;
	if (c->in_use > c->peak)
		c->peak = c->in_use;
	return (char *)p + 16;
}

static void
count_free(void *opaque, void *q)
{
	alloc_ctx *c = opaque;
	if (!q)
		return;
	size_t *p = (size_t *)((char *)q - 16);
	c->in_use -= p[0];
	free(p);
}

static void
expect_rc(const char *what, int a, int b)
{
	stats.calls++;
	if (a != b)
		fail("%s: stock %d bzn %d", what, a, b);
}

static void
suite_api(void)
{
	current_case = "api";
	bz_stream a, b;
	/* Init parameter validation; verbosity is not validated. */
	static const int bs[] = {-1, 0, 1, 5, 9, 10, 100};
	static const int wf[] = {-1, 0, 1, 30, 250, 251};
	static const int vb[] = {INT_MIN, -1, 0, 1, 4, 5, 1000};
	for (size_t i = 0; i < sizeof bs / sizeof bs[0]; i++)
		for (size_t j = 0; j < sizeof wf / sizeof wf[0]; j++)
			for (size_t k = 0; k < sizeof vb / sizeof vb[0]; k++) {
				memset(&a, 0, sizeof a);
				memset(&b, 0, sizeof b);
				int ra = REF.init(&a, bs[i], vb[k], wf[j]);
				int rb = NG.init(&b, bs[i], vb[k], wf[j]);
				stats.cases++;
				expect_rc("init params", ra, rb);
				if (ra == BZ_OK) {
					if (!b.bzalloc || !b.bzfree ||
					    !b.state || b.total_in_lo32 ||
					    b.total_out_lo32)
						fail("init did not set up the "
						     "stream");
					expect_rc("end", REF.end(&a),
						  NG.end(&b));
					if (b.state)
						fail("end did not clear state");
				} else if (b.state || b.bzalloc) {
					fail("failed init touched the stream");
				}
			}
	expect_rc("init NULL", REF.init(NULL, 9, 0, 0), NG.init(NULL, 9, 0, 0));
	expect_rc("compress NULL", REF.compress(NULL, BZ_RUN),
		  NG.compress(NULL, BZ_RUN));
	expect_rc("end NULL", REF.end(NULL), NG.end(NULL));
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	expect_rc("compress no state", REF.compress(&a, BZ_RUN),
		  NG.compress(&b, BZ_RUN));
	expect_rc("end no state", REF.end(&a), NG.end(&b));

	/* A copied bz_stream is rejected (state->strm != strm). */
	REF.init(&a, 9, 0, 0);
	NG.init(&b, 9, 0, 0);
	bz_stream ac = a, bc = b;
	expect_rc("compress copy", REF.compress(&ac, BZ_RUN),
		  NG.compress(&bc, BZ_RUN));
	expect_rc("end copy", REF.end(&ac), NG.end(&bc));
	/* Invalid actions while running and while flushing. */
	char inbuf[100], oa[200], ob[200];
	memset(inbuf, 'q', sizeof inbuf);
	for (int act = -2; act <= 5; act++) {
		a.next_in = b.next_in = inbuf;
		a.avail_in = b.avail_in = 0;
		a.next_out = oa;
		b.next_out = ob;
		a.avail_out = b.avail_out = 0;
		if (act == BZ_FLUSH || act == BZ_FINISH)
			continue;
		expect_rc("action while running", REF.compress(&a, act),
			  NG.compress(&b, act));
	}
	expect_rc("end", REF.end(&a), NG.end(&b));

	/* FLUSH with no input exposes "BZh" and holds the level byte. */
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	REF.init(&a, 9, 0, 0);
	NG.init(&b, 9, 0, 0);
	a.next_out = oa;
	b.next_out = ob;
	a.avail_out = b.avail_out = sizeof oa;
	a.avail_in = b.avail_in = 0;
	for (size_t i = 0; i < sizeof oa; i++)
		oa[i] = ob[i] = (char)pattern_byte(i, 0);
	int ra = REF.compress(&a, BZ_FLUSH), rb = NG.compress(&b, BZ_FLUSH);
	expect_rc("empty flush rc", ra, rb);
	if (a.total_out_lo32 != b.total_out_lo32 ||
	    memcmp(oa, ob, sizeof oa) != 0)
		fail("empty flush output: stock %u bytes, bzn %u",
		     a.total_out_lo32, b.total_out_lo32);
	if (verbose)
		printf("empty FLUSH released %u bytes\n", a.total_out_lo32);
	REF.end(&a);
	NG.end(&b);

	/*
	 * Custom allocator: opaque passed through, everything freed, and the
	 * peak within the documented 400k + 8 x block size.
	 */
	for (int level = 1; level <= 9; level++) {
		alloc_ctx c = {0};
		memset(&b, 0, sizeof b);
		b.bzalloc = count_alloc;
		b.bzfree = count_free;
		b.opaque = &c;
		uint8_t *in = malloc(100000 * (size_t)level + 1000),
			*out = malloc(200000 * (size_t)level + 1000);
		if (in == NULL || out == NULL) {
			fprintf(stderr, "out of memory\n");
			exit(2);
		}
		fill_random_input(in, 100000 * (size_t)level + 1000);
		stats.cases++;
		if (NG.init(&b, level, 0, 0) != BZ_OK)
			fail("init with allocator");
		b.next_in = (char *)in;
		b.avail_in = 100000 * (unsigned)level + 1000;
		b.next_out = (char *)out;
		b.avail_out = 200000 * (unsigned)level + 1000;
		int rc;
		while ((rc = NG.compress(&b, BZ_FINISH)) == BZ_FINISH_OK) {
		}
		if (rc != BZ_STREAM_END)
			fail("compress with allocator rc %d", rc);
		size_t budget = 400000 + 8 * 100000 * (size_t)level;
		if (verbose)
			printf("level %d: bzalloc peak %zu bytes (budget "
			       "%zu)\n",
			       level, c.peak, budget);
		if (c.peak > budget)
			fail("level %d: bzalloc peak %zu over %zu", level,
			     c.peak, budget);
		NG.end(&b);
		if (c.in_use != 0)
			fail("level %d: %zu bytes not freed", level, c.in_use);
		free(in);
		free(out);
	}
	/* Allocation failure at every allocation: BZ_MEM_ERROR, nothing leaked.
	 */
	for (long k = 1; k <= 8; k++) {
		alloc_ctx c = {.fail_at = k};
		memset(&b, 0, sizeof b);
		b.bzalloc = count_alloc;
		b.bzfree = count_free;
		b.opaque = &c;
		int rc = NG.init(&b, 9, 0, 0);
		stats.cases++;
		if ((long)c.calls >= k && rc != BZ_MEM_ERROR)
			fail("alloc failure %ld: rc %d", k, rc);
		if (rc == BZ_OK)
			NG.end(&b);
		if (c.in_use != 0)
			fail("alloc failure %ld: %zu bytes leaked", k,
			     c.in_use);
	}
	suite_report("api");
}

/*
 * A new temporary binary stream, removed when closed, as tmpfile() gives; NULL
 * on failure.  Windows' tmpfile() is not safe for tests run in parallel: two
 * processes started together can pick the same name (it then fails with
 * EACCES under Wine), and Microsoft's C runtime puts the file in the root of
 * the current drive.  So there the file is created exclusively in %TEMP%,
 * named after the process, and deleted when its descriptor is closed.
 */
static FILE *
temp_stream(void)
{
#ifdef _WIN32
	static unsigned serial;
	const char *dir = getenv("TEMP");
	char path[1024];

	if (dir == NULL)
		dir = ".";
	for (;;) {
		snprintf(path, sizeof path, "%s/bzng_enc_%d_%u.tmp", dir,
			 _getpid(), serial++);
		int fd = _open(path,
			       _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY |
				       _O_TEMPORARY,
			       _S_IREAD | _S_IWRITE);
		if (fd >= 0)
			return _fdopen(fd, "w+b");
		if (errno != EEXIST)
			return NULL;
	}
#else
	return tmpfile();
#endif
}

/*
 * BZ2_bzBuffToBuffCompress (incl. a too-small output) and BZ2_bzWrite* with
 * random write sizes, stock wrappers over each library.
 */
static void
suite_wrappers(void)
{
	input_list l = {0};
	edge_inputs(&l);
	for (size_t i = 0; i < l.n; i++) {
		input *x = &l.v[i];
		current_case = x->name;
		for (int level = 1; level <= 9; level += 4) {
			size_t cap = x->n + x->n / 50 + 700;
			char *oa = malloc(cap), *ob = malloc(cap);
			for (int shrink = 0; shrink < 2; shrink++) {
				unsigned la = (unsigned)cap, lb = (unsigned)cap;
				if (shrink)
					la = lb = (unsigned)(x->n / 3);
				const size_t offered = la;
				for (size_t k = 0; k < cap; k++)
					oa[k] = ob[k] =
						(char)pattern_byte(k, 0);
				int ra = ref_BZ2_bzBuffToBuffCompress(
					oa, &la, (char *)x->data,
					(unsigned)x->n, level, 0, 0);
				int rb = BZ2_bzBuffToBuffCompress(
					ob, &lb, (char *)x->data,
					(unsigned)x->n, level, 0, 0);
				stats.cases++;
				expect_rc("buff2buff rc", ra, rb);
				/*
				 * Past the stream (or past the space offered,
				 * when it did not fit) both buffers keep the
				 * pattern.
				 */
				for (size_t k = ra == BZ_OK ? la : offered;
				     k < cap; k++) {
					if (oa[k] != (char)pattern_byte(k, 0) ||
					    ob[k] != (char)pattern_byte(k, 0)) {
						fail("buff2buff wrote byte %zu "
						     "(level %d)",
						     k, level);
						break;
					}
				}
				if (ra == BZ_OK &&
				    (la != lb || memcmp(oa, ob, la) != 0)) {
					/* allow periodic origPtr: decode
					 * instead */
					unsigned dl = (unsigned)x->n + 1;
					char *d = malloc(x->n + 1);
					int rd = ref_BZ2_bzBuffToBuffDecompress(
						d, &dl, ob, lb, 0, 0);
					if (la != lb || rd != BZ_OK ||
					    dl != x->n ||
					    memcmp(d, x->data, x->n) != 0)
						fail("buff2buff output differs "
						     "(level %d)",
						     level);
					else
						stats.patched++;
					free(d);
				} else if (ra == BZ_OK) {
					stats.identical++;
				}
			}
			free(oa);
			free(ob);
			/* BZ2_bzWrite to a temporary file. */
			FILE *fa = temp_stream(), *fb = temp_stream();
			if (fa == NULL || fb == NULL) {
				perror("temporary file");
				exit(2);
			}
			int ea, eb;
			BZFILE *wa = ref_BZ2_bzWriteOpen(&ea, fa, level, 0, 0);
			BZFILE *wb = BZ2_bzWriteOpen(&eb, fb, level, 0, 0);
			expect_rc("writeopen", ea, eb);
			for (size_t p = 0; p < x->n;) {
				size_t k =
					1 + rnd_below(rnd_below(2) ? 7 : 6000);
				if (k > x->n - p)
					k = x->n - p;
				ref_BZ2_bzWrite(&ea, wa, x->data + p, (int)k);
				BZ2_bzWrite(&eb, wb, x->data + p, (int)k);
				expect_rc("write", ea, eb);
				p += k;
			}
			unsigned ia, ia2, oa1, oa2, ib, ib2, ob1, ob2;
			ref_BZ2_bzWriteClose64(&ea, wa, 0, &ia, &ia2, &oa1,
					       &oa2);
			BZ2_bzWriteClose64(&eb, wb, 0, &ib, &ib2, &ob1, &ob2);
			expect_rc("writeclose", ea, eb);
			stats.cases++;
			if (ia != ib || ia2 != ib2 || oa1 != ob1 || oa2 != ob2)
				fail("writeclose counts differ");
			long sa = ftell(fa), sb = ftell(fb);
			rewind(fa);
			rewind(fb);
			char *ba = malloc((size_t)sa + 1),
			     *bb = malloc((size_t)sb + 1);
			size_t ga = fread(ba, 1, (size_t)sa, fa),
			       gb = fread(bb, 1, (size_t)sb, fb);
			if (ga != gb || memcmp(ba, bb, ga) != 0) {
				unsigned dl = (unsigned)x->n + 1;
				char *d = malloc(x->n + 1);
				int rd = ref_BZ2_bzBuffToBuffDecompress(
					d, &dl, bb, (unsigned)gb, 0, 0);
				if (ga != gb || rd != BZ_OK || dl != x->n ||
				    memcmp(d, x->data, x->n) != 0)
					fail("bzWrite output differs (level "
					     "%d)",
					     level);
				else
					stats.patched++;
				free(d);
			} else {
				stats.identical++;
			}
			free(ba);
			free(bb);
			fclose(fa);
			fclose(fb);
		}
	}
	suite_report("wrappers");
}

/*
 * Corpus files cut into streams of `chunk` bytes (0 = whole file), each run
 * with several fixed schedules and one random one.
 */
static void
suite_corpus(int argc, char **argv, size_t chunk, int level, long max_streams)
{
	for (int f = 0; f < argc; f++) {
		FILE *fp = fopen(argv[f], "rb");
		if (!fp) {
			perror(argv[f]);
			stats.failures++;
			continue;
		}
		fseek(fp, 0, SEEK_END);
		size_t len = (size_t)ftell(fp);
		fseek(fp, 0, SEEK_SET);
		uint8_t *data = malloc(len ? len : 1);
		if (fread(data, 1, len, fp) != len) {
			perror(argv[f]);
			exit(1);
		}
		fclose(fp);
		size_t cs = chunk ? chunk : len;
		long streams = 0;
		for (size_t off = 0; off < len && streams < max_streams;
		     off += cs, streams++) {
			size_t n = len - off < cs ? len - off : cs;
			char name[128];
			snprintf(name, sizeof name, "%s@%zu", argv[f], off);
			current_case = name;
			const uint8_t *p = data + off;
			schedule one = {.kind = SCHED_FIXED};
			run_stream(p, n, level, 0, &one);
			schedule tool = {.kind = SCHED_FIXED,
					 .in_chunk = 5000,
					 .out_chunk = 5000};
			run_stream(p, n, level, 0, &tool);
			schedule k64 = {.kind = SCHED_FIXED,
					.in_chunk = 65536,
					.out_chunk = 4096};
			run_stream(p, n, level, 0, &k64);
			schedule rnd_sc = {.kind = SCHED_RANDOM,
					   .in_max = 70000,
					   .out_max = 70000,
					   .p_flush = 2,
					   .p_finish_partial = 0,
					   .p_err = 20,
					   .p_zero_out = 20};
			run_stream(p, n, level, 0, &rnd_sc);
		}
		free(data);
	}
	suite_report("corpus");
}

int
main(int argc, char **argv)
{
	require_requested_tier();
	long rounds = 200, max_streams = 1000000;
	size_t chunk = 0;
	int level = 9, opt;
	uint64_t seed = 1;
	while ((opt = getopt(argc, argv, "s:r:c:l:m:v")) != -1) {
		if (opt == 's')
			seed = strtoull(optarg, NULL, 10);
		else if (opt == 'r')
			rounds = atol(optarg);
		else if (opt == 'c')
			chunk = (size_t)atoll(optarg);
		else if (opt == 'l')
			level = atoi(optarg);
		else if (opt == 'm')
			max_streams = atol(optarg);
		else if (opt == 'v')
			verbose = 1;
		else
			return 2;
	}
	if (optind >= argc) {
		fprintf(stderr,
			"usage: difftest [-s seed] [-r rounds] [-c chunk] [-l "
			"level] [-m max_streams] [-v] "
			"api|edge|random|capacity|wrappers|corpus [files]\n");
		return 2;
	}
	rng_state = seed;
	const char *suite = argv[optind];
	suite_header();
	if (strcmp(suite, "api") == 0)
		suite_api();
	else if (strcmp(suite, "edge") == 0)
		suite_edge();
	else if (strcmp(suite, "random") == 0)
		suite_random(rounds);
	else if (strcmp(suite, "capacity") == 0)
		suite_capacity();
	else if (strcmp(suite, "wrappers") == 0)
		suite_wrappers();
	else if (strcmp(suite, "corpus") == 0)
		suite_corpus(argc - optind - 1, argv + optind + 1, chunk, level,
			     max_streams);
	else {
		fprintf(stderr, "unknown suite %s\n", suite);
		return 2;
	}
	return stats.failures ? 1 : 0;
}
