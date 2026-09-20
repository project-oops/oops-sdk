/*
 * `sscanf`, for the loaders a port arrives with.
 *
 * # Why this exists
 *
 * An OBJ mesh loader is a `fgets` and an `sscanf("%f %f %f")`. So is an MTL material loader, and
 * so is every level format anyone wrote by hand. A port without `sscanf` has its asset loader
 * rewritten, which is the work this library exists to remove.
 *
 * # Why it is here rather than in `src/system/libc.c`
 *
 * That file is target-only - a host build's real C library owns the name `sscanf` - so an
 * algorithm written there could never be run by a test. This builds both ways under the
 * `obs_` prefix and `libc.c` is a one-line rename over it, which is the promise that file's own
 * header makes.
 *
 * # What it converts
 *
 * `%d %i %u %o %x %X %p`, `%f %F %e %E %g %G %a %A`, `%s`, `%c`, `%n`, `%%` and `%[...]`
 * scansets, with the `hh h l ll L z j t` length modifiers, a field width, and `*` to convert
 * without assigning. Whitespace in the format matches any run of input whitespace including
 * none; any other character must match itself.
 *
 * The return is the number of assignments made, or `EOF` when the input ran out before the
 * first one - which is how a caller tells "end of file" from "that line did not parse".
 *
 * # What it is not
 *
 * **Floats are accumulated in double and are not correctly rounded.** The exponent is applied by
 * repeated multiplication, so the last bit or two can differ from a correctly-rounded
 * conversion, and the exponent is clamped at 10^400 - past which the answer is an infinity or a
 * zero anyway. A mesh coordinate does not notice, and a program that would has no business
 * reading it with `scanf`.
 *
 * `fscanf` and `scanf` are **not** here. Both need to put a character back when a conversion
 * reads one too many, and this SDK's file handles have no pushback; a loader reads its line with
 * `fgets` and scans the line, which is what the code being ported does anyway.
 */
#include "oops/freestd.h"

#define OBS_SCAN_NO_WIDTH (1 << 24)

static int obs_scan_isspace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int obs_scan_digit(char c, int base) {
  int d;
  if (c >= '0' && c <= '9') {
    d = c - '0';
  } else if (c >= 'a' && c <= 'z') {
    d = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'Z') {
    d = c - 'A' + 10;
  } else {
    return -1;
  }
  return (d < base) ? d : -1;
}

/*
 * An integer, at most `width` characters. `base` 0 is C's own detection: `0x` for sixteen, a
 * leading zero for eight, otherwise ten. Returns 0 when no digit was read at all, which is a
 * matching failure and ends the whole scan.
 */
static int obs_scan_int(const char **pp, int width, int base, unsigned long long *out,
                        int *neg_out) {
  const char *p = *pp;
  int used = 0;
  int neg = 0;
  if (used < width && (*p == '+' || *p == '-')) {
    neg = (*p == '-');
    p++;
    used++;
  }
  /*
   * **`0x` commits.** Once those two characters are seen under `%x` or `%i`, C has taken them as
   * the start of the input item - "0x" could begin a valid hexadecimal integer - so a missing
   * digit after them fails the conversion rather than falling back to reading the "0" and
   * leaving the "x". The float path above does the same for the same reason, and the
   * differential test against the host's library is what settled both.
   */
  if ((base == 0 || base == 16) && used + 1 < width && p[0] == '0' &&
      (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
    used += 2;
  } else if (base == 0) {
    /* A leading zero is octal, and the zero itself is a digit the loop below reads. */
    base = (p[0] == '0') ? 8 : 10;
  }
  unsigned long long v = 0;
  int digits = 0;
  while (used < width) {
    const int d = obs_scan_digit(*p, base);
    if (d < 0) {
      break;
    }
    v = v * (unsigned long long)base + (unsigned long long)d;
    p++;
    used++;
    digits++;
  }
  if (digits == 0) {
    return 0;
  }
  *pp = p;
  *out = v;
  if (neg_out != NULL) {
    *neg_out = neg;
  }
  return 1;
}

static int obs_scan_float(const char **pp, int width, double *out) {
  const char *p = *pp;
  int used = 0;
  int neg = 0;
  int digits = 0;
  double v = 0.0;
  if (used < width && (*p == '+' || *p == '-')) {
    neg = (*p == '-');
    p++;
    used++;
  }
  /*
   * **The hexadecimal form**, which C99 added and which a program never writes by hand but a
   * `%f` over "0x10" gets from the host's library as sixteen. Left out at first, and the
   * differential test against that library is what said so.
   *
   * `0x` then hex digits, an optional point, and an optional `p` binary exponent - which C does
   * not require here, unlike a hex literal in source.
   */
  if (used + 1 < width && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    /*
     * **Once `0x` is seen there is no falling back to decimal.** "0x" on its own could begin a
     * valid hexadecimal float, so C takes it as the input item and then fails the conversion
     * because it is not one - it does not read the "0" and leave the "x". Returning 1 and a zero
     * there was this library's next difference from the host's library, and the same test found
     * it.
     */
    p += 2;
    used += 2;
    while (used < width && obs_scan_digit(*p, 16) >= 0) {
      v = v * 16.0 + (double)obs_scan_digit(*p, 16);
      p++;
      used++;
      digits++;
    }
    if (used < width && *p == '.') {
      p++;
      used++;
      double scale = 1.0 / 16.0;
      while (used < width && obs_scan_digit(*p, 16) >= 0) {
        v += (double)obs_scan_digit(*p, 16) * scale;
        scale /= 16.0;
        p++;
        used++;
        digits++;
      }
    }
    if (digits == 0) {
      return 0; /* "0x" with nothing that belongs to it */
    }
    if (used < width && (*p == 'p' || *p == 'P')) {
      int eneg = 0;
      int edigits = 0;
      int ev = 0;
      p++;
      used++;
      if (used < width && (*p == '+' || *p == '-')) {
        eneg = (*p == '-');
        p++;
        used++;
      }
      while (used < width && *p >= '0' && *p <= '9') {
        if (ev < 4000) {
          ev = ev * 10 + (*p - '0');
        }
        p++;
        used++;
        edigits++;
      }
      if (edigits == 0) {
        return 0; /* a `p` with no digits fails the conversion, as `e` does below */
      }
      if (ev > 1100) {
        ev = 1100;
      }
      double m = 1.0;
      for (int i = 0; i < ev; i++) {
        m *= 2.0;
      }
      if (eneg) {
        v /= m;
      } else {
        v *= m;
      }
    }
    *pp = p;
    *out = neg ? -v : v;
    return 1;
  }

  while (used < width && *p >= '0' && *p <= '9') {
    v = v * 10.0 + (double)(*p - '0');
    p++;
    used++;
    digits++;
  }
  if (used < width && *p == '.') {
    p++;
    used++;
    double scale = 0.1;
    while (used < width && *p >= '0' && *p <= '9') {
      v += (double)(*p - '0') * scale;
      scale *= 0.1;
      p++;
      used++;
      digits++;
    }
  }
  if (digits == 0) {
    return 0; /* a lone sign or dot is not a number */
  }
  if (used < width && (*p == 'e' || *p == 'E')) {
    /*
     * **"1e" is a matching failure, not the number one.** C defines the input item as the
     * longest sequence that could begin a valid number - "1e" can, because "1e5" is one - and
     * then fails the conversion if that sequence is not itself valid. So an exponent marker
     * with no digits after it takes the whole conversion down rather than being wound back.
     *
     * This library's first draft wound back and returned 1, which reads as the friendlier
     * answer and is the wrong one: the point of having `sscanf` at all is that ported code
     * behaves as it did, and `test_freestd_sscanf_agrees_with_the_host` caught the difference
     * against the host's own library.
     */
    int eneg = 0;
    int edigits = 0;
    int ev = 0;
    p++;
    used++;
    if (used < width && (*p == '+' || *p == '-')) {
      eneg = (*p == '-');
      p++;
      used++;
    }
    while (used < width && *p >= '0' && *p <= '9') {
      if (ev < 400) {
        ev = ev * 10 + (*p - '0');
      }
      p++;
      used++;
      edigits++;
    }
    if (edigits == 0) {
      return 0;
    }
    if (ev > 400) {
      ev = 400;
    }
    double m = 1.0;
    for (int i = 0; i < ev; i++) {
      m *= 10.0;
    }
    if (eneg) {
      v /= m;
    } else {
      v *= m;
    }
  }
  (void)used;
  *pp = p;
  *out = neg ? -v : v;
  return 1;
}

/*
 * A character's membership of a `%[...]` scanset. `set` is the first character inside the
 * bracket and `end` the closing one. A leading `^` negates; a `]` immediately after the bracket
 * (or after the `^`) is a literal, as C says; `a-z` is a range, and a `-` at either end of the
 * set is itself.
 */
static int obs_scan_in_class(const char *set, const char *end, char c) {
  int negate = 0;
  if (set < end && *set == '^') {
    negate = 1;
    set++;
  }
  int hit = 0;
  const char *q = set;
  if (q < end && *q == ']') {
    if (c == ']') {
      hit = 1;
    }
    q++;
  }
  for (; q < end; q++) {
    if (*q == '-' && q > set && q + 1 < end) {
      if (c >= q[-1] && c <= q[1]) {
        hit = 1;
      }
      q++;
      continue;
    }
    if (*q == c) {
      hit = 1;
    }
  }
  return negate ? !hit : hit;
}

/* One conversion's result into whatever the length modifier says the pointer is. */
static void obs_scan_store_int(va_list *ap, int len, int is_signed, unsigned long long v,
                               int neg) {
  const unsigned long long u = neg ? (unsigned long long)(-(long long)v) : v;
  switch (len) {
    case 1: /* hh */
      if (is_signed) {
        *va_arg(*ap, signed char *) = (signed char)u;
      } else {
        *va_arg(*ap, unsigned char *) = (unsigned char)u;
      }
      break;
    case 2: /* h */
      if (is_signed) {
        *va_arg(*ap, short *) = (short)u;
      } else {
        *va_arg(*ap, unsigned short *) = (unsigned short)u;
      }
      break;
    case 3: /* l, z, j, t */
      if (is_signed) {
        *va_arg(*ap, long *) = (long)u;
      } else {
        *va_arg(*ap, unsigned long *) = (unsigned long)u;
      }
      break;
    case 4: /* ll, L */
      if (is_signed) {
        *va_arg(*ap, long long *) = (long long)u;
      } else {
        *va_arg(*ap, unsigned long long *) = u;
      }
      break;
    default:
      if (is_signed) {
        *va_arg(*ap, int *) = (int)u;
      } else {
        *va_arg(*ap, unsigned int *) = (unsigned int)u;
      }
      break;
  }
}

int obs_vsscanf(const char *s, const char *fmt, va_list args) {
  if (s == NULL || fmt == NULL) {
    return -1;
  }
  const char *p = s;
  int assigned = 0;
  int ran_out = 0;
  va_list ap;
  va_copy(ap, args);

  for (; *fmt != '\0'; fmt++) {
    if (obs_scan_isspace(*fmt)) {
      while (obs_scan_isspace(*p)) {
        p++;
      }
      continue;
    }
    if (*fmt != '%') {
      if (*p != *fmt) {
        if (*p == '\0') {
          ran_out = 1;
        }
        goto done; /* a literal that does not match ends the scan */
      }
      p++;
      continue;
    }

    fmt++;
    int suppress = 0;
    int width = OBS_SCAN_NO_WIDTH;
    int len = 0;
    if (*fmt == '*') {
      suppress = 1;
      fmt++;
    }
    if (*fmt >= '0' && *fmt <= '9') {
      width = 0;
      while (*fmt >= '0' && *fmt <= '9') {
        width = width * 10 + (*fmt - '0');
        fmt++;
      }
      if (width == 0) {
        width = OBS_SCAN_NO_WIDTH; /* a width of zero is undefined; treat it as none */
      }
    }
    if (*fmt == 'h') {
      fmt++;
      len = 2;
      if (*fmt == 'h') {
        fmt++;
        len = 1;
      }
    } else if (*fmt == 'l') {
      fmt++;
      len = 3;
      if (*fmt == 'l') {
        fmt++;
        len = 4;
      }
    } else if (*fmt == 'L') {
      fmt++;
      len = 4;
    } else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') {
      fmt++;
      len = 3;
    }

    const char conv = *fmt;
    if (conv == '\0') {
      break; /* a format ending in '%' converts nothing */
    }
    if (conv == '%') {
      if (*p != '%') {
        if (*p == '\0') {
          ran_out = 1;
        }
        goto done;
      }
      p++;
      continue;
    }
    if (conv == 'n') {
      /* Not a conversion: it assigns but does not count, and it cannot fail. */
      if (!suppress) {
        obs_scan_store_int(&ap, len, 1, (unsigned long long)(size_t)(p - s), 0);
      }
      continue;
    }

    /* Every conversion but `%c` and `%[` skips leading whitespace first. */
    if (conv != 'c' && conv != '[') {
      while (obs_scan_isspace(*p)) {
        p++;
      }
    }
    if (*p == '\0') {
      ran_out = 1;
      goto done;
    }

    if (conv == 'c') {
      const int n = (width == OBS_SCAN_NO_WIDTH) ? 1 : width;
      char *dest = suppress ? NULL : va_arg(ap, char *);
      int i = 0;
      while (i < n && p[i] != '\0') {
        i++;
      }
      if (i < n) {
        ran_out = 1;
        goto done; /* not enough characters left: nothing is assigned */
      }
      if (dest != NULL) {
        for (i = 0; i < n; i++) {
          dest[i] = p[i]; /* no terminator: %c is characters, not a string */
        }
        assigned++;
      }
      p += n;
      continue;
    }

    if (conv == 's') {
      char *dest = suppress ? NULL : va_arg(ap, char *);
      int n = 0;
      while (*p != '\0' && !obs_scan_isspace(*p) && n < width) {
        if (dest != NULL) {
          dest[n] = *p;
        }
        p++;
        n++;
      }
      if (n == 0) {
        goto done;
      }
      if (dest != NULL) {
        dest[n] = '\0';
        assigned++;
      }
      continue;
    }

    if (conv == '[') {
      const char *const set = fmt + 1;
      const char *end = set;
      if (*end == '^') {
        end++;
      }
      if (*end == ']') {
        end++;
      }
      while (*end != '\0' && *end != ']') {
        end++;
      }
      if (*end == '\0') {
        goto done; /* a set with no closing bracket is a malformed format */
      }
      char *dest = suppress ? NULL : va_arg(ap, char *);
      int n = 0;
      while (*p != '\0' && n < width && obs_scan_in_class(set, end, *p)) {
        if (dest != NULL) {
          dest[n] = *p;
        }
        p++;
        n++;
      }
      fmt = end; /* the loop's own fmt++ steps past the ']' */
      if (n == 0) {
        goto done;
      }
      if (dest != NULL) {
        dest[n] = '\0';
        assigned++;
      }
      continue;
    }

    if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' ||
        conv == 'G' || conv == 'a' || conv == 'A') {
      double v = 0.0;
      if (!obs_scan_float(&p, width, &v)) {
        goto done;
      }
      if (!suppress) {
        if (len >= 3) {
          *va_arg(ap, double *) = v;
        } else {
          *va_arg(ap, float *) = (float)v;
        }
        assigned++;
      }
      continue;
    }

    {
      int base;
      int is_signed = 1;
      switch (conv) {
        case 'd': base = 10; break;
        case 'i': base = 0; break;
        case 'u': base = 10; is_signed = 0; break;
        case 'o': base = 8; is_signed = 0; break;
        case 'x':
        case 'X': base = 16; is_signed = 0; break;
        case 'p': base = 16; is_signed = 0; len = 4; break;
        default: goto done; /* an unknown conversion ends the scan rather than guessing */
      }
      unsigned long long v = 0;
      int neg = 0;
      if (!obs_scan_int(&p, width, base, &v, &neg)) {
        goto done;
      }
      if (!suppress) {
        obs_scan_store_int(&ap, len, is_signed, v, neg);
        assigned++;
      }
    }
  }

done:
  va_end(ap);
  /* C's rule, and the one a loader's read loop turns on: the input running out before anything
   * was assigned is end of file, which is not the same answer as a line that did not parse. */
  if (assigned == 0 && ran_out) {
    return -1;
  }
  return assigned;
}

int obs_sscanf(const char *s, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int n = obs_vsscanf(s, fmt, args);
  va_end(args);
  return n;
}
