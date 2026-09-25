/*
 * `inttypes.h` - the `printf`/`scanf` length macros for the fixed-width types.
 *
 * **Why this is not already here.** `-ffreestanding` gets `stdint.h` from the compiler, which
 * supplies it because the standard requires a freestanding implementation to. `inttypes.h` is a
 * *hosted* header, so clang does not, and a port that writes `PRIu32` stops with "file not
 * found" - libgfxd is the first here to do it.
 *
 * The values are for this target's LP64 model, where `int32_t` is `int` and `int64_t` is `long`.
 * That is why the 64-bit forms are `"ld"` and not `"lld"`: on an ILP32 platform they would be the
 * other way round, and a macro that is wrong by one `l` prints garbage rather than failing, which
 * is the kind of mistake worth stating the reasoning against.
 *
 * `imaxdiv_t` and the `strtoimax` family are not here. Nothing has asked for them, and a
 * declaration with no definition behind it is the shape that links clean and faults on the
 * console - see `oops-sdk/AGENTS.md`. They belong here when something needs them.
 */
#ifndef OOPS_LIBC_INTTYPES_H
#define OOPS_LIBC_INTTYPES_H

#include <stdint.h>

/* Signed, decimal. `d` and `i` differ only for `scanf`, where `i` takes a base prefix. */
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"

/* Unsigned: decimal, octal, and hexadecimal in both cases. */
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "lX"

/* The `LEAST` and `FAST` families are the same types here: this target has exact-width types for
 * every size, so the smallest type with at least N bits is the type with exactly N bits. */
#define PRIdLEAST8  PRId8
#define PRIdLEAST16 PRId16
#define PRIdLEAST32 PRId32
#define PRIdLEAST64 PRId64
#define PRIuLEAST8  PRIu8
#define PRIuLEAST16 PRIu16
#define PRIuLEAST32 PRIu32
#define PRIuLEAST64 PRIu64
#define PRIxLEAST8  PRIx8
#define PRIxLEAST16 PRIx16
#define PRIxLEAST32 PRIx32
#define PRIxLEAST64 PRIx64
#define PRIdFAST8   PRId8
#define PRIdFAST16  PRId16
#define PRIdFAST32  PRId32
#define PRIdFAST64  PRId64
#define PRIuFAST8   PRIu8
#define PRIuFAST16  PRIu16
#define PRIuFAST32  PRIu32
#define PRIuFAST64  PRIu64
#define PRIxFAST8   PRIx8
#define PRIxFAST16  PRIx16
#define PRIxFAST32  PRIx32
#define PRIxFAST64  PRIx64

/* `intmax_t` is 64-bit and `intptr_t` is a pointer, which on LP64 is also `long`. */
#define PRIdMAX PRId64
#define PRIiMAX PRIi64
#define PRIuMAX PRIu64
#define PRIoMAX PRIo64
#define PRIxMAX PRIx64
#define PRIXMAX PRIX64
#define PRIdPTR PRId64
#define PRIiPTR PRIi64
#define PRIuPTR PRIu64
#define PRIoPTR PRIo64
#define PRIxPTR PRIx64
#define PRIXPTR PRIX64

/* The `scanf` half. Identical here, because the lengths are a property of the type rather than
 * of the direction. */
#define SCNd8  "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 "ld"
#define SCNi8  "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 "li"
#define SCNu8  "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNo8  "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNo64 "lo"
#define SCNx8  "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 "lx"
#define SCNdMAX SCNd64
#define SCNuMAX SCNu64
#define SCNxMAX SCNx64
#define SCNdPTR SCNd64
#define SCNuPTR SCNu64
#define SCNxPTR SCNx64

#endif /* OOPS_LIBC_INTTYPES_H */
