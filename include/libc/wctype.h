/*
 * `<wctype.h>`: classifying and case-mapping wide characters, which are Unicode scalar
 * values on this target (`<wchar.h>`).
 *
 * Exact for ASCII and Latin-1. Above that there is no Unicode character database here,
 * and the rules are these, chosen so that text in any script reads as text:
 * - `iswalpha` holds for everything that is not a surrogate, not in the punctuation,
 *   symbol and arrow blocks (U+2000-U+2BFF), not CJK punctuation (U+3000-U+303F), not
 *   private use, and not the half- and full-width punctuation forms. Letters in every
 *   script are therefore alphabetic, and so are a few symbols outside those blocks.
 * - `iswspace` and `iswblank` know Unicode's space separators; `iswdigit` is `0`-`9`
 *   alone, as C requires.
 * - Case maps ASCII, Latin-1, Greek and basic Cyrillic; everything else maps to itself.
 */
#ifndef OOPS_LIBC_WCTYPE_H
#define OOPS_LIBC_WCTYPE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long wctype_t;
typedef int wctrans_t;

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);

/* By name: "alnum" ... "xdigit", and "tolower"/"toupper". 0 for a name not known. */
wctype_t wctype(const char *name);
int iswctype(wint_t c, wctype_t t);
wctrans_t wctrans(const char *name);
wint_t towctrans(wint_t c, wctrans_t t);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_WCTYPE_H */
