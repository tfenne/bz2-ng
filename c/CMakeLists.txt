# bz2-ng: a drop-in replacement for libbz2 1.0.8.
#
#   cmake -S c -B c/build -DCMAKE_BUILD_TYPE=Release && cmake --build c/build && ctest --test-dir c/build
#
# The library is a flat list of C11 sources (BZ2NG_SOURCES). Every source is
# compiled on every architecture; the ISA-specific ones compile to nothing
# where they do not apply. Only the five ISA translation units get
# instruction-set flags, and only on their own architecture (bzng_isa_flags
# below); the run-time dispatch (lib/kern.h, lib/crc32.c) calls them only on
# CPUs that have the instructions. Nothing is generated at configure time
# except the patched copy of libsais that lib/sort_libsais.c compiles,
# bzip2.pc, the shared library's list of exported symbols and the symbolic
# links that the install adds (and, for the tests, the randomising compressor
# of test/CMakeLists.txt).
cmake_minimum_required(VERSION 3.16)
project(bz2ng VERSION 0.1.0 LANGUAGES C)

# A compiler that CMake cannot identify gets none of the compiler-specific
# settings below (no optimisation, warnings or symbol visibility) and CMake's
# generic platform rules, which on Windows mean MSVC's library names and no
# import library for the DLL.  Such a build half works, so it is refused.
if(NOT CMAKE_C_COMPILER_ID)
  message(FATAL_ERROR
          "CMake could not identify the C compiler ${CMAKE_C_COMPILER}. Set CMAKE_C_COMPILER to gcc, clang "
          "or cl, in a new build directory: CMake tries cc before gcc, and a cc such as MSYS's can be one it "
          "cannot identify.")
endif()

# DJGPP only supports static libraries and  not shared objects.
# DJGPP stdio.h has fdopen only in GNU (non-strictly conforming) C modes.
include(CheckCSourceCompiles)
check_c_source_compiles("#ifndef __DJGPP__
#error not DJGPP
#endif
int main(void) { return 0; }"
                        BZ2NG_TARGET_DJGPP)
if(BZ2NG_TARGET_DJGPP)
  set(BZ2NG_SHARED_DEFAULT OFF)
  set(BZ2NG_TOOLS_DEFAULT OFF)
else()
  set(BZ2NG_SHARED_DEFAULT ON)
  set(BZ2NG_TOOLS_DEFAULT ON)
endif()

option(BZ2NG_BUILD_SHARED "Build the shared library" ${BZ2NG_SHARED_DEFAULT})
if(BZ2NG_TARGET_DJGPP AND BZ2NG_BUILD_SHARED)
  message(FATAL_ERROR "DJGPP supports static libraries only: set BZ2NG_BUILD_SHARED=OFF")
endif()
set(BZ2NG_SONAME "1.0" CACHE STRING "ELF shared-library SONAME version: 1.0 (upstream, Debian, Ubuntu) or 1 (Fedora, openSUSE)")
set_property(CACHE BZ2NG_SONAME PROPERTY STRINGS "1.0" "1")
option(BZ2NG_NO_STDIO "Build without the FILE* layer (BZ_NO_STDIO); static library only" OFF)
option(BZ2NG_SANITIZE "Build everything with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BZ2NG_BUILD_PROGRAMS "Build the bzip2 and bzip2recover programs" ON)
option(BZ2NG_BUILD_TESTS "Build the tests (CTest)" ${BZ2NG_TOOLS_DEFAULT})
option(BZ2NG_BUILD_BENCH "Build the benchmark tool" ${BZ2NG_TOOLS_DEFAULT})
option(BZ2NG_BUILD_FUZZ
       "Build the libFuzzer targets in fuzz/ (clang with libFuzzer; everything built with ASan and UBSan; needs BZ2NG_BUILD_TESTS)"
       OFF)
option(BZ2NG_DECODER_IN_FORMULA
       "Decoder memory inside libbz2's documented 100 kB + 4 x block size (default: 100 kB + 5 x block size, faster on full-size blocks)"
       OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(CheckCCompilerFlag)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(BZ2NG_TARGET_DJGPP)
  set(CMAKE_C_EXTENSIONS ON)
  set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
  set(CMAKE_EXECUTABLE_SUFFIX_C ".exe")
else()
  set(CMAKE_C_EXTENSIONS OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(BZ2NG_WARNINGS -Wall -Wextra)
elseif(MSVC)
  # C4324 reports the padding that an _Alignas asks for.
  set(BZ2NG_WARNINGS /W4 /wd4324)
endif()
# cl: C11 (_Alignas, _Static_assert), which CMake asks for itself only from
# 3.21; the POSIX names libbz2's code uses (setmode, fileno, O_BINARY), which
# Microsoft's C runtime hides wherever __STDC__ is defined; and none of its
# deprecation warnings for standard C and POSIX names.
if(MSVC)
  add_compile_options(/std:c11)
  add_compile_definitions(_CRT_DECLARE_NONSTDC_NAMES=1 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
endif()
if(BZ2NG_SANITIZE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "BZ2NG_SANITIZE needs gcc or clang")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()
# The fuzz build instruments everything, the reference libbz2 included, with
# the sanitisers and libFuzzer's coverage; only the targets in fuzz/ link
# libFuzzer itself.  Apple's clang has no libFuzzer, hence the check.
if(BZ2NG_BUILD_FUZZ)
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
  check_c_source_compiles("
    #include <stddef.h>
    #include <stdint.h>
    int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { return 0; }"
    BZ2NG_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  if(NOT BZ2NG_HAVE_LIBFUZZER)
    message(FATAL_ERROR "BZ2NG_BUILD_FUZZ needs a clang with libFuzzer (-fsanitize=fuzzer); Apple's clang has none")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# The decoder's per-stream memory layout used by BZ2_bzDecompressInit
# (lib/dec.h).
if(BZ2NG_DECODER_IN_FORMULA)
  set(BZ2NG_DEFAULT_LAYOUT BZNG_LAYOUT_FIT)
else()
  set(BZ2NG_DEFAULT_LAYOUT BZNG_LAYOUT_WIDE)
endif()

# Architecture of the target, for the ISA-specific flags. The sources decide
# from the compiler's own macros (lib/common_defs.h) whether to build a vector
# tier, so the flags must follow the same answer. CMAKE_SYSTEM_PROCESSOR is
# empty in a cross build whose toolchain does not set it and is not always
# spelt as matched here; where it is not recognised the compiler is asked.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(BZ2NG_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(BZ2NG_ARCH aarch64)
else()
  include(CheckCSourceCompiles)
  check_c_source_compiles("
    #if !defined(__x86_64__) && !(defined(_M_X64) && !defined(_M_ARM64EC))
    #error not x86-64
    #endif
    int main(void) { return 0; }"
    BZ2NG_COMPILER_TARGETS_X86_64)
  check_c_source_compiles("
    #if !defined(__aarch64__) && !defined(_M_ARM64)
    #error not AArch64
    #endif
    int main(void) { return 0; }"
    BZ2NG_COMPILER_TARGETS_AARCH64)
  if(BZ2NG_COMPILER_TARGETS_X86_64 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BZ2NG_ARCH x86_64)
  elseif(BZ2NG_COMPILER_TARGETS_AARCH64)
    set(BZ2NG_ARCH aarch64)
  else()
    set(BZ2NG_ARCH other)
  endif()
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _n_arch)
  if(_n_arch EQUAL 1)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
      set(BZ2NG_ARCH x86_64)
    elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
      set(BZ2NG_ARCH aarch64)
    endif()
  endif()
endif()

# ---- library sources --------------------------------------------------------------
set(BZ2NG_SOURCES
  lib/bzlib.c            # libbz2's stdio, buffer-to-buffer and zlib-style layer, altered
  lib/compress.c         # legacy compression driver (derived from libbz2)
  lib/enc_block.c        # block core
  lib/enc_huff.c         # Huffman stage (contains libbz2 code)
  lib/enc_sort.c         # least rotation into the sorter
  lib/sort_libsais.c     # block sorter: compiles third_party/libsais
  lib/decompress.c       # legacy decompression driver and header parser (derived from libbz2)
  lib/dec_block.c        # Huffman tables, inverse BWT, text source
  lib/randtable.c        # randomisation table (libbz2)
  lib/crc32.c            # CRC table kernel and dispatch
  lib/crc32_table.c
  lib/utils.c
  lib/kern_scalar.c      # portable kernel tier
  lib/arm/cpu_features.c
  lib/arm/crc32_armv8.c  # ISA: AArch64 CRC32
  lib/arm/crc32_pmull.c  # ISA: AArch64 CRC32 and PMULL
  lib/arm/kern_neon.c    # AArch64 NEON tier (baseline, no flags)
  lib/x86/cpu_features.c
  lib/x86/crc32_pclmul.c # ISA: x86-64 PCLMULQDQ
  lib/x86/crc32_vpclmul.c # ISA: x86-64 VPCLMULQDQ
  lib/x86/kern_sse4.c    # ISA: x86-64-v2 tier
  lib/x86/kern_avx2.c    # ISA: x86-64-v3 tier
)

# ---- libsais, as compiled -----------------------------------------------------------
# lib/sort_libsais.c compiles the vendored libsais with the patches in
# third_party/libsais/patches applied in name order. They are applied here to
# a copy in the build directory, and the result must match the SHA-256 sums
# in patches/SHA256, so that a patch that applies with fuzz, in part or to a
# different libsais never compiles.
set(BZ2NG_LIBSAIS_VENDORED ${PROJECT_SOURCE_DIR}/third_party/libsais)
set(BZ2NG_LIBSAIS_DIR ${PROJECT_BINARY_DIR}/libsais)
file(GLOB BZ2NG_LIBSAIS_PATCHES CONFIGURE_DEPENDS ${BZ2NG_LIBSAIS_VENDORED}/patches/*.patch)
list(SORT BZ2NG_LIBSAIS_PATCHES)
# configure_file makes the vendored files configure dependencies as well.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${BZ2NG_LIBSAIS_VENDORED}/patches/SHA256 ${BZ2NG_LIBSAIS_PATCHES})
function(bzng_patch_libsais)
  set(_files libsais.c libsais.h)
  set(_work ${PROJECT_BINARY_DIR}/libsais-patching)
  file(REMOVE_RECURSE ${_work})
  # By content: GNU patch refuses to patch a symbolic link.
  foreach(_file IN LISTS _files)
    configure_file(${BZ2NG_LIBSAIS_VENDORED}/${_file} ${_work}/${_file} COPYONLY)
  endforeach()
  if(BZ2NG_LIBSAIS_PATCHES)
    # Git for Windows installs patch outside the PATH it sets.
    find_program(BZ2NG_PATCH patch PATHS "C:/Program Files/Git/usr/bin")
    find_program(BZ2NG_GIT git)
    if(BZ2NG_PATCH)
      set(_tool ${BZ2NG_PATCH})
      set(_apply ${BZ2NG_PATCH} -p1 -N -i)
    elseif(BZ2NG_GIT)
      # The ceiling keeps git from taking the build directory to be inside a
      # repository, whose configuration could change how a patch applies, and
      # core.autocrlf=false overrides a user's or system's setting, which
      # would otherwise write CRLF line endings.
      set(_tool "${BZ2NG_GIT} apply")
      set(_apply ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${PROJECT_BINARY_DIR}
                 ${BZ2NG_GIT} -c core.autocrlf=false apply -p1 --whitespace=nowarn)
    else()
      message(FATAL_ERROR
              "patch was not found: the build applies third_party/libsais/patches with it (or with git, "
              "also not found). Install patch, for example with apt-get install patch, and configure again.")
    endif()
  endif()
  foreach(_patch IN LISTS BZ2NG_LIBSAIS_PATCHES)
    execute_process(COMMAND ${_apply} ${_patch} WORKING_DIRECTORY ${_work}
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _log ERROR_VARIABLE _log)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "${_patch} does not apply to libsais:\n${_log}")
    endif()
    get_filename_component(_name ${_patch} NAME)
    message(STATUS "libsais: applied ${_name} with ${_tool}")
  endforeach()
  file(STRINGS ${BZ2NG_LIBSAIS_VENDORED}/patches/SHA256 _lines)
  foreach(_line IN LISTS _lines)
    if(NOT _line MATCHES "^([0-9a-f]+)  (.+)$")
      message(FATAL_ERROR "third_party/libsais/patches/SHA256 has a line that is not a SHA-256 sum: ${_line}")
    endif()
    set(_sum_${CMAKE_MATCH_2} ${CMAKE_MATCH_1})
  endforeach()
  foreach(_file IN LISTS _files)
    if(NOT DEFINED _sum_${_file})
      message(FATAL_ERROR "third_party/libsais/patches/SHA256 has no sum for ${_file}")
    endif()
    file(SHA256 ${_work}/${_file} _have)
    if(NOT _have STREQUAL _sum_${_file})
      message(FATAL_ERROR
              "libsais's ${_file} with third_party/libsais/patches applied has SHA-256 ${_have}, not the "
              "${_sum_${_file}} that patches/SHA256 records: the vendored file, a patch or the record has "
              "changed.")
    endif()
  endforeach()
  # Unchanged files keep their time stamps, so that configuring again does not
  # rebuild the sorter.
  foreach(_file IN LISTS _files)
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_work}/${_file} ${BZ2NG_LIBSAIS_DIR}/${_file}
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "could not write ${BZ2NG_LIBSAIS_DIR}/${_file}")
    endif()
  endforeach()
  file(REMOVE_RECURSE ${_work})
endfunction()
bzng_patch_libsais()

# Per-file ISA flags.
function(bzng_isa_flags)
  if(BZ2NG_ARCH STREQUAL "x86_64" AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    # cl compiles any intrinsic in any file (lib/vec.h). /arch:AVX2 defines
    # __AVX2__ for the AVX2 tier and VEX-encodes all of a file's vector code.
    set_source_files_properties(lib/x86/kern_avx2.c lib/x86/crc32_vpclmul.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  elseif(BZ2NG_ARCH STREQUAL "x86_64")
    # gcc before 11 and clang before 12 do not know the level names; for them
    # the same instruction sets are named one by one.
    set(_v2 -mcx16 -msahf -mpopcnt -msse3 -msse4.1 -msse4.2 -mssse3)
    set(_v3 ${_v2} -mavx -mavx2 -mbmi -mbmi2 -mf16c -mfma -mlzcnt -mmovbe -mxsave)
    check_c_compiler_flag(-march=x86-64-v3 BZ2NG_HAVE_MARCH_LEVELS)
    if(BZ2NG_HAVE_MARCH_LEVELS)
      set(_v2 -march=x86-64-v2)
      set(_v3 -march=x86-64-v3)
    endif()
    set_source_files_properties(lib/x86/kern_sse4.c PROPERTIES COMPILE_OPTIONS "${_v2}")
    set_source_files_properties(lib/x86/kern_avx2.c PROPERTIES COMPILE_OPTIONS "${_v3}")
    set_source_files_properties(lib/x86/crc32_pclmul.c PROPERTIES COMPILE_OPTIONS "-msse4.1;-mpclmul")
    set_source_files_properties(lib/x86/crc32_vpclmul.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mpclmul;-mvpclmulqdq")
  elseif(BZ2NG_ARCH STREQUAL "aarch64")
    set_source_files_properties(lib/arm/crc32_armv8.c PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crc")
    set_source_files_properties(lib/arm/crc32_pmull.c PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crc+crypto")
  endif()
endfunction()
bzng_isa_flags()

# Optimisation: the encoder and sorter at -O3, everything else at -O2
# (measured: the encoder is faster at -O3, the decoder and the kernel tiers
# gain nothing from it; libsais is built as it was timed, and at -O2 it was
# no different where that was tried).
# Appended after the build type's flags, so they win.
set(BZ2NG_O3_SOURCES lib/compress.c lib/enc_block.c lib/enc_huff.c lib/enc_sort.c lib/sort_libsais.c)

set(BZ2NG_EXPORTED
  BZ2_bzCompressInit BZ2_bzCompress BZ2_bzCompressEnd
  BZ2_bzDecompressInit BZ2_bzDecompress BZ2_bzDecompressEnd
  BZ2_bzBuffToBuffCompress BZ2_bzBuffToBuffDecompress BZ2_bzlibVersion)
if(NOT BZ2NG_NO_STDIO)
  list(APPEND BZ2NG_EXPORTED
    BZ2_bzReadOpen BZ2_bzReadClose BZ2_bzReadGetUnused BZ2_bzRead
    BZ2_bzWriteOpen BZ2_bzWrite BZ2_bzWriteClose BZ2_bzWriteClose64
    BZ2_bzopen BZ2_bzdopen BZ2_bzread BZ2_bzwrite BZ2_bzflush BZ2_bzclose BZ2_bzerror)
endif()

# Common settings of every compilation of the library sources.
function(bzng_library_settings target)
  # PROJECT_SOURCE_DIR, not the current directory: test/ calls this too, and
  # a wrong path goes unnoticed wherever the system has a bzlib.h of its own.
  target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/lib)
  # _DEFAULT_SOURCE: glibc hides POSIX functions (fdopen, used by the stdio
  # layer) under a strict -std=c11.
  target_compile_definitions(${target} PRIVATE BZNG_VERSION="${PROJECT_VERSION}" _FILE_OFFSET_BITS=64 _DEFAULT_SOURCE)
  if(BZ2NG_NO_STDIO)
    target_compile_definitions(${target} PUBLIC BZ_NO_STDIO)
  endif()
  # ASSERT (lib/lib_common.h) is compiled in for debug and sanitiser builds.
  if(BZ2NG_SANITIZE OR BZ2NG_BUILD_FUZZ)
    target_compile_definitions(${target} PRIVATE BZNG_ENABLE_ASSERTIONS)
  else()
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:BZNG_ENABLE_ASSERTIONS>)
  endif()
  target_compile_options(${target} PRIVATE ${BZ2NG_WARNINGS})
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    if(NOT BZ2NG_TARGET_DJGPP)
      target_compile_options(${target} PRIVATE -fvisibility=hidden)
    endif()
    target_compile_options(${target} PRIVATE $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O2>)
  endif()
endfunction()
foreach(_src IN LISTS BZ2NG_O3_SOURCES)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    get_source_file_property(_opts ${_src} COMPILE_OPTIONS)
    if(NOT _opts)
      set(_opts "")
    endif()
    list(APPEND _opts "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O3>")
    set_source_files_properties(${_src} PROPERTIES COMPILE_OPTIONS "${_opts}")
  endif()
endforeach()
# gcc's first scheduling pass on AArch64 moves a prefetch in libsais's final
# induction scans above the test that guards it, since a prefetch cannot trap.
# It then also runs for marked entries, at addresses gigabytes outside the
# arrays, and Arm cores stall on each of those for tens of nanoseconds,
# costing a third of the sorter's speed.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND BZ2NG_ARCH STREQUAL "aarch64")
  set_property(SOURCE lib/sort_libsais.c APPEND PROPERTY COMPILE_OPTIONS -fno-sched-spec)
endif()
# libbz2's own code in bzlib.c has an unused parameter (BZ2_bzflush), and
# for cl also stores fread's and fwrite's size_t results in its Int32s.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(lib/bzlib.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
elseif(MSVC)
  set_source_files_properties(lib/bzlib.c PROPERTIES COMPILE_OPTIONS "/wd4100;/wd4267")
endif()

# decompress.c is compiled on its own so the tests can build libraries with
# each decoder memory layout as the default.
set(BZ2NG_CORE_SOURCES ${BZ2NG_SOURCES})
list(REMOVE_ITEM BZ2NG_CORE_SOURCES lib/decompress.c)
add_library(bz2_core_objects OBJECT ${BZ2NG_CORE_SOURCES})
bzng_library_settings(bz2_core_objects)
# Where lib/sort_libsais.c finds libsais.c.
target_include_directories(bz2_core_objects PRIVATE ${BZ2NG_LIBSAIS_DIR})
add_library(bz2_dec_objects OBJECT lib/decompress.c)
bzng_library_settings(bz2_dec_objects)
target_compile_definitions(bz2_dec_objects PRIVATE BZNG_DEFAULT_LAYOUT=${BZ2NG_DEFAULT_LAYOUT})

add_library(bz2_static STATIC $<TARGET_OBJECTS:bz2_core_objects> $<TARGET_OBJECTS:bz2_dec_objects>)
set_target_properties(bz2_static PROPERTIES OUTPUT_NAME bz2)
target_include_directories(bz2_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(BZ2NG_NO_STDIO)
  target_compile_definitions(bz2_static PUBLIC BZ_NO_STDIO)
endif()

if(BZ2NG_BUILD_SHARED AND NOT BZ2NG_NO_STDIO)
  add_library(bz2_shared SHARED $<TARGET_OBJECTS:bz2_core_objects> $<TARGET_OBJECTS:bz2_dec_objects>)
  set_target_properties(bz2_shared PROPERTIES OUTPUT_NAME bz2 VERSION 1.0.8 SOVERSION ${BZ2NG_SONAME})
  target_include_directories(bz2_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  # Export exactly the public functions.
  if(APPLE)
    set(_list "")
    foreach(_sym IN LISTS BZ2NG_EXPORTED)
      string(APPEND _list "_${_sym}\n")
    endforeach()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bz2.exports "${_list}")
    target_link_options(bz2_shared PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/bz2.exports")
  elseif(WIN32)
    # A module-definition file, as libbz2's own libbz2.def: it also gives the
    # names undecorated where WINAPI decorates them (32-bit x86).
    set(_list "EXPORTS\n")
    foreach(_sym IN LISTS BZ2NG_EXPORTED)
      string(APPEND _list "  ${_sym}\n")
    endforeach()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bz2.def "${_list}")
    target_sources(bz2_shared PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/bz2.def)
  else()
    set(_list "")
    foreach(_sym IN LISTS BZ2NG_EXPORTED)
      string(APPEND _list "    ${_sym};\n")
    endforeach()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bz2.map "{\n  global:\n${_list}  local:\n    *;\n};\n")
    target_link_options(bz2_shared PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/bz2.map")
  endif()
  # A drop-in DLL needs no GCC runtime DLL beside it (the portable tier calls
  # libgcc's popcount).  On 32-bit x86 the .def file's undecorated names are
  # matched to the stdcall-decorated functions, which is intended, so ld is
  # told not to warn about it.
  if(MINGW)
    target_link_options(bz2_shared PRIVATE -static-libgcc "LINKER:--enable-stdcall-fixup")
  endif()
  # A program built with bzlib.h on 32-bit x86 calls the stdcall names
  # (BZ2_bzRead@16), which the import library must hold.  ld from binutils
  # 2.40 writes the .def file's undecorated names into it instead (2.42 writes
  # the decorated ones), so dlltool rewrites it from the decorated names, each
  # importing the DLL's undecorated export (-k).  The decorations are the
  # functions' argument bytes in bzlib.h; the abi_dll_imports test links a
  # program that calls all of them.  dlltool names the archive's members after
  # the path it is given for the library, so it runs in the library's directory
  # with the bare file name: given the full path, every member name carries the
  # build directory (libbz2.dll.a held C__Users_..._libbz2_dll_a_t.o), and the
  # file differs from one build directory to the next.
  if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_stdcall_names
      BZ2_bzCompressInit@16 BZ2_bzCompress@8 BZ2_bzCompressEnd@4
      BZ2_bzDecompressInit@12 BZ2_bzDecompress@4 BZ2_bzDecompressEnd@4
      BZ2_bzBuffToBuffCompress@28 BZ2_bzBuffToBuffDecompress@24 BZ2_bzlibVersion@0
      BZ2_bzReadOpen@24 BZ2_bzReadClose@8 BZ2_bzReadGetUnused@16 BZ2_bzRead@16
      BZ2_bzWriteOpen@20 BZ2_bzWrite@16 BZ2_bzWriteClose@20 BZ2_bzWriteClose64@28
      BZ2_bzopen@8 BZ2_bzdopen@8 BZ2_bzread@12 BZ2_bzwrite@12 BZ2_bzflush@4 BZ2_bzclose@4 BZ2_bzerror@8)
    list(TRANSFORM _stdcall_names REPLACE "@[0-9]+$" "" OUTPUT_VARIABLE _undecorated)
    if(NOT _undecorated STREQUAL BZ2NG_EXPORTED)
      message(FATAL_ERROR "the stdcall names for the import library do not match BZ2NG_EXPORTED")
    endif()
    if(NOT CMAKE_DLLTOOL)
      message(FATAL_ERROR "32-bit mingw-w64 needs dlltool for the import library; set CMAKE_DLLTOOL")
    endif()
    list(JOIN _stdcall_names "\n  " _list)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bz2-stdcall.def "EXPORTS\n  ${_list}\n")
    add_custom_command(TARGET bz2_shared POST_BUILD
                       COMMAND ${CMAKE_DLLTOOL} -m i386 -k -d ${CMAKE_CURRENT_BINARY_DIR}/bz2-stdcall.def
                               -D $<TARGET_FILE_NAME:bz2_shared> -l $<TARGET_LINKER_FILE_NAME:bz2_shared>
                       WORKING_DIRECTORY $<TARGET_LINKER_FILE_DIR:bz2_shared>
                       VERBATIM)
  endif()
endif()

# ---- library names ------------------------------------------------------------------
# Each is the name of the file it replaces in the platform's own bzip2
# package, read from the packages themselves.  On ELF
# targets: libbz2.a, and libbz2.so.1.0.8 with the SONAME BZ2NG_SONAME selects
# and the links the install rules add.  The file is named as by upstream's
# Makefile-libbz2_so and Fedora; Debian and Ubuntu call theirs
# libbz2.so.1.0.4, a name nothing loads it by.
if(APPLE AND TARGET bz2_shared)
  # macOS's own /usr/lib/libbz2.1.0.dylib, compatibility version 1.0.0 and
  # current version 1.0.8, which is what programs built for macOS record;
  # Homebrew's formula ships no dylib there.  The install name is absolute,
  # like the system's, so that the installed bzip2 finds the library without
  # an rpath.
  if(NOT BZ2NG_SONAME STREQUAL "1.0")
    message(FATAL_ERROR "BZ2NG_SONAME applies to ELF targets; on macOS the library is libbz2.1.0.dylib")
  endif()
  set_target_properties(bz2_shared PROPERTIES INSTALL_NAME_DIR ${CMAKE_INSTALL_FULL_LIBDIR})
elseif(MINGW AND TARGET bz2_shared)
  # msys2's libbz2-1.dll, which its programs load by that name; the import
  # library keeps CMake's libbz2.dll.a, as msys2's does.
  set_target_properties(bz2_shared PROPERTIES RUNTIME_OUTPUT_NAME bz2-1)
elseif(WIN32 AND NOT MINGW)
  # vcpkg's bz2.dll with the import library bz2.lib, or bz2.lib alone where
  # it builds no DLL, and bz2d for debug builds.  Beside a DLL the static
  # library cannot be bz2.lib, so it is bz2_static.lib, as in libbz2's own
  # CMake build.
  set_target_properties(bz2_static PROPERTIES DEBUG_POSTFIX d)
  if(TARGET bz2_shared)
    set_target_properties(bz2_shared PROPERTIES DEBUG_POSTFIX d)
    set_target_properties(bz2_static PROPERTIES OUTPUT_NAME bz2_static)
  endif()
endif()

# ---- pkg-config and install ---------------------------------------------------------
# Links the install adds, made in the build tree so that DESTDIR and --prefix
# apply to them as to every other file: name, pointing at target, is
# installed into destination.
function(bzng_install_link target name destination)
  file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/install-links)
  file(CREATE_LINK ${target} ${PROJECT_BINARY_DIR}/install-links/${name} SYMBOLIC)
  install(FILES ${PROJECT_BINARY_DIR}/install-links/${name} DESTINATION ${destination})
endfunction()
if(BZ2NG_NO_STDIO)
  set(BZ2NG_PC_CFLAGS " -DBZ_NO_STDIO")
else()
  set(BZ2NG_PC_CFLAGS "")
endif()
configure_file(bzip2.pc.in ${CMAKE_CURRENT_BINARY_DIR}/bzip2.pc @ONLY)
install(FILES bzlib.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bzip2.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(TARGETS bz2_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(TARGET bz2_shared)
  install(TARGETS bz2_shared RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  # Debian and Ubuntu add libbz2.so.1 beside their SONAME libbz2.so.1.0, so
  # that programs linked where the SONAME is libbz2.so.1 run too.
  if(UNIX AND NOT APPLE AND BZ2NG_SONAME STREQUAL "1.0")
    bzng_install_link(libbz2.so.1.0.8 libbz2.so.1 ${CMAKE_INSTALL_LIBDIR})
  endif()
endif()

# ---- programs (libbz2 1.0.8's, unmodified) -------------------------------------------
if(BZ2NG_BUILD_PROGRAMS AND NOT BZ2NG_NO_STDIO)
  foreach(_prog bzip2 bzip2recover)
    add_executable(${_prog} programs/${_prog}.c)
    target_compile_definitions(${_prog} PRIVATE _FILE_OFFSET_BITS=64 _DEFAULT_SOURCE)
    # libbz2's unmodified programs: quiet the warnings modern compilers give
    # for their code (an ignored fchown result, "%Lu" for a 64-bit value,
    # sprintf, which macOS's SDK marks deprecated).
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${_prog} PRIVATE -Wno-unused-result -Wno-format -Wno-deprecated-declarations)
    endif()
    install(TARGETS ${_prog} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endforeach()
  # bzip2 loads the shared library where there is one, as the bzip2 of
  # Debian, Fedora, msys2 and macOS does.  bzip2recover calls no library
  # function, and libbz2's Makefile links it with nothing.
  if(TARGET bz2_shared)
    target_link_libraries(bzip2 PRIVATE bz2_shared)
  else()
    target_link_libraries(bzip2 PRIVATE bz2_static)
  endif()
  # bunzip2 and bzcat are bzip2, which reads the name it was started by:
  # links on Unix (Debian, Fedora), copies with mingw (msys2), and neither
  # with MSVC (vcpkg ships bzip2 and bzip2recover alone).  libbz2 1.0.8's
  # manual page, with the other three programs' pages as links to it on Unix
  # (Debian, Fedora, macOS); msys2 ships the page alone, vcpkg none.
  if(NOT WIN32)
    foreach(_alias bunzip2 bzcat)
      bzng_install_link(bzip2 ${_alias} ${CMAKE_INSTALL_BINDIR})
    endforeach()
    foreach(_page bunzip2 bzcat bzip2recover)
      bzng_install_link(bzip2.1 ${_page}.1 ${CMAKE_INSTALL_MANDIR}/man1)
    endforeach()
  elseif(MINGW)
    foreach(_alias bunzip2 bzcat)
      install(PROGRAMS $<TARGET_FILE:bzip2> DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME ${_alias}.exe)
    endforeach()
  endif()
  if(NOT WIN32 OR MINGW)
    install(FILES programs/bzip2.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
  endif()
endif()

# ---- stock libbz2 1.0.8 for the tests and the benchmark ---------------------------
# test/reference holds its unmodified sources; built with every global symbol
# renamed ref_* (test/reference_rename.h) so that it links next to bz2-ng, at
# libbz2's own -O2, and without warnings.
if((BZ2NG_BUILD_TESTS OR BZ2NG_BUILD_BENCH) AND NOT BZ2NG_NO_STDIO)
  if(MSVC)
    set(BZ2NG_RENAME_REFERENCE /FI${CMAKE_CURRENT_SOURCE_DIR}/test/reference_rename.h)
    set(BZ2NG_NO_WARNINGS /W0)
  else()
    set(BZ2NG_RENAME_REFERENCE -include ${CMAKE_CURRENT_SOURCE_DIR}/test/reference_rename.h)
    set(BZ2NG_NO_WARNINGS -w)
  endif()
  set(BZ2NG_REFERENCE_SOURCES blocksort bzlib compress crctable decompress huffman randtable)
  list(TRANSFORM BZ2NG_REFERENCE_SOURCES PREPEND test/reference/)
  list(TRANSFORM BZ2NG_REFERENCE_SOURCES APPEND .c)
  add_library(bz2_reference STATIC ${BZ2NG_REFERENCE_SOURCES})
  target_include_directories(bz2_reference BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/reference
                             INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_compile_definitions(bz2_reference PRIVATE BZPFX=ref_ _FILE_OFFSET_BITS=64 _DEFAULT_SOURCE)
  target_compile_options(bz2_reference PRIVATE ${BZ2NG_RENAME_REFERENCE} ${BZ2NG_NO_WARNINGS}
                         $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O2>)
endif()

# MSVC's toolchain has no nm or objdump; the symbol tests read objects,
# libraries and the DLL with its dumpbin.
if(BZ2NG_BUILD_TESTS AND MSVC)
  get_filename_component(_msvc_bin ${CMAKE_C_COMPILER} DIRECTORY)
  find_program(BZ2NG_DUMPBIN dumpbin HINTS ${_msvc_bin})
  set(BZ2NG_SYMBOL_TOOL -DDUMPBIN=${BZ2NG_DUMPBIN})
endif()
if(BZ2NG_BUILD_TESTS AND NOT BZ2NG_NO_STDIO)
  enable_testing()
  add_subdirectory(test)
elseif(BZ2NG_BUILD_TESTS)
  # Without stdio: the buffer-to-buffer functions with the integrator's
  # bz_internal_error, and a check that the library references no stdio.
  enable_testing()
  add_executable(no_stdio_test test/no_stdio_test.c)
  target_link_libraries(no_stdio_test PRIVATE bz2_static)
  add_test(NAME no_stdio_round_trip COMMAND no_stdio_test)
  add_test(NAME no_stdio_symbols
           COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:bz2_static> ${BZ2NG_SYMBOL_TOOL}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/test/no_stdio_symbols.cmake)
endif()
if(BZ2NG_BUILD_FUZZ)
  # The seed generator reuses the tests' reference and randomising builds.
  if(NOT BZ2NG_BUILD_TESTS OR BZ2NG_NO_STDIO)
    message(FATAL_ERROR "BZ2NG_BUILD_FUZZ needs BZ2NG_BUILD_TESTS and the stdio layer")
  endif()
  add_subdirectory(fuzz)
endif()
if(BZ2NG_BUILD_BENCH AND BZ2NG_NO_STDIO)
  message(STATUS "bench: not built (it uses the stdio layer; BZ2NG_NO_STDIO is on)")
elseif(BZ2NG_BUILD_BENCH)
  add_subdirectory(bench)
endif()
