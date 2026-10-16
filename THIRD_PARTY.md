# Third-party code

Code in this repository that was not written for bz2-ng, where it lives, and what its licence requires. All of it ships. Licence texts are in `c/licenses/` (and `c/third_party/libsais/LICENSE`); files taken or derived from another project keep its notice, and altered files say so in their first lines.

| Component | Version | Licence | Location | Notes |
|---|---|---|---|---|
| bzip2 / libbzip2 | git `f5dfc96` (1.0.8 and the fixes since, from `sourceware.org/git/bzip2.git`) | BSD-style (Julian Seward) | `c/bzlib.h`, `c/programs/` | Unmodified: the public header, the `bzip2` and `bzip2recover` programs and their manual page `bzip2.1`. The fixes since 1.0.8 are ten to `bzip2recover` (CVE-2026-42250's block-count check, `O_EXCL` output files, `snprintf`, EOF handling) and `bzip2.c`, the `--help` note in the manual, and the header's `#include`s moved outside `extern "C"`. |
| bzip2 / libbzip2 | 1.0.8 | BSD-style (Julian Seward) | `c/test/reference/` | Unmodified: the reference library and sample files the differential tests compare against (the release a distribution's package is built from). |
| bzip2 / libbzip2, altered | 1.0.8, with upstream's two post-1.0.8 fixes to the stdio layer in `bzlib.c` | BSD-style (Julian Seward) | `c/lib/bzlib.c`, `bzlib_private.h`, `randtable.c`, `compress.c`, `decompress.c`, `enc_huff.c`; `c/test/rand_compress.c` | The stdio and buffer-to-buffer layer, the two legacy drivers (libbz2's state machines) and the Huffman length and code assignment. |
| libdeflate | 1.26 | MIT (Eric Biggers) | `c/lib/common_defs.h`, `cpu_features_common.h`, `c/lib/x86/cpu_features.[ch]`, `c/lib/arm/cpu_features.[ch]` | Compiler and platform abstractions and CPU feature detection. |
| libsais | commit `1a696e6` (after 2.10.4) | Apache-2.0 WITH LLVM-exception (Ilya Grebnov) | `c/third_party/libsais/` | Unmodified `libsais.c` and `libsais.h`: the block sorter. `c/lib/sort_libsais.c` compiles a copy that the build alters at configure time with the patches in `patches/` and checks by SHA-256; the patches are bz2-ng's changes to libsais and, containing its code, are under its licence. That commit is the relicence from plain Apache-2.0; the exception is what makes it usable in a library that GPLv2-only programs link. Earlier releases carry plain Apache-2.0 and nothing is taken from them. |

Sources that may be consulted for ideas but never copied or translated: lbzip2 (GPLv3) and 7-Zip's bzip2 encoder (LGPL).
