/*
 * dll_imports.c - a client of the DLL that calls every public function
 *
 * Part of bz2-ng; MIT licence (see LICENSE).
 *
 * Built with the unmodified bzlib.h and linked against the DLL's import
 * library, as a program using libbz2 is.  On 32-bit x86 each call names the
 * function's stdcall symbol (BZ2_bzRead@16), so the program links only if
 * the import library resolves all 24; the abi_dll_imports test then reads
 * the names it imports from the DLL.  Run, it prints the library version.
 */

#include <stddef.h>
#include <stdio.h>

#include "bzlib.h"

int
main(int argc, char **argv)
{
	(void)argv;
	/* Never true when run: the calls are there to be linked. */
	if (argc > 1000) {
		BZ2_bzCompressInit(NULL, 0, 0, 0);
		BZ2_bzCompress(NULL, 0);
		BZ2_bzCompressEnd(NULL);
		BZ2_bzDecompressInit(NULL, 0, 0);
		BZ2_bzDecompress(NULL);
		BZ2_bzDecompressEnd(NULL);
		BZ2_bzBuffToBuffCompress(NULL, NULL, NULL, 0, 0, 0, 0);
		BZ2_bzBuffToBuffDecompress(NULL, NULL, NULL, 0, 0, 0);
		BZ2_bzReadOpen(NULL, NULL, 0, 0, NULL, 0);
		BZ2_bzReadClose(NULL, NULL);
		BZ2_bzReadGetUnused(NULL, NULL, NULL, NULL);
		BZ2_bzRead(NULL, NULL, NULL, 0);
		BZ2_bzWriteOpen(NULL, NULL, 0, 0, 0);
		BZ2_bzWrite(NULL, NULL, NULL, 0);
		BZ2_bzWriteClose(NULL, NULL, 0, NULL, NULL);
		BZ2_bzWriteClose64(NULL, NULL, 0, NULL, NULL, NULL, NULL);
		BZ2_bzopen(NULL, NULL);
		BZ2_bzdopen(0, NULL);
		BZ2_bzread(NULL, NULL, 0);
		BZ2_bzwrite(NULL, NULL, 0);
		BZ2_bzflush(NULL);
		BZ2_bzclose(NULL);
		BZ2_bzerror(NULL, NULL);
	}
	puts(BZ2_bzlibVersion());
	return 0;
}
