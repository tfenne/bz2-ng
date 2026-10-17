# Contributing to bz2-ng

Thank you for your interest. bz2-ng is a drop-in replacement for libbz2 1.0.8 with a faster single-threaded encoder and decoder. `docs/spec.md` is its specification, and every change is held to it.

## Issues

Bug reports are welcome. Please give the platform, the compiler and its version, the full CMake configure line, and the output that shows the problem; for a stream that fails or decodes wrongly, the smallest input that reproduces it, or the commands that generate it.

Evidence from outside is welcome too: drop-in runs on a distribution, scans of the packages that link libbz2, measurements on hardware we do not have. Publishing the scripts with the results lets others repeat them.

## Open an issue before you write code

A small, self-contained fix for a bug on a supported platform can come straight as a pull request. For anything else, please open an issue first and wait until we have agreed on the approach: a new feature, a change to the API, a performance change, a change to the build system or CI, and **any request to support another platform**. Review is the scarce part of this project, and a pull request that arrives without that agreement may be closed without a detailed review, however good the code.

## Platforms

The supported platforms, and what it takes to add one, are in `docs/spec.md` §4.7. bz2-ng exists to replace libbz2 where it is installed today, as a system or package-manager library on current hardware, so the list is short on purpose, and every platform on it is built and tested on CI or on our reference machines.

A request to support another platform is welcome as an issue, but most will be closed. We understand the appeal of a library that builds everywhere. Every platform, though, is a lasting cost in testing and review, so we add one only when there is a case beyond its being possible: that a maintained distribution or package manager ships libbz2 there and would take bz2-ng in its place, and that CI can build and test it. If you can make that case, we would like to hear it.

## What a pull request needs

- Output stays byte-identical to libbz2's, and the legacy API matches libbz2 call for call (`docs/spec.md` §2).
- No GPL or LGPL code, copied or read for reference. MIT, BSD, zlib-licensed and public-domain code may be reused with its notice kept.
- Tests generate their data in code. The corpora under `data/` are for benchmarks and bulk checks.
- C follows libdeflate's style: `c/scripts/format.sh` runs clang-format 21 with `c/.clang-format`. libbz2's own files (`c/bzlib.h`, `c/programs/`, `c/test/reference/`) keep upstream's formatting.
- A performance change carries its measurement: single-threaded, against libbz2 built by the same compiler, in rounds interleaved with the baseline and with a second build of the baseline as a control (`docs/spec.md` §3).
- Documentation is in British English, and Markdown is not hard-wrapped.
- The suite passes: `cmake -S c -B c/build -DCMAKE_BUILD_TYPE=Release && cmake --build c/build && ctest --test-dir c/build`.

CI runs on every pull request that touches `c/` or the workflow. A first-time contributor's run waits until a maintainer approves it.
