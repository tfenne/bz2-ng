/*
 * enc.h - encoder internals
 *
 * Encoder internals shared by the legacy driver (compress.c), the block core
 * (enc_block.c, enc_sort.c, enc_huff.c) and the kernel tiers (kern_template.h).
 *
 * The block core works on one completed RLE1 block in caller-owned scratch
 * and knows nothing about bz_stream.
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */
#ifndef LIB_ENC_H
#define LIB_ENC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"
#include "kern.h"
#include "lib_common.h"

/* libbz2 1.0.8 limits encoder code lengths to 17 bits (compress.c:450). */
#define BZNG_MAX_CODE_LEN 17
/*
 * Bytes of slack after the block buffer, so vector loads and stores may run
 * past the logical end.
 */
#define BZNG_ENC_PAD 64

/* ---- 64-bit MSB-first bit writer ----------------------------------------- */

/*
 * Right-aligned accumulator: the low nbits bits of acc are pending output.
 * bzng_bw_flush writes all whole bytes with one unaligned 8-byte store, so
 * the output needs BZNG_BW_SLACK bytes of slack past the last real byte; a
 * write that would start beyond end sets overflow instead.
 */
#define BZNG_BW_SLACK 8

/*
 * Room the fast emitter of coded MTF values (enc_huff.c) requires before
 * each group of BZ_G_SIZE symbols, which it writes without further checks:
 * 50 codes of at most BZNG_MAX_CODE_LEN bits are at most 107 bytes.
 */
#define BZNG_GROUP_GUARD 112

typedef struct {
	u64 acc;
	u32 nbits;
	u8 *out;
	u8 *end; /* last position a flush may start at (buffer end minus
		    BZNG_BW_SLACK) */
	int overflow;
} bzng_bitwriter;

/* Writes the whole pending bytes of b. */
static inline void
bzng_bw_flush(bzng_bitwriter *b)
{
	if (b->nbits == 0)
		return;
	put_unaligned_be64(b->acc << (64 - b->nbits), b->out);
	b->out += b->nbits >> 3;
	b->nbits &= 7;
}

/*
 * Appends the n <= 32 low bits of v to b; flushes when the accumulator could
 * overflow.
 */
static inline void
bzng_bw_put(bzng_bitwriter *b, u32 n, u64 v)
{
	if (b->nbits + n > 63) {
		if (unlikely(b->out > b->end)) {
			b->overflow = 1;
			b->out = b->end;
		}
		bzng_bw_flush(b);
	}
	b->acc = (b->acc << n) | v;
	b->nbits += n;
}

/* ---- block core scratch -------------------------------------------------- */

/* The Huffman stage's tables and selectors. */
typedef struct {
	u8 len[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE];
	s32 rfreq[2][BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE];
	u64 pk[BZ_MAX_ALPHA_SIZE];
	u32 ce[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE];
	/*
	 * The code-length builder's arrays: leaf weights by table (index 1 to
	 * the alphabet size), and a heap and the tree's parent links for each
	 * of the tables built side by side.
	 */
	u32 leaf_weight[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE + 1];
	u64 heap[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE + 2];
	u16 parent[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE * 2];
	u8 selector[BZ_MAX_SELECTORS];
	u8 selector_mtf[BZ_MAX_SELECTORS];
	u8 prev_sel[BZ_MAX_SELECTORS];
} bzng_huff_scratch;

/*
 * Everything one block encode needs, owned by the caller. Sizes for a block
 * capacity cap (bytes after RLE1):
 * tt:   2*cap + BZNG_ENC_PAD bytes: the block in tt[0..n), its copy in
 *       tt[n..2n) for the least rotation; the BWT is written over the rotated
 *       block inside tt.
 * work: bzng_bwt_work_ints(cap) ints: the least rotation's candidate list,
 *       then the sorter's work array; after the sort the MTF values (uint16)
 *       from its start and the compressed block from byte offset zbits_off.
 * huff: the Huffman stage's tables and selectors.
 */
typedef struct {
	s32 cap; /* largest block (bytes after RLE1) the arrays hold */
	u8 *tt;
	s32 *work;
	size_t work_ints;
	size_t zbits_off; /* byte offset of the compressed-block area inside
			     work */
	size_t zbits_cap; /* its size in bytes, >= bzng_block_bound(cap) */
	bzng_huff_scratch *huff;
} bzng_enc_core;

/*
 * Upper bound on the bytes one block of up to cap RLE1 bytes can occupy in
 * the compressed stream, including the stream header and trailer, a carry
 * of up to 4 bytes from the previous block and the bit writer's slack.
 */
size_t bzng_block_bound(s32 cap);

/*
 * Byte offset in the work array at which the compressed block is staged:
 * behind the MTF values of a block of up to cap bytes.
 */
size_t bzng_zbits_offset(s32 cap);

/*
 * Encodes the block core->tt[0..nblock) (1 <= nblock <= cap) with its
 * finalised CRC: block magic, CRC, randomised bit, origPtr, symbol map,
 * selectors, code lengths and coded MTF values, appended to w. Returns the
 * width in bits of the last code written (the end-of-block code), which the
 * streaming driver needs to reproduce libbz2's byte release, or 0 if the
 * sorter failed (an internal error).
 */
u32 bzng_block_encode(bzng_enc_core *core, s32 nblock, u32 block_crc,
		      bzng_bitwriter *w);

/*
 * Sorts the block core->tt[0..n) (filling the copy in tt[n..2n)): writes
 * its BWT to *L_out (inside tt) and its byte frequencies to freq, and returns
 * origPtr, or -1 if the sorter failed. For a block u^m (m > 1) any of the m
 * tied rows is a valid origPtr; this returns the last.
 */
s32 bzng_sort_block(bzng_enc_core *core, s32 n, s32 *freq, u8 **L_out);

/*
 * Table selection, code lengths, and emission of selectors, code-length
 * tables and the coded MTF values mtfv[0..nmtf) with alphabet alpha_size.
 * Returns the width of the last code written.
 */
u32 bzng_huff_encode(bzng_huff_scratch *h, const u16 *mtfv, s32 nmtf,
		     const s32 *mtf_freq, s32 alpha_size, bzng_bitwriter *bw);

/* ---- input stage --------------------------------------------------------- */

/*
 * libbz2's RLE1 with its exact block-boundary behaviour: input is cut into
 * units (maximal runs of one byte, cut at 255); a unit of 1-3 bytes is
 * stored as is, a longer one as 4 bytes plus a count byte. A unit is written
 * to the block (and its bytes added to the block CRC) only when the byte
 * after it arrives; the block is full once a written unit brings it to
 * nblock_max or more, and the byte that triggered that write is consumed and
 * stays pending.
 */
typedef struct bzng_rle_state {
	u32 ch;	    /* byte of the pending unit, 256 if none */
	s32 len;    /* its length so far (0 if none) */
	s32 nblock; /* bytes in the block */
	u32 crc;    /* running (non-finalised) CRC of the units written */
} bzng_rle_state;

/* CRC register after len copies of byte c. */
static inline u32
bzng_crc_repeat(u32 crc, u8 c, s32 len)
{
	for (s32 i = 0; i < len; i++)
		crc = crc32_byte(crc, c);
	return crc;
}

/*
 * Writes one unit of len (1..255) copies of c at blk[nb] with one 8-byte
 * store (needs 8 bytes of room); returns the new block length.  The unit is
 * built with its first byte lowest, and v holds it in memory order.
 */
static inline s32
bzng_put_unit(u8 *blk, s32 nb, u8 c, s32 len)
{
	u64 x = len >= 4 ? (u64)(len - 4) : c;
	u64 v = le64_bswap((0x0000000001010101ULL * c) | (x << 32));
	memcpy(blk + nb, &v, 8);
	return nb + (len >= 4 ? 5 : len);
}

#endif /* LIB_ENC_H */
