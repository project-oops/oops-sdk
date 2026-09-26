/*
 * <ctype.h> - what a parser calls.
 *
 * The C locale's classifiers, the only locale here: no table, no `setlocale`, and
 * `isalpha` is the twenty-six letters. A port that needs more than ASCII needs more
 * than this header.
 *
 * On the target include path only - see <libc/math.h>.
 */
#ifndef OOPS_LIBC_CTYPE_H
#define OOPS_LIBC_CTYPE_H

/* C linkage for a C++ includer; `stdlib.h` carries the reasoning. */
#ifdef __cplusplus
extern "C" {
#endif

static inline int isdigit(int c) {
    return c >= '0' && c <= '9';
}
static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int islower(int c) {
    return c >= 'a' && c <= 'z';
}
static inline int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}
static inline int isalpha(int c) {
    return islower(c) || isupper(c);
}
static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}
static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
static inline int isblank(int c) {
    return c == ' ' || c == '\t';
}
static inline int isprint(int c) {
    return c >= 0x20 && c < 0x7f;
}
static inline int isgraph(int c) {
    return c > 0x20 && c < 0x7f;
}
static inline int iscntrl(int c) {
    return (c >= 0 && c < 0x20) || c == 0x7f;
}
static inline int ispunct(int c) {
    return isgraph(c) && !isalnum(c);
}
static inline int tolower(int c) {
    return isupper(c) ? c + 32 : c;
}
static inline int toupper(int c) {
    return islower(c) ? c - 32 : c;
}

/*
 * The character-class bitmasks C++'s `std::ctype_base::mask` is defined from, a bit
 * per class so `std::ctype<char>::is()` can test several at once. libc++ picks these
 * names because the target is `x86_64-unknown-freebsd`.
 *
 * The values are FreeBSD's, from
 * `oops-mesa/toolchain/sysroot/usr/include/_ctype.h:47-58`, so they agree with any mask
 * from elsewhere on this target. `_CTYPE_G` (graph),
 * `_CTYPE_I` (ideogram) and the classes above `_CTYPE_R` are omitted: nothing uses
 * them.
 */
#define _CTYPE_A 0x00000100L /* alpha   */
#define _CTYPE_C 0x00000200L /* control */
#define _CTYPE_D 0x00000400L /* digit   */
#define _CTYPE_L 0x00001000L /* lower   */
#define _CTYPE_P 0x00002000L /* punct   */
#define _CTYPE_S 0x00004000L /* space   */
#define _CTYPE_U 0x00008000L /* upper   */
#define _CTYPE_X 0x00010000L /* hex digit */
#define _CTYPE_B 0x00020000L /* blank   */
#define _CTYPE_R 0x00040000L /* print   */

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_CTYPE_H */
