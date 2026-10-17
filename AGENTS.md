# Guidance for agents working in this repository

bz2-ng is a drop-in replacement for libbz2 1.0.8 with a faster single-threaded encoder and decoder. `docs/spec.md` is the specification; read it before changing or reviewing anything under `c/`.

## Hard rules

- **libsais is the block sorter, read and used from the vendored copy only.** Its author relicensed it to Apache-2.0 WITH LLVM-exception (libsais commit `1a696e6`, 5 October 2026); the exception removes the conflict that plain Apache-2.0 has with GPLv2-only programs that link a system libbz2. `c/third_party/libsais/` holds `libsais.c`, `libsais.h` and `LICENSE` from that commit, unmodified: keep them so. What is compiled is that copy with the patches in `c/third_party/libsais/patches/` applied, which CMake does at configure time in the build directory and checks against `patches/SHA256`; a change to libsais is a new patch there, with its measurement in its header and the new sums, and the compiled file names its patches in its first lines (`docs/spec.md` §4.3). Releases up to 2.10.4 carry plain Apache-2.0: take nothing from those.
- **Nothing is allocated during a compress call**: libbz2's API has no out-of-memory return there. `c/lib/sort_libsais.c` compiles libsais so that it cannot reach `malloc`, and hands it enough of the block's work array that it never needs to (`docs/spec.md` §4.3). A change to the work array's size, to the sorter's call or to the libsais patches must keep that bound.
- **No GPL or LGPL code** is read for code or copied (lbzip2, 7-Zip's bzip2 encoder, sdsl-lite and similar). MIT, BSD, zlib-licensed and public-domain code may be reused with its notice kept and altered files marked.
- **Output is byte-identical to libbz2's** and the legacy API matches libbz2 call for call (`docs/spec.md` §2). A change that alters either is a bug unless the spec lists it as a divergence.
- **Single-threaded.** No parallel encoder or decoder modes.

## The C library (`c/`)

- **Style and structure follow libdeflate** (`c/.clang-format`; `docs/spec.md` §4.8): tabs at width 8, 80 columns, kernel-style braces, `common_defs.h`, CPU feature detection and kernels under `lib/x86/` and `lib/arm/`, self-replacing dispatch pointers, template headers.
- **Files that are libbz2's own code keep upstream formatting**: `c/bzlib.h` and `c/programs/` are upstream bzip2's at commit `f5dfc96` (1.0.8 plus the fixes since, including CVE-2026-42250) and `c/test/reference/` is 1.0.8, all unmodified; `c/lib/bzlib.c` and `bzlib_private.h` are lightly altered. The legacy drivers (`compress.c`, `decompress.c`) deliberately keep libbz2's state machines recognisable.
- **Kernel tiers are chosen at run time** and every tier must produce the same bytes. `BZ2NG_TIER` exists for tests, and on AArch64 `BZ2NG_NO_PMULL`.
- **Correctness evidence is the differential testers**, which drive stock libbz2 and bz2-ng with identical calls. Build and test: `cmake -S c -B c/build && cmake --build c/build && ctest --test-dir c/build`.
- Every optimisation needs a benchmark it must not regress. Timings are single-threaded and compared with libbz2 built by the same compiler. Time a change in rounds interleaved with its baseline and with a second build of the baseline as a control: code placement alone moves a cell by 1–3%. Where a change gains on one compiler and machine and loses on another, clang on AArch64 and gcc on x86-64 count most: a small loss with gcc on AArch64 or clang on x86-64 is acceptable for a gain there (`docs/spec.md` §3).

## Writing

- Comments say why, describe the code as it is, and carry no history.
- Markdown prose is not hard-wrapped. British English.
- Tests generate their data in code; the corpora under `data/` are for benchmarks and bulk checks.

## Repository

- CI runs on hosted Linux and Windows runners, never macOS: on every pull request and each push to one, on merges to `main`, and on demand (`gh workflow run ci.yml --ref <branch> -f jobs=<group>`). A change that touches neither `c/` nor the workflow gets no run.
