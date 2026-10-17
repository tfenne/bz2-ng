/*
 * crc_test.c - the CRC-32/BZIP2 tables and kernels
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 */

/*
 * Recomputes every entry of bzng_crc32_table from the polynomial, then checks
 * every CRC kernel this CPU can run, and the dispatched bzng_crc32, against a
 * bitwise reference: every length 0..2100 at 16 alignments from the initial
 * register, from other registers (0, arbitrary values), and continued across
 * arbitrary chunk splits, as the library uses them (a block's CRC is carried
 * from call to call).  On AArch64 it also checks which kernel the dispatch
 * selects for the CPU's features.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"
#include "test_tier.h"
#if BZNG_X86_TIERS
#  include "x86/cpu_features.h"
#elif defined(ARCH_ARM64) && CPU_IS_LITTLE_ENDIAN()
#  include "arm/crc32_impl.h"
#endif

static int failures;

/* CRC register after p[0..len), one bit at a time. */
static u32
crc_bitwise(u32 crc, const u8 *p, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		crc ^= (u32)p[i] << 24;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04c11db7 :
						   crc << 1;
	}
	return crc;
}

/* Every table entry: byte b followed by k zero bytes from a zero register. */
static void
check_tables(void)
{
	for (int k = 0; k < 16; k++) {
		for (int b = 0; b < 256; b++) {
			u8 msg[16] = {0};

			msg[0] = (u8)b;
			if (crc_bitwise(0, msg, (size_t)k + 1) !=
			    bzng_crc32_table[k][b]) {
				printf("FAIL table[%d][%d]\n", k, b);
				failures++;
			}
		}
	}
}

/* Kernel f against the bitwise reference on every length and alignment. */
static void
check_kernel(const char *name, crc32_func_t f)
{
	static u8 buf[2100 + 16];
	u32 x = 0x12345678;

	for (size_t i = 0; i < sizeof(buf); i++) {
		x = x * 1103515245 + 12345;
		buf[i] = (u8)(x >> 16);
	}
	for (size_t align = 0; align < 16; align++) {
		for (size_t len = 0; len + align <= 2100 + 16 && len <= 2100;
		     len++) {
			u32 want = crc_bitwise(0xffffffff, buf + align, len);
			u32 got = f(0xffffffff, buf + align, len);

			if (want != got) {
				printf("FAIL %s len %zu align %zu\n", name, len,
				       align);
				failures++;
				return;
			}
		}
	}
	/* Other starting registers. */
	static const u32 regs[] = {0, 1, 0x80000000, 0x12345678, 0xdeadbeef};
	for (size_t r = 0; r < sizeof(regs) / sizeof(regs[0]); r++) {
		for (size_t len = 0; len <= 2100; len += 1 + len / 16) {
			const size_t align = (len * 7 + r) % 16;
			u32 want = crc_bitwise(regs[r], buf + align, len);
			u32 got = f(regs[r], buf + align, len);

			if (want != got) {
				printf("FAIL %s from register %08x len %zu\n",
				       name, (unsigned)regs[r], len);
				failures++;
				return;
			}
		}
	}
	/*
	 * One message from a random register, continued across random chunk
	 * splits (so every chunk but the first starts at an arbitrary
	 * alignment and register).
	 */
	for (int trial = 0; trial < 2000; trial++) {
		x = x * 1103515245 + 12345;
		const size_t start = (x >> 8) % 16;
		const size_t total = (x >> 12) % 2100;
		const u32 init = x * 2654435761u;
		const u32 want = crc_bitwise(init, buf + start, total);
		u32 got = init;
		size_t done = 0;

		while (done < total) {
			x = x * 1103515245 + 12345;
			size_t part = (x >> 16) % 300;
			if (part > total - done)
				part = total - done;
			got = f(got, buf + start + done, part);
			done += part;
		}
		if (want != got) {
			printf("FAIL %s split message len %zu\n", name, total);
			failures++;
			return;
		}
	}
	printf("crc kernel %s ok\n", name);
}

#if defined(ARCH_ARM64) && CPU_IS_LITTLE_ENDIAN()
/*
 * The dispatch's choice: for the NEON tier the PMULL kernel on a CPU with
 * PMULL, the CRC32-instruction kernel on one without (as BZ2NG_NO_PMULL
 * makes this one appear); the table kernel for the scalar tier.
 */
static void
check_arm_selection(void)
{
	const u32 features = get_arm_cpu_features();
	const char *no_pmull = getenv("BZ2NG_NO_PMULL");
	const crc32_func_t got = arch_select_crc32_func();
	crc32_func_t want = NULL;

	if (no_pmull != NULL && *no_pmull != '\0' && HAVE_PMULL(features)) {
		printf("FAIL BZ2NG_NO_PMULL left PMULL in the features\n");
		failures++;
	}
	if (arch_select_tier() == BZNG_TIER_NEON)
		want = HAVE_PMULL(features) ? bzng_crc32_pmull :
					      bzng_crc32_armv8;
	if (got != want) {
		printf("FAIL the dispatch selected the wrong CRC kernel\n");
		failures++;
		return;
	}
	printf("crc selection %s ok\n", want == bzng_crc32_pmull ? "pmull" :
					want == bzng_crc32_armv8 ? "armv8" :
								   "slice16");
}
#endif

int
main(void)
{
	require_requested_tier();
	check_tables();
	check_kernel("slice16", bzng_crc32_slice16);
	check_kernel("dispatched", bzng_crc32);
#if defined(ARCH_ARM64) && CPU_IS_LITTLE_ENDIAN()
	if (HAVE_CRC32(get_arm_cpu_features()))
		check_kernel("armv8", bzng_crc32_armv8);
	if (HAVE_CRC32(get_arm_cpu_features()) &&
	    HAVE_PMULL(get_arm_cpu_features()))
		check_kernel("pmull", bzng_crc32_pmull);
	check_arm_selection();
#elif BZNG_X86_TIERS
	if (HAVE_PCLMULQDQ(get_x86_cpu_features()))
		check_kernel("pclmul", bzng_crc32_pclmul);
	if (HAVE_X86_V3(get_x86_cpu_features()) &&
	    HAVE_VPCLMULQDQ(get_x86_cpu_features()))
		check_kernel("vpclmul", bzng_crc32_vpclmul);
#endif
	printf("crc_test: %s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
