#!/bin/sh
# Formats bz2-ng's own C sources with c/.clang-format (house style: libdeflate's).
# libbz2's own files (bzlib.h, lib/bzlib.c, lib/bzlib_private.h,
# lib/randtable.c, programs/, test/reference/, test/rand_hook.c) and the
# files taken from libdeflate (lib/common_defs.h, lib/cpu_features_common.h,
# lib/*/cpu_features.[ch]) keep their upstream layout and are not listed.
# usage: scripts/format.sh [clang-format binary]
set -eu
cd "$(dirname "$0")/.."
CF=${1:-clang-format}
# Other major versions lay some constructs out differently, which would show
# up as changes unrelated to the edit being formatted.
WANT=21
have=$("$CF" --version | sed -E 's/.*version ([0-9]+).*/\1/')
if [ "$have" != "$WANT" ]; then
    echo "format.sh: needs clang-format $WANT, found ${have:-none}" >&2
    exit 1
fi
"$CF" -i \
    lib/compress.c lib/decompress.c lib/dec.h lib/dec_block.c \
    lib/enc.h lib/enc_block.c lib/enc_huff.c lib/enc_sort.c \
    lib/sort.h lib/sort_libsais.c lib/sort_recursion.h \
    lib/kern.h lib/kern_template.h lib/kern_scalar.c lib/vec.h \
    lib/crc32.c lib/crc32.h lib/crc32_table.c lib/lib_common.h lib/utils.c \
    lib/arm/kern_impl.h lib/arm/kern_neon.c lib/arm/crc32_impl.h lib/arm/crc32_armv8.c \
    lib/arm/crc32_pmull.c \
    lib/x86/kern_impl.h lib/x86/kern_sse4.c lib/x86/kern_avx2.c lib/x86/crc32_impl.h \
    lib/x86/crc32_clmul.h lib/x86/crc32_pclmul.c lib/x86/crc32_vpclmul.c \
    test/enc_difftest.c test/dec_difftest.c test/memtest.c test/sort_test.c \
    test/crc_test.c test/layout_test.c test/tier_identity.c test/cli_tool.c test/dll_imports.c \
    test/huff_limit_test.c test/dec_bigbuf.c test/sort_libsais_trap.c test/rec_choice_test.c \
    test/no_stdio_test.c test/test_tier.h test/reference_api.h test/sort_oracle.h test/compat.h \
    fuzz/fuzz_common.h fuzz/fuzz_compress.c fuzz/fuzz_decompress.c fuzz/fuzz_sort.c \
    fuzz/gen_seeds.c fuzz/ref_state.c fuzz/ref_state.h \
    bench/bench.c
