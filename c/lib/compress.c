/*-------------------------------------------------------------*/
/*--- Legacy compression API (BZ2_bzCompressInit,           ---*/
/*--- BZ2_bzCompress, BZ2_bzCompressEnd).                   ---*/
/*---                                            compress.c ---*/
/*-------------------------------------------------------------*/

/*
 * ------------------------------------------------------------------
 * Derived from bzlib.c and compress.c of bzip2/libbzip2 1.0.8,
 * Copyright (C) 1996-2019 Julian Seward <jseward@acm.org>.
 * This program is released under the terms of the license contained
 * in licenses/bzip2-1.0.8.txt.
 * ------------------------------------------------------------------
 */

/*
 * ALTERED for bz2-ng: this is not the original libbzip2 code.
 *
 * The state machine is libbzip2's (bzlib.c: handle_compress, BZ2_bzCompress
 * and their helpers, kept line for line so every return code and counter
 * matches); what changes is underneath it:
 * - input is RLE1-coded in bulk per call (the rle_fill kernel) instead of
 *   byte by byte, with the same unit and block-full rules;
 * - a completed block goes through the bz2-ng block core (enc_block.c),
 *   with BZ2_compressBlock's stream framing redone around it;
 * - the compressed block is staged in the tail of the sort work array and
 *   released exactly as libbzip2's bit writer releases it: after a block
 *   that is not the last, libbzip2 has written out every whole byte that
 *   preceded its final bsW call (the end-of-block code), and holds the rest
 *   (< 32 bits) until the next block. Those held bits are carried here too,
 *   so the bytes available after every call match libbzip2's.
 *
 * workFactor is validated (0-250) and otherwise unused (it chose libbzip2's
 * fallback sorter), and libbzip2's sorter diagnostics are not printed.
 */
#include "bzlib_private.h"
#include "enc.h"
#include "sort.h"

/*
 * Compression state; replaces libbzip2's EState. Allocated through bzalloc
 * with slack and aligned internally (lib_common.h); raw is what bzfree gets.
 */
typedef struct {
	bz_stream *strm;
	void *raw;
	s32 mode;
	s32 state;
	u32 avail_in_expect;
	bzng_rle_state rle;
	s32 nblock_max;
	s32 block_no;
	u32 combined_crc;
	s32 block_size_100k;
	s32 verbosity;
	/* Released bytes of the current block: zbits[state_out_pos..num_z). */
	u8 *zbits;
	s32 num_z;
	s32 state_out_pos;
	/*
	 * Bits written but held back (libbzip2's bsBuff/bsLive between blocks).
	 */
	u64 carry;
	u32 carry_n;
	bzng_enc_core core;
	bzng_huff_scratch huff;
} bzng_estate;

/*
 * Internal-error codes for AssertH (libbzip2's compression side used 1001-1007
 * and 3001-3007; these are not part of any public contract).
 */
#define BZNG_ERR_SORT	  5001
#define BZNG_ERR_OVERFLOW 5002

/*
 * Bytes of the block buffer for blocks of up to cap bytes after RLE1: the
 * block and its copy for the least rotation, plus vector slack.
 */
static size_t
tt_bytes(s32 cap)
{
	return 2 * (size_t)cap + BZNG_ENC_PAD;
}

/*
 * Bytes of the sort work array: the sorter's guaranteed workspace, which
 * after the sort holds the MTF values and then the compressed block.
 */
static size_t
work_bytes(s32 cap)
{
	return bzng_bwt_work_ints(cap) * sizeof(s32);
}

/* Frees the block arrays of s (either may be NULL). */
static void
free_arrays(bz_stream *strm, bzng_estate *s)
{
	if (s->core.tt != NULL)
		BZFREE(s->core.tt);
	if (s->core.work != NULL)
		BZFREE(s->core.work);
}

/* Starts a new block: libbzip2's prepare_new_block. */
static void
prepare_new_block(bzng_estate *s)
{
	s->rle.nblock = 0;
	s->num_z = 0;
	s->state_out_pos = 0;
	s->rle.crc = 0xffffffffu;
	s->block_no++;
}

/* Clears the pending RLE1 unit: libbzip2's init_RL. */
static void
init_RL(bzng_estate *s)
{
	s->rle.ch = 256;
	s->rle.len = 0;
}

/* Whether no RLE1 unit is pending: libbzip2's isempty_RL. */
static Bool
isempty_RL(bzng_estate *s)
{
	if (s->rle.ch < 256 && s->rle.len > 0)
		return False;
	else
		return True;
}

int
BZ_API(BZ2_bzCompressInit)(bz_stream *strm, int blockSize100k, int verbosity,
			   int workFactor)
{
	bzng_estate *s;
	void *raw;

	if (!bzng_config_ok())
		return BZ_CONFIG_ERROR;

	if (strm == NULL || blockSize100k < 1 || blockSize100k > 9 ||
	    workFactor < 0 || workFactor > 250)
		return BZ_PARAM_ERROR;

	if (strm->bzalloc == NULL)
		strm->bzalloc = bzng_default_bzalloc;
	if (strm->bzfree == NULL)
		strm->bzfree = bzng_default_bzfree;

	raw = BZALLOC(sizeof(bzng_estate) + STATE_ALIGN - 1);
	if (raw == NULL)
		return BZ_MEM_ERROR;
	s = align_state(raw);
	s->raw = raw;
	s->strm = strm;

	s32 cap = 100000 * blockSize100k;
	memset(&s->core, 0, sizeof s->core);
	s->core.cap = cap;
	s->core.tt = BZALLOC((int)tt_bytes(cap));
	s->core.work = BZALLOC((int)work_bytes(cap));
	if (s->core.tt == NULL || s->core.work == NULL) {
		free_arrays(strm, s);
		BZFREE(raw);
		return BZ_MEM_ERROR;
	}
	s->core.work_ints = bzng_bwt_work_ints(cap);
	s->core.zbits_off = bzng_zbits_offset(cap);
	s->core.zbits_cap = work_bytes(cap) - s->core.zbits_off;
	AssertH(s->core.zbits_cap >= bzng_block_bound(cap), BZNG_ERR_OVERFLOW);
	s->core.huff = &s->huff;
	s->zbits = (u8 *)s->core.work + s->core.zbits_off;

	s->block_no = 0;
	s->state = BZ_S_INPUT;
	s->mode = BZ_M_RUNNING;
	s->combined_crc = 0;
	s->block_size_100k = blockSize100k;
	s->nblock_max = 100000 * blockSize100k - 19;
	s->verbosity = verbosity;
	s->avail_in_expect = 0;
	s->carry = 0;
	s->carry_n = 0;

	strm->state = s;
	strm->total_in_lo32 = 0;
	strm->total_in_hi32 = 0;
	strm->total_out_lo32 = 0;
	strm->total_out_hi32 = 0;
	init_RL(s);
	prepare_new_block(s);
	return BZ_OK;
}

/* Writes the pending RLE1 unit to the block: libbzip2's flush_RL. */
static void
flush_RL(bzng_estate *s)
{
	bzng_rle_state *st = &s->rle;
	if (st->ch < 256) {
		st->crc = bzng_crc_repeat(st->crc, (u8)st->ch, st->len);
		st->nblock = bzng_put_unit(s->core.tt, st->nblock, (u8)st->ch,
					   st->len);
	}
	init_RL(s);
}

/*
 * libbzip2's BZ2_compressBlock: codes the block (if any) behind the stream
 * header (first block) and before the trailer (last block), then releases
 * the bytes libbzip2 would have released. The writer starts with at most 27
 * carried bits plus the 32-bit stream header, so nothing reaches the staging
 * area (which the sorter uses as scratch) before the block has been sorted;
 * the first flush happens inside bzng_block_encode, after the sort.
 */
static void
compress_block(bzng_estate *s, Bool is_last_block)
{
	u8 *z = s->zbits;
	bzng_bitwriter w = {.acc = s->carry,
			    .nbits = s->carry_n,
			    .out = z,
			    .end = z + s->core.zbits_cap - BZNG_BW_SLACK};
	u32 last_width = 0, block_crc = 0;

	if (s->rle.nblock > 0) {
		block_crc = ~s->rle.crc;
		s->combined_crc =
			(s->combined_crc << 1) | (s->combined_crc >> 31);
		s->combined_crc ^= block_crc;
		if (s->verbosity >= 2)
			VPrintf4("    block %d: crc = 0x%08x, combined CRC = "
				 "0x%08x, size = %d\n",
				 (int)s->block_no, (unsigned)block_crc,
				 (unsigned)s->combined_crc, (int)s->rle.nblock);
	}

	if (s->block_no == 1) {
		bzng_bw_put(&w, 8, BZ_HDR_B);
		bzng_bw_put(&w, 8, BZ_HDR_Z);
		bzng_bw_put(&w, 8, BZ_HDR_h);
		bzng_bw_put(&w, 8, (u32)(BZ_HDR_0 + s->block_size_100k));
		last_width = 8;
	}

	if (s->rle.nblock > 0) {
		last_width = bzng_block_encode(&s->core, s->rle.nblock,
					       block_crc, &w);
		AssertH(last_width != 0, BZNG_ERR_SORT);
	}

	if (is_last_block) {
		bzng_bw_put(&w, 24, 0x177245);
		bzng_bw_put(&w, 24, 0x385090);
		bzng_bw_put(&w, 32, s->combined_crc);
		if (s->verbosity >= 2)
			VPrintf1("    final combined CRC = 0x%08x\n   ",
				 (unsigned)s->combined_crc);
		if (w.nbits & 7)
			bzng_bw_put(&w, 8 - (w.nbits & 7), 0);
		bzng_bw_flush(&w);
		s->num_z = (s32)(w.out - z);
		s->carry = 0;
		s->carry_n = 0;
	} else if (last_width == 0) {
		/* Nothing written (an empty block after the first): nothing
		 * released. */
		s->num_z = 0;
	} else {
		bzng_bw_flush(&w);
		u64 total = (u64)(w.out - z) * 8 + w.nbits;
		size_t release = (size_t)((total - last_width) / 8);
		u64 c = 0;
		for (const u8 *p = z + release; p < w.out; p++)
			c = (c << 8) | *p;
		c = (c << w.nbits) | (w.acc & ((1u << w.nbits) - 1));
		s->carry = c;
		s->carry_n = (u32)(total - (u64)release * 8);
		s->num_z = (s32)release;
	}
	AssertH(!w.overflow, BZNG_ERR_OVERFLOW);
}

/* The input stage's kernel, chosen on first use (kern.h). */
static size_t dispatch_rle_fill(struct bzng_rle_state *st, const u8 *in,
				size_t avail, u8 *blk, s32 nblock_max);

static volatile rle_fill_func_t rle_fill_impl = dispatch_rle_fill;

/* Choose the best implementation at runtime. */
static size_t
dispatch_rle_fill(struct bzng_rle_state *st, const u8 *in, size_t avail,
		  u8 *blk, s32 nblock_max)
{
	rle_fill_func_t f = arch_select_rle_fill();

	if (f == NULL)
		f = bzng_rle_fill_scalar;

	rle_fill_impl = f;
	return f(st, in, avail, blk, nblock_max);
}

/*
 * Feeds input to the block until it runs out, the block is full, or (when
 * flushing or finishing) the bytes that were present at the flush/finish
 * request are used up.
 */
static Bool
copy_input_until_stop(bzng_estate *s)
{
	bz_stream *strm = s->strm;
	if (s->rle.nblock >= s->nblock_max)
		return False;
	size_t avail = strm->avail_in;
	if (s->mode != BZ_M_RUNNING && s->avail_in_expect < avail)
		avail = s->avail_in_expect;
	size_t used = rle_fill_impl(&s->rle, (const u8 *)strm->next_in, avail,
				    s->core.tt, s->nblock_max);
	if (used == 0)
		return False;
	strm->next_in += used;
	strm->avail_in -= (unsigned int)used;
	u64 total =
		((u64)strm->total_in_hi32 << 32 | strm->total_in_lo32) + used;
	strm->total_in_lo32 = (unsigned int)total;
	strm->total_in_hi32 = (unsigned int)(total >> 32);
	if (s->mode != BZ_M_RUNNING)
		s->avail_in_expect -= (u32)used;
	return True;
}

/*
 * Copies released compressed bytes to the caller's buffer: libbzip2's
 * copy_output_until_stop. Returns whether any byte moved.
 */
static Bool
copy_output_until_stop(bzng_estate *s)
{
	bz_stream *strm = s->strm;
	size_t n = (size_t)(s->num_z - s->state_out_pos);
	if (strm->avail_out < n)
		n = strm->avail_out;
	if (n == 0)
		return False;
	memcpy(strm->next_out, s->zbits + s->state_out_pos, n);
	s->state_out_pos += (s32)n;
	strm->avail_out -= (unsigned int)n;
	strm->next_out += n;
	u64 total =
		((u64)strm->total_out_hi32 << 32 | strm->total_out_lo32) + n;
	strm->total_out_lo32 = (unsigned int)total;
	strm->total_out_hi32 = (unsigned int)(total >> 32);
	return True;
}

/*
 * libbzip2's handle_compress: alternates between filling the block and
 * draining its output until neither can progress. Returns whether any input
 * was consumed or output produced.
 */
static Bool
handle_compress(bz_stream *strm)
{
	Bool progress_in = False;
	Bool progress_out = False;
	bzng_estate *s = strm->state;

	while (True) {
		if (s->state == BZ_S_OUTPUT) {
			progress_out |= copy_output_until_stop(s);
			if (s->state_out_pos < s->num_z)
				break;
			if (s->mode == BZ_M_FINISHING &&
			    s->avail_in_expect == 0 && isempty_RL(s))
				break;
			prepare_new_block(s);
			s->state = BZ_S_INPUT;
			if (s->mode == BZ_M_FLUSHING &&
			    s->avail_in_expect == 0 && isempty_RL(s))
				break;
		}

		if (s->state == BZ_S_INPUT) {
			progress_in |= copy_input_until_stop(s);
			if (s->mode != BZ_M_RUNNING &&
			    s->avail_in_expect == 0) {
				flush_RL(s);
				compress_block(
					s, (Bool)(s->mode == BZ_M_FINISHING));
				s->state = BZ_S_OUTPUT;
			} else if (s->rle.nblock >= s->nblock_max) {
				compress_block(s, False);
				s->state = BZ_S_OUTPUT;
			} else if (s->strm->avail_in == 0) {
				break;
			}
		}
	}

	return progress_in || progress_out;
}

/*
 * libbz2's BZ2_bzCompress: one call of the compression state machine with
 * the given action.  Return codes and stream fields follow libbz2 1.0.8
 * exactly.
 */
int
BZ_API(BZ2_bzCompress)(bz_stream *strm, int action)
{
	Bool progress;
	bzng_estate *s;
	if (strm == NULL)
		return BZ_PARAM_ERROR;
	s = strm->state;
	if (s == NULL)
		return BZ_PARAM_ERROR;
	if (s->strm != strm)
		return BZ_PARAM_ERROR;

preswitch:
	switch (s->mode) {
	case BZ_M_IDLE:
		return BZ_SEQUENCE_ERROR;

	case BZ_M_RUNNING:
		if (action == BZ_RUN) {
			progress = handle_compress(strm);
			return progress ? BZ_RUN_OK : BZ_PARAM_ERROR;
		} else if (action == BZ_FLUSH) {
			s->avail_in_expect = strm->avail_in;
			s->mode = BZ_M_FLUSHING;
			goto preswitch;
		} else if (action == BZ_FINISH) {
			s->avail_in_expect = strm->avail_in;
			s->mode = BZ_M_FINISHING;
			goto preswitch;
		} else
			return BZ_PARAM_ERROR;

	case BZ_M_FLUSHING:
		if (action != BZ_FLUSH)
			return BZ_SEQUENCE_ERROR;
		if (s->avail_in_expect != s->strm->avail_in)
			return BZ_SEQUENCE_ERROR;
		progress = handle_compress(strm);
		if (s->avail_in_expect > 0 || !isempty_RL(s) ||
		    s->state_out_pos < s->num_z)
			return BZ_FLUSH_OK;
		s->mode = BZ_M_RUNNING;
		return BZ_RUN_OK;

	case BZ_M_FINISHING:
		if (action != BZ_FINISH)
			return BZ_SEQUENCE_ERROR;
		if (s->avail_in_expect != s->strm->avail_in)
			return BZ_SEQUENCE_ERROR;
		progress = handle_compress(strm);
		if (!progress)
			return BZ_SEQUENCE_ERROR;
		if (s->avail_in_expect > 0 || !isempty_RL(s) ||
		    s->state_out_pos < s->num_z)
			return BZ_FINISH_OK;
		s->mode = BZ_M_IDLE;
		return BZ_STREAM_END;
	}
	return BZ_OK; /*--not reached--*/
}

/* libbz2's BZ2_bzCompressEnd: frees everything BZ2_bzCompressInit allocated. */
int
BZ_API(BZ2_bzCompressEnd)(bz_stream *strm)
{
	bzng_estate *s;
	if (strm == NULL)
		return BZ_PARAM_ERROR;
	s = strm->state;
	if (s == NULL)
		return BZ_PARAM_ERROR;
	if (s->strm != strm)
		return BZ_PARAM_ERROR;

	free_arrays(strm, s);
	BZFREE(s->raw);

	strm->state = NULL;

	return BZ_OK;
}
