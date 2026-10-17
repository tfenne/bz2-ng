/*-------------------------------------------------------------*/
/*--- Legacy decompression API (BZ2_bzDecompressInit,       ---*/
/*--- BZ2_bzDecompress, BZ2_bzDecompressEnd).               ---*/
/*---                                          decompress.c ---*/
/*-------------------------------------------------------------*/

/*
 * ------------------------------------------------------------------
 * Derived from decompress.c and bzlib.c of bzip2/libbzip2 1.0.8,
 * Copyright (C) 1996-2019 Julian Seward <jseward@acm.org>.
 * This program is released under the terms of the license contained
 * in licenses/bzip2-1.0.8.txt.
 * ------------------------------------------------------------------
 */

/*
 * ALTERED for bz2-ng: this is not the original libbzip2 code. The header
 * parser keeps the structure of libbzip2's BZ2_decompress (a resumable
 * machine with a case label at every read, reading from a 64-bit bit
 * buffer); the symbol loop (a kernel, kern_template.h), the inverse BWT
 * (dec_block.c) and the output stage below are new.
 *
 * Observable behaviour is libbzip2's, call for call: every return code and
 * the values of next_in, avail_in, total_in, next_out, avail_out and
 * total_out after every call. The rules that make that possible:
 * - input is consumed lazily, as libbzip2 does: when the decoder waits for
 *   input it has consumed all of it; otherwise exactly ceil(bits used / 8)
 *   bytes, so bytes read ahead by the 64-bit refill are given back;
 * - a bit pattern that is no code is an error only once 21 bits from its
 *   start are available, as in libbzip2's bit-serial loop;
 * - the fast symbol path runs only with BZNG_FAST_IN_MARGIN input bytes in
 *   hand, so it never needs to suspend, and any anomaly in a fast group is
 *   resolved by the exact path or by re-decoding to the exact point;
 * - output is resumable at every byte; block CRC errors are reported in the
 *   call that delivers the block's last byte, and the next block header is
 *   parsed in the same call, as libbzip2 does.
 *
 * One deliberate difference: a call after an error return gets the same
 * error again (libbzip2 resumes its state machine, which its manual does not
 * allow and which reaches undefined behaviour inside libbzip2).  The
 * exception is a combined-CRC mismatch at the end of a stream: the stream is
 * finished, and the next call returns BZ_SEQUENCE_ERROR, as in libbzip2.
 */
#include <stdlib.h>
#include <string.h>

#include "dec.h"

/* ---- kernels ------------------------------------------------------------- */

/* The symbol loop's kernel, chosen on first use (kern.h). */
static int dispatch_symbols(bzng_dstate *s, bzng_bits *br);

static volatile symbols_func_t symbols_impl = dispatch_symbols;

/* Choose the best implementation at runtime. */
static int
dispatch_symbols(bzng_dstate *s, bzng_bits *br)
{
	symbols_func_t f = arch_select_symbols();

	if (f == NULL)
		f = bzng_symbols_scalar;

	symbols_impl = f;
	return f(s, br);
}

/* The bulk un-RLE1 kernel, chosen on first use (kern.h). */
static void dispatch_unrle(bzng_unrle_io *io);

static volatile unrle_func_t unrle_impl = dispatch_unrle;

/* Choose the best implementation at runtime. */
static void
dispatch_unrle(bzng_unrle_io *io)
{
	unrle_func_t f = arch_select_unrle();

	if (f == NULL)
		f = bzng_unrle_scalar;

	unrle_impl = f;
	f(io);
}

/* ---- output -------------------------------------------------------------- */

/*
 * un-RLE1 from the text source into the caller's buffer, resumable at every
 * output byte. Text positions 0..n-1 are decoded with the usual rule (four
 * equal bytes, then a count byte). A block whose text ends with four equal
 * bytes and no count byte is libbz2's one "corrupt" output case: it emits the
 * run with the count taken from text position n (the walk wrapping round),
 * then reports BZ_DATA_ERROR; in libbz2's fast, non-randomised path the error
 * is raised as soon as the run is out and that call's pointer updates are
 * lost, while the small and randomised paths raise it at the next call with
 * output space.
 * Returns BZNG_BLOCK_READY when the block's last byte is out, BZ_OK when the
 * output buffer is full, BZ_DATA_ERROR or BZNG_DISCARD (corrupt, fast path).
 */
static int
output(bzng_dstate *s, bz_stream *strm)
{
	u8 *out = (u8 *)strm->next_out;
	u8 *const out0 = out;
	u32 avail = strm->avail_out;
	int result;
	for (;;) {
		if (s->pend_len) {
			const u32 k = s->pend_len < avail ? s->pend_len : avail;
			if (k) {
				memset(out, s->pend_ch, k);
				out += k;
				avail -= k;
				s->pend_len -= k;
			}
			if (s->pend_len) {
				result = BZ_OK;
				break;
			}
		}
		if (s->corrupt_pending) {
			if (!s->small && !s->randomised) {
				result = BZNG_DISCARD;
				break;
			}
			result = avail == 0 ? BZ_OK : BZ_DATA_ERROR;
			break;
		}
		if (s->rd_len == 0) {
			if (s->text_left == 0) {
				result = BZNG_BLOCK_READY;
				break;
			}
			bzng_next_piece(s, s->text_left);
		}
		if (s->ur_eq == 4) {
			s->pend_len = *s->rd_ptr++;
			s->pend_ch = s->ur_b;
			s->rd_len--;
			s->text_left--;
			s->ur_eq = 0;
			continue;
		}
		if (s->rd_len >= 20 && avail >= 16) {
			const u8 *p = s->rd_ptr;
			u32 plen = s->rd_len;
			if (s->ur_eq) {
				/*
				 * a partial run carried in: it completes iff
				 * the next 4 - eq bytes equal it; otherwise the
				 * bytes up to the first different one are
				 * literals and that one starts afresh
				 */
				const u32 need = 4u - s->ur_eq;
				u32 m = 0;
				while (m < need && p[m] == s->ur_b)
					m++;
				memcpy(out, p, m);
				out += m;
				avail -= m;
				p += m;
				plen -= m;
				s->text_left -= m;
				s->ur_eq = m == need ? 4 : 0;
				s->rd_ptr = p;
				s->rd_len = plen;
				if (s->ur_eq == 4)
					continue;
			}
			/*
			 * Clean state: the bulk kernel copies literals and
			 * expands runs while it has its margins.
			 */
			bzng_unrle_io io = {.in = p,
					    .in_len = plen,
					    .text_after = s->text_left - plen,
					    .out = out,
					    .avail = avail};
			unrle_impl(&io);
			out = io.out;
			avail = io.avail;
			if (io.pend_len) {
				s->pend_len = io.pend_len;
				s->pend_ch = io.pend_ch;
			}
			s->ur_b = io.last;
			s->ur_eq = io.eq;
			s->text_left -= (u32)(io.in - s->rd_ptr);
			s->rd_ptr = io.in;
			s->rd_len = io.in_len;
			/*
			 * Without progress (the kernel keeps clear of the
			 * block's last bytes), go on byte by byte.
			 */
			if (io.in != p)
				continue;
		}
		if (avail == 0) {
			result = BZ_OK;
			break;
		}
		const u8 x = *s->rd_ptr++;
		s->rd_len--;
		s->text_left--;
		*out++ = x;
		avail--;
		if (s->ur_eq && x == s->ur_b) {
			s->ur_eq++;
		} else {
			s->ur_b = x;
			s->ur_eq = 1;
		}
		if (s->ur_eq == 4 && s->text_left == 0) {
			/* four equal bytes end the block: the count is text
			 * position n */
			if (s->rd_len == 0)
				bzng_next_piece(s, 1);
			s->pend_len = *s->rd_ptr;
			s->pend_ch = s->ur_b;
			s->ur_eq = 0;
			s->corrupt_pending = 1;
		}
	}
	if (result == BZNG_DISCARD)
		return result; /* libbz2 returns without saving */
	const u32 written = strm->avail_out - avail;
	if (written)
		s->calc_block_crc =
			bzng_crc32(s->calc_block_crc, out0, written);
	strm->next_out = (char *)out;
	strm->avail_out = avail;
	const u32 old = strm->total_out_lo32;
	strm->total_out_lo32 += written;
	if (strm->total_out_lo32 < old)
		strm->total_out_hi32++;
	return result;
}

/*
 * Allocates the per-stream arrays for the header digit: one bzalloc call
 * (two in small mode), as libbz2 does, so that an allocation failure is
 * reported at the same call.
 */
static int
alloc_stream(bzng_dstate *s)
{
	bz_stream *strm = s->strm;

	bzng_size_stream_arrays(s);
	s->area = BZALLOC(s->area_bytes);
	if (s->small)
		s->area2 = BZALLOC(s->area2_bytes);
	if (s->area == NULL || (s->small && s->area2 == NULL))
		return BZ_MEM_ERROR;
	bzng_place_stream_arrays(s);
	return BZ_OK;
}

/*
 * A resumable read of n bits into var: libbz2's GET_BITS.  The case label
 * lets a later call resume at this read; the code before it falls through.
 */
#define GET_BITS_CASE(lab, var, n)            \
	case lab:                             \
		if (!bzng_br_fill(br, (n))) { \
			s->state = (lab);     \
			return BZNG_NEED;     \
		}                             \
		(var) = bzng_br_take(br, (n));
#define GET_BITS(lab, var, n) \
	FALLTHROUGH;          \
	GET_BITS_CASE(lab, var, n)

#define RETURN(code)            \
	do {                    \
		s->state = cur; \
		return (code);  \
	} while (0)

/*
 * Header parsing and the symbol loop (libbz2's BZ2_decompress). Returns
 * BZNG_NEED (input exhausted), BZNG_BLOCK_READY (block ready for output),
 * BZ_STREAM_END, or an error code.
 */
static int
decode(bzng_dstate *s, bzng_bits *br)
{
	u32 uc;
	s32 cur = s->state;
	switch (s->state) {
		GET_BITS_CASE(BZ_X_MAGIC_1, uc, 8);
		cur = BZ_X_MAGIC_1;
		if (uc != BZ_HDR_B)
			RETURN(BZ_DATA_ERROR_MAGIC);
		GET_BITS(BZ_X_MAGIC_2, uc, 8);
		cur = BZ_X_MAGIC_2;
		if (uc != BZ_HDR_Z)
			RETURN(BZ_DATA_ERROR_MAGIC);
		GET_BITS(BZ_X_MAGIC_3, uc, 8);
		cur = BZ_X_MAGIC_3;
		if (uc != BZ_HDR_h)
			RETURN(BZ_DATA_ERROR_MAGIC);
		GET_BITS(BZ_X_MAGIC_4, uc, 8);
		cur = BZ_X_MAGIC_4;
		if (uc < BZ_HDR_0 + 1 || uc > BZ_HDR_0 + 9)
			RETURN(BZ_DATA_ERROR_MAGIC);
		s->block_size_100k = (s32)uc - BZ_HDR_0;
		if (alloc_stream(s) != BZ_OK)
			RETURN(BZ_MEM_ERROR);

		GET_BITS(BZ_X_BLKHDR_1, uc, 8);
		cur = BZ_X_BLKHDR_1;
		if (uc == 0x17)
			goto endhdr_2;
		if (uc != 0x31)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_BLKHDR_2, uc, 8);
		cur = BZ_X_BLKHDR_2;
		if (uc != 0x41)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_BLKHDR_3, uc, 8);
		cur = BZ_X_BLKHDR_3;
		if (uc != 0x59)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_BLKHDR_4, uc, 8);
		cur = BZ_X_BLKHDR_4;
		if (uc != 0x26)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_BLKHDR_5, uc, 8);
		cur = BZ_X_BLKHDR_5;
		if (uc != 0x53)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_BLKHDR_6, uc, 8);
		cur = BZ_X_BLKHDR_6;
		if (uc != 0x59)
			RETURN(BZ_DATA_ERROR);

		s->curr_block_no++;
		if (s->verbosity >= 2)
			VPrintf1("\n    [%d: huff+mtf ", (int)s->curr_block_no);

		s->stored_block_crc = 0;
		GET_BITS(BZ_X_BCRC_1, uc, 8);
		s->stored_block_crc = (s->stored_block_crc << 8) | uc;
		GET_BITS(BZ_X_BCRC_2, uc, 8);
		s->stored_block_crc = (s->stored_block_crc << 8) | uc;
		GET_BITS(BZ_X_BCRC_3, uc, 8);
		s->stored_block_crc = (s->stored_block_crc << 8) | uc;
		GET_BITS(BZ_X_BCRC_4, uc, 8);
		s->stored_block_crc = (s->stored_block_crc << 8) | uc;

		GET_BITS(BZ_X_RANDBIT, uc, 1);
		s->randomised = (u8)uc;

		s->orig_ptr = 0;
		GET_BITS(BZ_X_ORIGPTR_1, uc, 8);
		s->orig_ptr = (s->orig_ptr << 8) | (s32)uc;
		GET_BITS(BZ_X_ORIGPTR_2, uc, 8);
		s->orig_ptr = (s->orig_ptr << 8) | (s32)uc;
		GET_BITS(BZ_X_ORIGPTR_3, uc, 8);
		cur = BZ_X_ORIGPTR_3;
		s->orig_ptr = (s->orig_ptr << 8) | (s32)uc;
		if (s->orig_ptr < 0)
			RETURN(BZ_DATA_ERROR);
		if (s->orig_ptr > 10 + 100000 * s->block_size_100k)
			RETURN(BZ_DATA_ERROR);

		for (s->i = 0; s->i < 16; s->i++) {
			GET_BITS(BZ_X_MAPPING_1, uc, 1);
			s->in_use16[s->i] = (u8)uc;
		}
		s->n_in_use = 0;
		memset(s->seq_to_unseq, 0, sizeof s->seq_to_unseq);
		for (s->i = 0; s->i < 16; s->i++) {
			if (s->in_use16[s->i]) {
				for (s->j = 0; s->j < 16; s->j++) {
					GET_BITS(BZ_X_MAPPING_2, uc, 1);
					if (uc)
						s->seq_to_unseq[s->n_in_use++] =
							(u8)(s->i * 16 + s->j);
				}
			}
		}
		cur = BZ_X_MAPPING_2;
		if (s->n_in_use == 0)
			RETURN(BZ_DATA_ERROR);
		s->alpha_size = s->n_in_use + 2;

		GET_BITS(BZ_X_SELECTOR_1, uc, 3);
		cur = BZ_X_SELECTOR_1;
		s->n_groups = (s32)uc;
		if (s->n_groups < 2 || s->n_groups > BZ_N_GROUPS)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_SELECTOR_2, uc, 15);
		cur = BZ_X_SELECTOR_2;
		s->n_selectors = (s32)uc;
		if (s->n_selectors < 1)
			RETURN(BZ_DATA_ERROR);
		for (int v = 0; v < s->n_groups; v++)
			s->sel_pos[v] = (u8)v;
		/*
		 * selector MTF values are undone as they arrive; libbz2 1.0.8
		 * keeps the first 18002 and reads and ignores the rest. While 8
		 * input bytes remain, whole selectors are read from the 64-bit
		 * buffer (a unary code is a count of leading ones); otherwise
		 * bit by bit.
		 */
		s->i = 0;
		while (s->i < s->n_selectors) {
			if (br->cnt >= 8 || BZNG_SPAN(br->p, br->end) >= 8) {
				if (br->cnt < 8)
					bzng_br_refill_fast(br);
				u32 j = (u32)63 - bsr64(~br->buf | 1);
				if (j >= (u32)s->n_groups) {
					bzng_br_take(br, (u32)s->n_groups);
					cur = BZ_X_SELECTOR_3;
					RETURN(BZ_DATA_ERROR);
				}
				bzng_br_take(br, j + 1);
				s->j = (s32)j;
			} else {
				s->j = 0;
				for (;;) {
					GET_BITS(BZ_X_SELECTOR_3, uc, 1);
					cur = BZ_X_SELECTOR_3;
					if (uc == 0)
						break;
					s->j++;
					if (s->j >= s->n_groups)
						RETURN(BZ_DATA_ERROR);
				}
			}
			if (s->i < BZ_MAX_SELECTORS) {
				int v = s->j;
				const u8 tmp = s->sel_pos[v];
				for (; v > 0; v--)
					s->sel_pos[v] = s->sel_pos[v - 1];
				s->sel_pos[0] = tmp;
				s->selector[s->i] = tmp;
			}
			s->i++;
		}
		if (s->n_selectors > BZ_MAX_SELECTORS)
			s->n_selectors = BZ_MAX_SELECTORS;

		/*
		 * code lengths: a 5-bit start, then per symbol steps of
		 * 10 (+1) or 11 (-1) ended by 0, the range checked before
		 * every step; two bits at a time from the 64-bit buffer while
		 * 8 input bytes remain
		 */
		for (s->t = 0; s->t < s->n_groups; s->t++) {
			GET_BITS(BZ_X_CODING_1, uc, 5);
			s->curr = (s32)uc;
			s->i = 0;
			while (s->i < s->alpha_size) {
				for (;;) {
					cur = BZ_X_CODING_1;
					if (s->curr < 1 || s->curr > 20)
						RETURN(BZ_DATA_ERROR);
					if (br->cnt < 2 &&
					    BZNG_SPAN(br->p, br->end) >= 8)
						bzng_br_refill_fast(br);
					if (br->cnt >= 2) {
						const u32 two =
							(u32)(br->buf >> 62);
						if (two < 2) {
							bzng_br_take(br, 1);
							break;
						}
						bzng_br_take(br, 2);
						s->curr += two == 2 ? 1 : -1;
						continue;
					}
					GET_BITS(BZ_X_CODING_2, uc, 1);
					if (uc == 0)
						break;
					GET_BITS(BZ_X_CODING_3, uc, 1);
					if (uc == 0)
						s->curr++;
					else
						s->curr--;
				}
				s->len[s->t][s->i] = (u8)s->curr;
				s->i++;
			}
		}

		for (int t = 0; t < s->n_groups; t++)
			bzng_build_htable(&s->tab[t], s->len[t], s->alpha_size);

		s->eob = (u32)s->n_in_use + 1;
		s->nblock_max = 100000u * (u32)s->block_size_100k;
		s->group_no = -1;
		s->group_pos = 0;
		s->nblock = 0;
		s->run = 0;
		s->run_shift = 0;
		memcpy(s->mtf, s->seq_to_unseq, 256);
		s->state = BZNG_ST_SYMBOLS;
		FALLTHROUGH;
	case BZNG_ST_SYMBOLS: {
		const int r = symbols_impl(s, br);
		if (r == BZNG_NEED) {
			s->state = BZNG_ST_SYMBOLS;
			return BZNG_NEED;
		}
		if (r != BZNG_EOB) {
			s->state = BZNG_ST_SYMBOLS;
			return r;
		}
		if (s->orig_ptr < 0 || (u32)s->orig_ptr >= s->nblock) {
			s->state = BZNG_ST_SYMBOLS;
			return BZ_DATA_ERROR;
		}
		s->state = BZNG_ST_OUTPUT;
		if (s->verbosity >= 2)
			VPrintf0("rt+rld");
		bzng_finish_block(s);
		return BZNG_BLOCK_READY;
	}

	endhdr_2:
		GET_BITS_CASE(BZ_X_ENDHDR_2, uc, 8);
		cur = BZ_X_ENDHDR_2;
		if (uc != 0x72)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_ENDHDR_3, uc, 8);
		cur = BZ_X_ENDHDR_3;
		if (uc != 0x45)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_ENDHDR_4, uc, 8);
		cur = BZ_X_ENDHDR_4;
		if (uc != 0x38)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_ENDHDR_5, uc, 8);
		cur = BZ_X_ENDHDR_5;
		if (uc != 0x50)
			RETURN(BZ_DATA_ERROR);
		GET_BITS(BZ_X_ENDHDR_6, uc, 8);
		cur = BZ_X_ENDHDR_6;
		if (uc != 0x90)
			RETURN(BZ_DATA_ERROR);
		s->stored_combined_crc = 0;
		GET_BITS(BZ_X_CCRC_1, uc, 8);
		s->stored_combined_crc = (s->stored_combined_crc << 8) | uc;
		GET_BITS(BZ_X_CCRC_2, uc, 8);
		s->stored_combined_crc = (s->stored_combined_crc << 8) | uc;
		GET_BITS(BZ_X_CCRC_3, uc, 8);
		s->stored_combined_crc = (s->stored_combined_crc << 8) | uc;
		GET_BITS(BZ_X_CCRC_4, uc, 8);
		s->stored_combined_crc = (s->stored_combined_crc << 8) | uc;
		s->state = BZNG_ST_IDLE;
		return BZ_STREAM_END;

	default:
		AssertH(False, 4001);
	}
	AssertH(False, 4002);
	return BZ_SEQUENCE_ERROR;
}

#undef GET_BITS
#undef GET_BITS_CASE
#undef RETURN

int
bzng_decompress_init(bz_stream *strm, int verbosity, int small, int layout)
{
	bzng_dstate *s;
	void *raw;
	if (!bzng_config_ok())
		return BZ_CONFIG_ERROR;
	if (strm == NULL)
		return BZ_PARAM_ERROR;
	if (small != 0 && small != 1)
		return BZ_PARAM_ERROR;
	if (verbosity < 0 || verbosity > 4)
		return BZ_PARAM_ERROR;
	if (strm->bzalloc == NULL)
		strm->bzalloc = bzng_default_bzalloc;
	if (strm->bzfree == NULL)
		strm->bzfree = bzng_default_bzfree;
	raw = BZALLOC(sizeof(bzng_dstate) + STATE_ALIGN - 1);
	if (raw == NULL)
		return BZ_MEM_ERROR;
	s = align_state(raw);
	s->raw = raw;
	s->strm = strm;
	strm->state = s;
	s->state = BZ_X_MAGIC_1;
	s->sticky = 0;
	s->bit_buf = 0;
	s->bit_cnt = 0;
	s->calc_combined_crc = 0;
	strm->total_in_lo32 = 0;
	strm->total_in_hi32 = 0;
	strm->total_out_lo32 = 0;
	strm->total_out_hi32 = 0;
	s->small = (u8)small;
	s->verbosity = (u8)verbosity;
	s->layout = (u8)layout;
	s->area = NULL;
	s->area2 = NULL;
	s->curr_block_no = 0;
	return BZ_OK;
}

/*
 * libbz2's BZ2_bzDecompressInit, with the default per-stream memory layout
 * (BZNG_DEFAULT_LAYOUT).  Allocates only the state; the per-stream arrays
 * follow when the stream header gives the block size.
 */
int
BZ_API(BZ2_bzDecompressInit)(bz_stream *strm, int verbosity, int small)
{
	return bzng_decompress_init(strm, verbosity, small,
				    BZNG_DEFAULT_LAYOUT);
}

/*
 * Hands back the input bytes read ahead of libbz2's position (only bytes
 * read in this call; see the file comment) and stores the remaining bits.
 */
static void
sync_input(bzng_dstate *s, bz_stream *strm, bzng_bits *br, const u8 *in0,
	   int waiting)
{
	if (!waiting) {
		u32 give = br->cnt / 8;
		const u32 read = in0 ? (u32)BZNG_SPAN(in0, br->p) : 0;
		if (give > read)
			give = read;
		br->p -= give;
		br->cnt -= 8 * give;
	}
	s->bit_cnt = br->cnt;
	s->bit_buf = br->cnt ? br->buf & (~(u64)0 << (64 - br->cnt)) : 0;
	const u32 used = in0 ? (u32)BZNG_SPAN(in0, br->p) : 0;
	strm->next_in = (char *)br->p;
	strm->avail_in -= used;
	const u32 old = strm->total_in_lo32;
	strm->total_in_lo32 += used;
	if (strm->total_in_lo32 < old)
		strm->total_in_hi32++;
}

/*
 * libbz2's BZ2_bzDecompress: decodes as much as the caller's input and output
 * allow.  Return codes, stream fields and the bytes delivered after every call
 * follow libbz2 1.0.8, except that a call after an error returns that error
 * again; after a combined-CRC mismatch at the end of a stream the next call
 * returns BZ_SEQUENCE_ERROR, as in libbz2.
 */
int
BZ_API(BZ2_bzDecompress)(bz_stream *strm)
{
	bzng_dstate *s;
	if (strm == NULL)
		return BZ_PARAM_ERROR;
	s = strm->state;
	if (s == NULL)
		return BZ_PARAM_ERROR;
	if (s->strm != strm)
		return BZ_PARAM_ERROR;
	if (s->sticky)
		return s->sticky;

	const u8 *const in0 = (const u8 *)strm->next_in;
	bzng_bits br = {s->bit_buf, s->bit_cnt, in0,
			strm->avail_in ? in0 + strm->avail_in : in0};
	int ret;
	for (;;) {
		if (s->state == BZNG_ST_IDLE) {
			ret = BZ_SEQUENCE_ERROR;
			break;
		}
		if (s->state == BZNG_ST_OUTPUT) {
			/* output never touches the input, so sync it first */
			const int r = output(s, strm);
			if (r == BZNG_DISCARD || r == BZ_DATA_ERROR) {
				ret = BZ_DATA_ERROR;
				s->sticky = ret;
				break;
			}
			if (r == BZ_OK) {
				ret = BZ_OK;
				break;
			}
			s->calc_block_crc = ~s->calc_block_crc;
			if (s->verbosity >= 3)
				VPrintf2(" {0x%08x, 0x%08x}",
					 (unsigned)s->stored_block_crc,
					 (unsigned)s->calc_block_crc);
			if (s->verbosity >= 2)
				VPrintf0("]");
			if (s->calc_block_crc != s->stored_block_crc) {
				/*
				 * libbz2 leaves the block finished; every later
				 * call fails the same way
				 */
				ret = BZ_DATA_ERROR;
				s->sticky = ret;
				break;
			}
			s->calc_combined_crc = (s->calc_combined_crc << 1) |
					       (s->calc_combined_crc >> 31);
			s->calc_combined_crc ^= s->calc_block_crc;
			s->state = BZ_X_BLKHDR_1;
		}
		const int r = decode(s, &br);
		if (r == BZNG_BLOCK_READY)
			continue;
		if (r == BZNG_NEED) {
			sync_input(s, strm, &br, in0, 1);
			return BZ_OK;
		}
		if (r == BZ_STREAM_END) {
			if (s->verbosity >= 3)
				VPrintf2("\n    combined CRCs: stored = "
					 "0x%08x, computed = 0x%08x",
					 (unsigned)s->stored_combined_crc,
					 (unsigned)s->calc_combined_crc);
			/*
			 * Not sticky: the stream is finished either way, so
			 * the next call is BZ_SEQUENCE_ERROR, as in libbz2.
			 */
			ret = s->calc_combined_crc != s->stored_combined_crc ?
				      BZ_DATA_ERROR :
				      BZ_STREAM_END;
			break;
		}
		ret = r;
		s->sticky = r;
		break;
	}
	sync_input(s, strm, &br, in0, 0);
	return ret;
}

/* libbz2's BZ2_bzDecompressEnd: frees the state and the per-stream arrays. */
int
BZ_API(BZ2_bzDecompressEnd)(bz_stream *strm)
{
	bzng_dstate *s;
	if (strm == NULL)
		return BZ_PARAM_ERROR;
	s = strm->state;
	if (s == NULL)
		return BZ_PARAM_ERROR;
	if (s->strm != strm)
		return BZ_PARAM_ERROR;
	if (s->area != NULL)
		BZFREE(s->area);
	if (s->area2 != NULL)
		BZFREE(s->area2);
	BZFREE(s->raw);
	strm->state = NULL;
	return BZ_OK;
}
