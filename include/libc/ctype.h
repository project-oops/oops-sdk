/*
 * <ctype.h> - what a parser calls.
 *
 * Every OBJ loader, config reader and command-line splitter a port carries is built out of these.
 * They are the C locale's, which is the only locale here: no table, no `setlocale`, and
 * `isalpha` is the twenty-six letters. A port that needs more than ASCII needs more than this
 * header, and should say so rather than finding out from a mis-parsed file.
 *
 * On the target include path only - see <libc/math.h>.
 */
#ifndef OOPS_LIBC_CTYPE_H
#define OOPS_LIBC_CTYPE_H

/* C linkage for a C++ includer; `stdlib.h` carries the reasoning. */
#ifdef __cplusplus
extern "C" {
#endif

static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int isalpha(int c) { return islower(c) || isupper(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
static inline int isblank(int c) { return c == ' ' || c == '\t'; }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int isgraph(int c) { return c > 0x20 && c < 0x7f; }
static inline int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_CTYPE_H */
