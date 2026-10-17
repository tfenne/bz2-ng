# Porting note: packaging bz2-ng in place of libbz2 1.0.8

For packagers and integrators replacing libbz2 with bz2-ng. The full contract is `docs/spec.md` §2; this note is the short, practical version, and it lists everything a package built from bz2-ng does differently from one built from bzip2 1.0.8.

## What a package gets

- **The library**: `libbz2` as a shared and a static library, exporting exactly the 24 public functions of libbz2's `libbz2.def` and nothing else. The public header is upstream bzip2's `bzlib.h`, unmodified (1.0.8's plus its one later fix, the `#include`s moved outside `extern "C"`), so `libbz2-dev`-style packages change nothing but the library. The stdio layer carries upstream's two fixes since 1.0.8 (the compressor state leaked when a write was abandoned after an I/O error; an `isdigit` cast).
- **The programs**: `bzip2` and `bzip2recover` built from upstream bzip2's current sources (commit `f5dfc96`: 1.0.8 plus the fixes since, among them CVE-2026-42250's block-count check and `O_EXCL` output files in `bzip2recover`, the fixes Debian and Ubuntu carry as patches), with the `bunzip2` and `bzcat` links (copies with mingw-w64) and `bzip2.1` with its `bunzip2.1`, `bzcat.1` and `bzip2recover.1` links.
- **`bzip2.pc`** for pkg-config, reporting version 1.0.8 as the API version.
- The installed file names match the platform's existing bzip2 package, read from the packages themselves: `libbz2.so.1.0.8` with the `libbz2.so.1.0` SONAME (Debian, Ubuntu, upstream; note that Debian and Ubuntu name their own file `libbz2.so.1.0.4` and point both links at it, so a package replacing theirs installs under that name or updates the links) or `libbz2.so.1` (`-DBZ2NG_SONAME=1`; Fedora and Alpine); `libbz2.1.0.8.dylib` under the install name `libbz2.1.0.dylib` on macOS; msys2's `libbz2-1.dll` with `libbz2.dll.a` for mingw-w64; vcpkg's `bz2.dll` with `bz2.lib` for MSVC. `c/test/install_layout.txt` lists each platform's full layout, and the `install_layout` test checks an install against it.
- Not shipped: `bzdiff`, `bzgrep` and `bzmore`, whose headers say they were adapted from gzip's GPL scripts; keep your distribution's.

Programs linked against libbz2 need no relinking: the SONAME is the same, the symbols are the same, and `BZ2_bzlibVersion()` returns `"1.0.8, bz2-ng <version>"`, which begins with the string libbz2 1.0.8 returns, for callers that parse a leading version.

## Building

CMake 3.16+ and a C11 compiler (gcc 9+, clang 10+, Apple clang, MSVC). `cmake -S c -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build && cmake --install build`. The options are in the README; the ones a package is likely to set are `BZ2NG_SONAME` and, for a build that must stay inside libbz2's documented decompression memory, `BZ2NG_DECODER_IN_FORMULA=ON` (see below).

If you compile the sources with your own build system rather than CMake, `c/CMakeLists.txt` is the reference. In particular:

- `c/third_party/libsais/libsais.c` is compiled as part of `c/lib/sort_libsais.c`, which includes it, and not on its own.
- Before compiling `c/lib/sort_libsais.c`, apply `c/third_party/libsais/patches/*.patch` in name order with `patch -p1` from `c/third_party/libsais/` (or a copy of it), check the result there with `sha256sum -c patches/SHA256` (`shasum -a 256 -c` on macOS), and put that directory on `sort_libsais.c`'s include path, from which it includes `libsais.c`.
- `c/lib/sort_libsais.c` must be compiled with `-fno-sched-spec` under gcc on AArch64: gcc's scheduler otherwise hoists a prefetch that costs a third of the sorter's speed on Arm cores (`docs/spec.md` §4.3).
- The x86 kernel files need their instruction-set flags: `-march=x86-64-v2` for `kern_sse4.c`, `-march=x86-64-v3` for `kern_avx2.c`, `-msse4.1 -mpclmul` and `-mavx2 -mpclmul -mvpclmulqdq` for the CRC kernels. gcc before 11 and clang before 12 have no level names and take the instruction sets they stand for one by one, as `c/CMakeLists.txt` lists them. A kernel file compiled without its flags fails to compile rather than build portable code under the tier's name, and no other file should have them, since the library chooses the kernels at run time by what the CPU has.
- The AArch64 CRC files need `-march=armv8-a+crc` (`crc32_armv8.c`) and `-march=armv8-a+crc+crypto` (`crc32_pmull.c`).
- The shared library must be linked with the export list (a version script, an exported-symbols list or a `.def` file) so that only the 24 functions are visible.
- On 32-bit x86 Windows with mingw-w64, the import library must give programs the functions' stdcall names (`BZ2_bzRead@16`), which `ld` from binutils 2.40 leaves out; CMake writes it with `dlltool`.

The test suite builds a reference libbz2 1.0.8 from the unmodified sources in `c/test/reference/` and drives both libraries with identical call sequences; it needs no network and generates its own data. Expect it to take a few minutes.

## What differs from libbz2 1.0.8

Everything below is deliberate and documented in `docs/spec.md` §2.5 with its evidence. Nothing else differs: every return code, every stream field, every byte of output and every byte of the caller's buffers match libbz2 1.0.8 for any sequence of calls a correct program can make.

| Behaviour | libbz2 1.0.8 | bz2-ng | What a packager should know |
|---|---|---|---|
| Decompression memory | 100 kB + 4 × block size (documented) | 100 kB + 5 × block size by default: about 900 kB more at level 9 | The default layout decodes full-size blocks 1–7% faster, depending on the machine. If a dependant sizes memory pools by libbz2's documented formula, build with `BZ2NG_DECODER_IN_FORMULA=ON`, which stays inside it at every level. Compression stays inside 400 kB + 8 × block size at every level, and `small` mode inside 100 kB + 2.5 × block size. |
| `bzalloc` request sizes | compression: four requests, the largest 3.6 MB at level 9; decompression: the state and one table | fewer and larger requests; the in-formula decoder layout still uses 14–24 kB more in total than libbz2 actually does | Only a caller whose pool is carved to libbz2's exact request sizes could notice. The documented formulas are the contract and are honoured. |
| A decompression call after an error return | resumes its state machine | returns the same error again | The manual permits only `BZ2_bzDecompressEnd` after an error; libbz2's resumption reaches undefined behaviour in libbz2 itself. One case is unchanged: after a combined-CRC mismatch at the end of a stream, the next call returns `BZ_SEQUENCE_ERROR`, as in libbz2. |
| `origPtr` of exactly periodic blocks | depends on which internal sorter ran | always the last tied row | A block that is a string repeated a whole number of times (for example all zeros) has several valid BWT origins. Such streams differ from libbz2's in 24 bits and decode identically. No file in any benchmark corpus is affected; one 600-byte test input in HTSlib's suite is, and a contributor's scan of 38,923 mixed real files of up to 64 MiB (issue #9) found 11 affected at level 1 and 2 at level 9 — about 1 file in 3,500 at level 1, in textures, disk images and archives with long constant runs. Only something that compares compressed bytes across implementations — a reproducible-build check, an archive checksum — can notice. |
| Diagnostics at `verbosity` ≥ 2 | describe libbz2's sorter | not printed | That sorter no longer exists; everything else printed at every verbosity matches. |
| Exported private symbols (`BZ2_blockSort`, `BZ2_decompress`, `BZ2_crc32Table`, `BZ2_hbCreateDecodeTables`, …) | leak from the shared library | not exported | They were never part of the API. A scan of every package that links libbz2 in Ubuntu 24.04 (474 binaries) and Fedora 44 (609), and an independent scan of Alpine 3.23 (108 packages, 2,621 ELF files; issue #13), found one that references them: Ubuntu's `unalz`, which bundles a modified libbz2 `decompress.c` linked against the leaked internals and fails to start under bz2-ng. A distribution that switches to bz2-ng needs to rebuild `unalz` with its own bzip2 code; nothing else in those three package sets is affected. |

## Platforms

Linux and macOS on x86-64 and AArch64 are the reference platforms, with gcc, clang and Apple clang; Windows x86-64 with MSVC and mingw-w64 is built and tested on CI; 32-bit (i386, armv7, mips) and big-endian (s390x, mips) targets pass the suite under emulation and produce the same bytes. Big-endian AArch64 and any platform without a vector tier run the scalar tier. Windows on ARM64 is not supported.

Kernel tiers (scalar; SSE4.1 and AVX2 on x86-64; NEON on AArch64) are chosen at run time from the CPU's features, so one build serves every CPU of its architecture and the output never depends on the CPU. The environment variables `BZ2NG_TIER` and, on AArch64, `BZ2NG_NO_PMULL` force a tier or remove the PMULL CRC kernel; they exist for tests and measurement and have no effect on output.

## Licences

bz2-ng's own code is MIT. The files derived from bzip2 1.0.8 keep its BSD-style licence and are marked as altered where they are; the CPU-feature detection comes from libdeflate (MIT); the block sorter is libsais under Apache-2.0 WITH LLVM-exception, taken from the commit at which it was relicensed so that GPLv2-only programs that link libbz2 can link bz2-ng. `THIRD_PARTY.md` lists each component and `c/licenses/` holds the texts; a package's copyright file should carry all four.
