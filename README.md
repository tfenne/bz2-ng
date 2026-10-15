# bz2-ng

A drop-in replacement for libbz2 1.0.8 (the bzip2 library) with a much faster single-threaded encoder and decoder. Same header, same 24 exported functions, same behaviour call for call, same bytes out: a program linked against libbz2 keeps working, and keeps producing the same files, when the shared library is swapped for this one. It compresses about 2.7–10 times faster than libbz2 and decompresses about 1.6–4.9 times faster, depending on the data, the machine and the compiler, at every level from 1 to 9.

## What it is

bzip2 is a block-sorting compressor: the Burrows–Wheeler transform of each block, move-to-front coding, run-length coding of the zeros and Huffman coding. libbz2 1.0.8 is from 2019 and its encoder and decoder are from the 1990s. bz2-ng keeps the format and the public interface and rewrites what lies behind them:

- **The encoder** builds each block's BWT with [libsais](https://github.com/IlyaGrebnov/libsais), the fastest suffix-array construction library available, over a least-rotation bridge that turns suffix order into libbz2's cyclic-rotation order; the run-length input stage and the CRC work in bulk over each caller chunk, the move-to-front stage scans change positions with vector kernels, and the Huffman stage reproduces libbz2's table selection exactly over a 64-bit bit writer. It emits the same block boundaries and the same codes as libbz2, so its output is byte-identical.
- **The decoder** decodes whole 50-symbol Huffman groups from a 64-bit buffer with the head of the MTF list in a vector register, then replaces libbz2's one-pointer-at-a-time inverse BWT with a segmented table build and a 24-chain walk with software prefetch.
- **Kernels are chosen at run time**: scalar everywhere, SSE4.1 and AVX2 on x86-64, NEON on AArch64, with hardware CRC (PCLMULQDQ, or PMULL and the CRC32 instructions). Every kernel produces the same bytes.

It is plain C11 with no dependencies, single-threaded by design (no block-parallel modes), and the format is unchanged: anything bz2-ng writes, libbz2 reads, and the reverse.

## Speed

Level 9, MB/s of uncompressed data, with the multiple of libbz2 1.0.8 built by the same compiler, measured through the libbz2 API with one stream per call on the corpora in `data/` (the `MC:Z` and `XA:Z` tag values of a public 1000 Genomes sample, and Silesia). Compression / decompression.

| Data | Apple M4 Max, Apple clang | AMD Zen 5, gcc 13 | Intel Granite Rapids, gcc 13 | AWS Graviton4, clang 18 |
|---|---|---|---|---|
| Tag values from a CRAM (`MC:Z`), 640 kB streams | 72 (6.9×) / 480 (4.1×) | 75 (8.8×) / 457 (4.9×) | 61 (8.9×) / 282 (4.3×) | 53 (8.0×) / 240 (3.2×) |
| Tag values from a CRAM (`XA:Z`), 1.65 MB streams | 75 (3.0×) / 296 (4.1×) | 61 (2.9×) / 260 (4.9×) | 50 (2.9×) / 176 (4.3×) | 48 (2.8×) / 152 (3.4×) |
| Silesia corpus, 12 files | 66 (2.9×) / 243 (3.6×) | 54 (2.7×) / 210 (4.2×) | 45 (2.8×) / 148 (4.0×) | 44 (2.8×) / 135 (3.4×) |

bz2-ng is never slower than libbz2 in any measured cell — every file of the benchmark corpus, every level, both directions, both compilers on every machine, and independent streams as small as 16 kB, where the lowest multiple is 1.6× (decompression of 16 kB `MC:Z` streams on Graviton4). The compression ratio is identical to libbz2's, because the output is. For scale, bzip2 at level 9 produces output 20–32% smaller than gzip level 5 from libdeflate, which compresses two to seven times faster than bz2-ng and decompresses five to eleven times faster.

`docs/spec.md` §3 states the requirements and how the numbers are taken: interleaved rounds against libbz2 built by the same compiler, with an identical second build as the control for code-placement noise.

## Status

The library is complete and hardened: it passes its differential testers (which drive stock libbz2 and bz2-ng with identical call sequences and compare every return code, every buffer and every byte of output) on x86-64 and AArch64 with gcc, clang, Apple clang and MSVC, and under emulation on big-endian (s390x) and 32-bit (i386, armv7, mips) targets; bzip2's own `make test` and CPython's `test_bz2` pass against it; a fuzzing campaign of 24 CPU-hours per target on each of two machines found nothing. It builds for Windows with MSVC and mingw-w64 and passes its tests there.

It has been validated as a rebuild-free drop-in on Ubuntu 24.04: installed over the distribution's library and programs, the unmodified test suites of libarchive, HTSlib and samtools, CPython, GNU tar, `file` and the bzip2 package itself pass exactly as before, with byte-identical bzip2 streams, and `cmake --install` lays the files out as each platform's bzip2 package does. Not supported: Windows on ARM64. See `docs/spec.md` §7 for the milestones and what is left.

## Building and testing

Requirements: CMake 3.16 or later and a C11 compiler — gcc 9 or later, clang 10 or later, Apple clang, or MSVC (Visual Studio 2022 or later; tested with 2026). No other dependencies.

```sh
cmake -S c -B c/build -DCMAKE_BUILD_TYPE=Release
cmake --build c/build
ctest --test-dir c/build
```

The test suite takes a few minutes; it includes the differential testers against a reference libbz2 1.0.8 built from the unmodified sources in `c/test/reference/`, a memory-bound test with a counting allocator, every kernel tier, and bzip2's own tests through the `bzip2` program. With MSVC, configure with `-A x64` and build and test with `--config Release`.

Build options (`-D` on the configure line):

| Option | Default | What it does |
|---|---|---|
| `BZ2NG_SONAME` | `1.0` | Shared-library SONAME version: `1.0` as upstream and Debian/Ubuntu use, `1` as Fedora and openSUSE use. |
| `BZ2NG_DECODER_IN_FORMULA` | `OFF` | Keep decompression memory inside libbz2's documented 100 kB + 4 × block size. The default uses one block more (100 kB + 5 × block size) and decompresses full-size blocks 1–7% faster, depending on the machine; see "Compatibility" below. |
| `BZ2NG_NO_STDIO` | `OFF` | Build without the `FILE *` layer, as libbz2's `BZ_NO_STDIO` does. |
| `BZ2NG_BUILD_SHARED`, `BZ2NG_BUILD_PROGRAMS`, `BZ2NG_BUILD_TESTS`, `BZ2NG_BUILD_BENCH`, `BZ2NG_BUILD_FUZZ` | shared, programs and tests `ON`; bench and fuzz `OFF` | What to build besides the static library. |
| `BZ2NG_SANITIZE` | `OFF` | Build everything with AddressSanitizer and UndefinedBehaviorSanitizer. |

For tests and measurement only, the environment variable `BZ2NG_TIER` (`scalar`, `sse4`, `avx2`, `neon`) forces a kernel tier, and on AArch64 `BZ2NG_NO_PMULL` disables the PMULL CRC kernel; neither affects output.

To benchmark against libbz2: configure with `-DBZ2NG_BUILD_BENCH=ON`, build the `bench` target, fetch the corpora (`data/README.md`), then `sh c/scripts/bench_run.sh c/build/bench/bench out.tsv 9 5 data` and `python3 c/scripts/bench_summary.py out.tsv "my machine"`. The benchmark program links stock libbz2 and bz2-ng into one binary and times them on identical streams, verifying every byte first.

## Installing and using it as a drop-in

`cmake --install` gives each file the name and place it has in your platform's own bzip2 package, so bz2-ng replaces that package with nothing renamed, and programs already linked against libbz2 pick it up through the SONAME without relinking:

```sh
cmake -S c -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
DESTDIR="$PWD/stage" cmake --install build      # or without DESTDIR, over the distribution's files
```

| Platform | Configure with | Library files |
|---|---|---|
| Debian, Ubuntu | (nothing) | `libbz2.so.1.0.8`, SONAME `libbz2.so.1.0`, links `libbz2.so.1.0`, `libbz2.so.1`, `libbz2.so`; `libbz2.a` |
| Fedora | `-DBZ2NG_SONAME=1` | `libbz2.so.1.0.8`, SONAME `libbz2.so.1`, links `libbz2.so.1`, `libbz2.so`; `libbz2.a` |
| macOS | (nothing) | `libbz2.1.0.8.dylib`, install name `<prefix>/lib/libbz2.1.0.dylib`, links `libbz2.1.0.dylib`, `libbz2.dylib`; `libbz2.a` |
| Windows, mingw-w64 (msys2) | (nothing) | `bin/libbz2-1.dll`, `lib/libbz2.dll.a`, `lib/libbz2.a` |
| Windows, MSVC (vcpkg) | (nothing) | `bin/bz2.dll`, `lib/bz2.lib`, `lib/bz2_static.lib`; with `-DBZ2NG_BUILD_SHARED=OFF`, `lib/bz2.lib` alone; Debug builds end in `d` |

Every platform also gets `include/bzlib.h` (upstream's, unmodified), `lib/pkgconfig/bzip2.pc`, and the `bzip2` and `bzip2recover` programs built from upstream's current sources (1.0.8 plus the fixes since, CVE-2026-42250's among them), with `bzip2` using the shared library. `bunzip2` and `bzcat` are links to `bzip2` on Unix and copies of it with mingw-w64. libbz2's manual page `bzip2.1` goes to `share/man/man1`, on Unix with `bunzip2.1`, `bzcat.1` and `bzip2recover.1` linked to it. The scripts `bzdiff`, `bzgrep` and `bzmore` are not installed: keep your distribution's. A test (`install_layout`) checks the installed files against this layout on every platform.

- Linux builds set no rpath, as distribution packages do. For a prefix outside the loader's search path, set `LD_LIBRARY_PATH`, or configure with `CMAKE_INSTALL_RPATH` set to the installed library directory (`lib` or `lib64` under the prefix). To try the library without installing, put the build directory first in `LD_LIBRARY_PATH` (`DYLD_LIBRARY_PATH` on macOS, for programs not protected by System Integrity Protection; on Windows, the DLL beside the program).
- On macOS the install name is the configured `<prefix>/lib`. Install to the prefix you configured: programs installed with `cmake --install --prefix` somewhere else still look in the configured one.
- On 32-bit x86 Windows the functions are stdcall, as in libbz2's own DLL. msys2's and vcpkg's 32-bit packages are cdecl, and bz2-ng does not replace them.

## Compatibility

The contract is in `docs/spec.md` §2; in short: the public header is upstream bzip2's `bzlib.h` unmodified (1.0.8's plus its one later fix); exactly the 24 public functions are exported; every call returns what libbz2 returns and leaves the stream fields as libbz2 leaves them; the caller's output buffer is never touched beyond the bytes delivered; all memory comes from `bzalloc` in the `Init` call; and `BZ2_bzlibVersion()` returns a string beginning `1.0.8, ` so that callers which parse a version keep working.

Where bz2-ng deliberately differs from libbz2 1.0.8 (`docs/spec.md` §2.5 has the reasons and the evidence):

- **Memory.** Compression uses less than libbz2 documents (400 kB + 8 × block size) at every level. Decompression by default uses one block more than documented, 100 kB + 5 × block size, because that layout decodes full-size blocks faster; `BZ2NG_DECODER_IN_FORMULA=ON` restores the documented 100 kB + 4 × block size at a cost of 0–7% on full-size blocks depending on the machine. The individual allocation requests differ in size from libbz2's (fewer, larger requests), which matters only to a caller whose memory pool is carved to libbz2's exact request sizes.
- **A decompression call after an error** returns the same error again. libbz2 resumes its state machine, which the manual does not permit and which reaches undefined behaviour inside libbz2 itself.
- **Exactly periodic blocks** (for example a block of all zeros) have several valid BWT origins; libbz2's choice depends on which of its sorters ran, bz2-ng always writes the last. Such streams differ in 24 bits and decode identically. No file in any benchmark corpus is affected; one 600-byte test input in HTSlib's suite is. Only a byte comparison of compressed output across implementations can see it.
- **Diagnostics** at `verbosity` 2 and above that describe libbz2's own sorter are not printed, since that sorter no longer exists.
- **Private symbols** that leak from libbz2's shared library (`BZ2_blockSort`, `BZ2_decompress`, `BZ2_crc32Table` and the like) are not exported. They are not part of the API. A scan of every package that links libbz2 in Ubuntu 24.04 and Fedora 44 found one that references them, Ubuntu's `unalz`, which bundles a modified copy of libbz2's decoder; it needs rebuilding with its own bzip2 code.

## Layout

- `c/` — the C library: sources in `c/lib/`, the vendored block sorter in `c/third_party/libsais/`, the public header `c/bzlib.h`, tests in `c/test/`, the `bzip2` programs in `c/programs/`, the benchmark in `c/bench/`.
- `docs/spec.md` — the specification: compatibility contract, performance requirements, architecture, verification and milestones.
- `data/` — benchmark corpora: scripts to regenerate the public 1000 Genomes-derived tag and quality corpora (committed in Git LFS) and to fetch the general corpora; see `data/README.md`.
- `THIRD_PARTY.md` — third-party code in the tree and its licences.

## Licensing

bz2-ng's own code is under the MIT licence (`LICENSE`). Files taken from bzip2 — the public header and the `bzip2` programs from upstream's current sources, and the stdio layer, the two legacy drivers and the Huffman code assignment derived from 1.0.8 — keep bzip2's BSD-style licence and are marked as altered where they are. The CPU-feature detection and platform abstractions come from libdeflate (MIT). The block sorter, libsais, is used from the commit at which its author relicensed it to Apache-2.0 WITH LLVM-exception; the exception removes the conflict that plain Apache-2.0 has with GPLv2-only programs, so a GPLv2 program that links the system libbz2 may link bz2-ng. The licence texts are in `c/licenses/` and `THIRD_PARTY.md` lists every component.

## Acknowledgements

Julian Seward's bzip2 and libbzip2, whose format, interface and test suite this library serves; Ilya Grebnov's libsais, and his prompt relicensing of it so that it could be used here; Eric Biggers' libdeflate, whose structure and portability layer this project follows.
