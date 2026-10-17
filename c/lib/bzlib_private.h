/*-------------------------------------------------------------*/
/*--- Private header file for the library.                  ---*/
/*---                                       bzlib_private.h ---*/
/*-------------------------------------------------------------*/

/* ------------------------------------------------------------------
   This file is part of bzip2/libbzip2, a program and library for
   lossless, block-sorting data compression.

   bzip2/libbzip2 version 1.0.8 of 13 July 2019
   Copyright (C) 1996-2019 Julian Seward <jseward@acm.org>

   Please read the WARNING, DISCLAIMER and PATENTS sections in the
   README file.

   This program is released under the terms of the license contained
   in the file LICENSE.
   ------------------------------------------------------------------ */

/* ALTERED for bz2-ng: this is not the original libbzip2 header. It keeps
   only what bz2-ng's legacy drivers and libbzip2's stdio layer still use
   (the integer typedefs, AssertH and VPrintf, the allocator macros, the
   format constants and the driver state names). EState, DState and the
   block-sorting, Huffman and CRC declarations are gone; the internal-error
   hook is bzng_assert_fail, the randomisation table is bzng_rand_nums, and
   bzlib.h is included with default symbol visibility so that only its
   functions are exported from a library built with -fvisibility=hidden.
   The licence is in licenses/bzip2-1.0.8.txt. */

#ifndef _BZLIB_PRIVATE_H
#define _BZLIB_PRIVATE_H

#include <stdlib.h>
#include <string.h>

#ifndef BZ_NO_STDIO
#include <stdio.h>
#include <ctype.h>
#endif

#if defined(__GNUC__) && !defined(__DJGPP__)
#pragma GCC visibility push(default)
#endif
#include "bzlib.h"
#if defined(__GNUC__) && !defined(__DJGPP__)
#pragma GCC visibility pop
#endif


/*-- General stuff. --*/

#ifndef BZNG_VERSION
#define BZNG_VERSION "0.1.0"
#endif
/* Callers parse the libbzip2 version before the comma. */
#define BZ_VERSION  "1.0.8, bz2-ng " BZNG_VERSION

typedef char            Char;
typedef unsigned char   Bool;
typedef unsigned char   UChar;
typedef int             Int32;
typedef unsigned int    UInt32;
typedef short           Int16;
typedef unsigned short  UInt16;

#define True  ((Bool)1)
#define False ((Bool)0)

#ifndef BZ_NO_STDIO

extern void bzng_assert_fail ( int errcode );
#define AssertH(cond,errcode) \
   { if (!(cond)) bzng_assert_fail ( errcode ); }

#define VPrintf0(zf) \
   fprintf(stderr,zf)
#define VPrintf1(zf,za1) \
   fprintf(stderr,zf,za1)
#define VPrintf2(zf,za1,za2) \
   fprintf(stderr,zf,za1,za2)
#define VPrintf3(zf,za1,za2,za3) \
   fprintf(stderr,zf,za1,za2,za3)
#define VPrintf4(zf,za1,za2,za3,za4) \
   fprintf(stderr,zf,za1,za2,za3,za4)

#else

extern void bz_internal_error ( int errcode );
#define AssertH(cond,errcode) \
   { if (!(cond)) bz_internal_error ( errcode ); }
#define VPrintf0(zf)                     do { } while (0)
#define VPrintf1(zf,za1)                 do { } while (0)
#define VPrintf2(zf,za1,za2)             do { } while (0)
#define VPrintf3(zf,za1,za2,za3)         do { } while (0)
#define VPrintf4(zf,za1,za2,za3,za4)     do { } while (0)

#endif


#define BZALLOC(nnn) (strm->bzalloc)(strm->opaque,(nnn),1)
#define BZFREE(ppp)  (strm->bzfree)(strm->opaque,(ppp))


/*-- Header bytes. --*/

#define BZ_HDR_B 0x42   /* 'B' */
#define BZ_HDR_Z 0x5a   /* 'Z' */
#define BZ_HDR_h 0x68   /* 'h' */
#define BZ_HDR_0 0x30   /* '0' */

/*-- Constants for the back end. --*/

#define BZ_MAX_ALPHA_SIZE 258

#define BZ_RUNA 0
#define BZ_RUNB 1

#define BZ_N_GROUPS 6
#define BZ_G_SIZE   50
#define BZ_N_ITERS  4

#define BZ_MAX_SELECTORS (2 + (900000 / BZ_G_SIZE))


/*-- Stuff for randomising repetitive blocks. --*/

extern const Int32 bzng_rand_nums[512];


/*-- states and modes for compression. --*/

#define BZ_M_IDLE      1
#define BZ_M_RUNNING   2
#define BZ_M_FLUSHING  3
#define BZ_M_FINISHING 4

#define BZ_S_OUTPUT    1
#define BZ_S_INPUT     2


/*-- states for decompression. --*/

#define BZ_X_IDLE        1
#define BZ_X_OUTPUT      2

#define BZ_X_MAGIC_1     10
#define BZ_X_MAGIC_2     11
#define BZ_X_MAGIC_3     12
#define BZ_X_MAGIC_4     13
#define BZ_X_BLKHDR_1    14
#define BZ_X_BLKHDR_2    15
#define BZ_X_BLKHDR_3    16
#define BZ_X_BLKHDR_4    17
#define BZ_X_BLKHDR_5    18
#define BZ_X_BLKHDR_6    19
#define BZ_X_BCRC_1      20
#define BZ_X_BCRC_2      21
#define BZ_X_BCRC_3      22
#define BZ_X_BCRC_4      23
#define BZ_X_RANDBIT     24
#define BZ_X_ORIGPTR_1   25
#define BZ_X_ORIGPTR_2   26
#define BZ_X_ORIGPTR_3   27
#define BZ_X_MAPPING_1   28
#define BZ_X_MAPPING_2   29
#define BZ_X_SELECTOR_1  30
#define BZ_X_SELECTOR_2  31
#define BZ_X_SELECTOR_3  32
#define BZ_X_CODING_1    33
#define BZ_X_CODING_2    34
#define BZ_X_CODING_3    35
#define BZ_X_MTF_1       36
#define BZ_X_MTF_2       37
#define BZ_X_MTF_3       38
#define BZ_X_MTF_4       39
#define BZ_X_MTF_5       40
#define BZ_X_MTF_6       41
#define BZ_X_ENDHDR_2    42
#define BZ_X_ENDHDR_3    43
#define BZ_X_ENDHDR_4    44
#define BZ_X_ENDHDR_5    45
#define BZ_X_ENDHDR_6    46
#define BZ_X_CCRC_1      47
#define BZ_X_CCRC_2      48
#define BZ_X_CCRC_3      49
#define BZ_X_CCRC_4      50


#endif


/*-------------------------------------------------------------*/
/*--- end                                   bzlib_private.h ---*/
/*-------------------------------------------------------------*/
