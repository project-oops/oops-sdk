/*
 * Freestanding standard helper implementations.
 *
 * Factored out of runtime.c so both the probe and injector/payload layers
 * can use string formatting and memory operations without cross-linking.
 */

#include "oops/freestd.h"
#include "oops/krw.h"

size_t obs_strlen(const char *s) {
  size_t n = 0;
  while (s != NULL && s[n] != '\0') {
    n++;
  }
  return n;
}

int obs_strcmp(const char *s1, const char *s2) {
  if (s1 == NULL && s2 == NULL) {
    return 0;
  }
  if (s1 == NULL) {
    return -1;
  }
  if (s2 == NULL) {
    return 1;
  }
  while (*s1 != '\0' && *s1 == *s2) {
    s1++;
    s2++;
  }
  return (int)((unsigned char)*s1 - (unsigned char)*s2);
}

int obs_strncmp(const char *s1, const char *s2, size_t n) {
  if (n == 0) {
    return 0;
  }
  if (s1 == NULL && s2 == NULL) {
    return 0;
  }
  if (s1 == NULL) {
    return -1;
  }
  if (s2 == NULL) {
    return 1;
  }
  for (size_t i = 0; i < n; i++) {
    if (s1[i] != s2[i] || s1[i] == '\0' || s2[i] == '\0') {
      return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
    }
  }
  return 0;
}

char *obs_strncpy(char *dest, const char *src, size_t n) {
  if (dest == NULL || src == NULL || n == 0) {
    return dest;
  }
  size_t i = 0;
  for (; i < n && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

size_t obs_format_u64(char *dest, uint64_t value) {
  char scratch[OBS_NUM_MAX];
  size_t n = 0;
  if (value == 0) {
    dest[0] = '0';
    return 1;
  }
  while (value > 0 && n < sizeof(scratch)) {
    scratch[n++] = (char)('0' + (value % 10u));
    value /= 10u;
  }
  for (size_t i = 0; i < n; i++) {
    dest[i] = scratch[n - 1 - i];
  }
  return n;
}

size_t obs_format_i64(char *dest, int64_t value) {
  if (value < 0) {
    dest[0] = '-';
    uint64_t magnitude = ~(uint64_t)value + 1u;
    return 1 + obs_format_u64(dest + 1, magnitude);
  }
  return obs_format_u64(dest, (uint64_t)value);
}

size_t obs_format_hex(char *dest, uint64_t value) {
  static const char digits[] = "0123456789abcdef";
  char scratch[OBS_NUM_MAX];
  size_t n = 0;
  dest[0] = '0';
  dest[1] = 'x';
  if (value == 0) {
    dest[2] = '0';
    return 3;
  }
  while (value > 0 && n < sizeof(scratch)) {
    scratch[n++] = digits[value & 0xfu];
    value >>= 4;
  }
  for (size_t i = 0; i < n; i++) {
    dest[2 + i] = scratch[n - 1 - i];
  }
  return 2 + n;
}

#if !defined(OBSCENE_HOST_BUILD) && !defined(OOPS_HOST_BUILD)
void *memset(void *dest, int value, size_t len) {
  unsigned char *d = (unsigned char *)dest;
  for (size_t i = 0; i < len; i++) {
    d[i] = (unsigned char)value;
  }
  return dest;
}

void *memcpy(void *dest, const void *src, size_t len) {
  unsigned char *d = (unsigned char *)dest;
  const unsigned char *s = (const unsigned char *)src;
  for (size_t i = 0; i < len; i++) {
    d[i] = s[i];
  }
  return dest;
}

int memcmp(const void *s1, const void *s2, size_t len) {
  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;
  for (size_t i = 0; i < len; i++) {
    if (p1[i] != p2[i]) {
      return (int)(p1[i] - p2[i]);
    }
  }
  return 0;
}
#endif

static inline uint32_t sha1_rol(uint32_t val, int bits) {
  return (val << bits) | (val >> (32 - bits));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t temp = sha1_rol(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = sha1_rol(b, 30);
    b = a;
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void obs_compute_nid(const char *name, char out_nid[12]) {
  static const uint8_t suffix[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde,
                                     0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1,
                                     0xc3, 0xe5, 0x52, 0x30};
  static const char alphabet[65] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

  uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                       0xC3D2E1F0};
  uint8_t buffer[128];
  for (size_t i = 0; i < sizeof(buffer); i++)
    buffer[i] = 0;

  size_t name_len = obs_strlen(name);
  size_t total_len = name_len + sizeof(suffix);

  size_t buf_pos = 0;
  while (buf_pos < name_len && buf_pos < 100) {
    buffer[buf_pos] = (uint8_t)name[buf_pos];
    buf_pos++;
  }
  for (size_t i = 0; i < sizeof(suffix); i++) {
    buffer[buf_pos++] = suffix[i];
  }

  buffer[buf_pos++] = 0x80;
  size_t block_len = ((buf_pos + 8 + 63) / 64) * 64;
  uint64_t bit_len = (uint64_t)total_len * 8;
  buffer[block_len - 8] = (uint8_t)(bit_len >> 56);
  buffer[block_len - 7] = (uint8_t)(bit_len >> 48);
  buffer[block_len - 6] = (uint8_t)(bit_len >> 40);
  buffer[block_len - 5] = (uint8_t)(bit_len >> 32);
  buffer[block_len - 4] = (uint8_t)(bit_len >> 24);
  buffer[block_len - 3] = (uint8_t)(bit_len >> 16);
  buffer[block_len - 2] = (uint8_t)(bit_len >> 8);
  buffer[block_len - 1] = (uint8_t)(bit_len);

  for (size_t i = 0; i < block_len; i += 64) {
    sha1_transform(state, buffer + i);
  }

  uint8_t digest8[8];
  digest8[0] = (uint8_t)(state[0] >> 24);
  digest8[1] = (uint8_t)(state[0] >> 16);
  digest8[2] = (uint8_t)(state[0] >> 8);
  digest8[3] = (uint8_t)(state[0]);
  digest8[4] = (uint8_t)(state[1] >> 24);
  digest8[5] = (uint8_t)(state[1] >> 16);
  digest8[6] = (uint8_t)(state[1] >> 8);
  digest8[7] = (uint8_t)(state[1]);

  uint64_t val = (uint64_t)digest8[0] | ((uint64_t)digest8[1] << 8) |
                 ((uint64_t)digest8[2] << 16) | ((uint64_t)digest8[3] << 24) |
                 ((uint64_t)digest8[4] << 32) | ((uint64_t)digest8[5] << 40) |
                 ((uint64_t)digest8[6] << 48) | ((uint64_t)digest8[7] << 56);

  unsigned __int128 bits = ((unsigned __int128)val) << 2;
  for (int pos = 10; pos >= 0; pos--) {
    int shift = pos * 6;
    int idx = (int)((bits >> shift) & 0x3F);
    out_nid[10 - pos] = alphabet[idx];
  }
  out_nid[11] = '\0';
}

const void *obs_kexport_lookup(const obs_kexport_table_t *table,
                               const char *nid) {
  if (table == NULL || nid == NULL)
    return NULL;
  uintptr_t utable = (uintptr_t)table;
  if (utable < 0x10000UL || utable >= 0x0000800000000000UL || (utable & 0x7UL) != 0)
    return NULL;
  if (table->count == 0 || table->count > 16384)
    return NULL;
  int low = 0;
  int high = (int)table->count - 1;
  while (low <= high) {
    int mid = low + (high - low) / 2;
    int cmp = obs_strcmp(table->entries[mid].nid, nid);
    if (cmp == 0) {
      return (const void *)(uintptr_t)table->entries[mid].vaddr;
    } else if (cmp < 0) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return NULL;
}

typedef struct {
  char *buf;
  size_t size;
  size_t written;
} snprintf_ctx_t;

static inline void snprintf_putc(snprintf_ctx_t *ctx, char c) {
  if (ctx->buf != NULL && ctx->size > 0) {
    if (ctx->written + 1 < ctx->size) {
      ctx->buf[ctx->written] = c;
    }
  }
  ctx->written++;
}

static void snprintf_puts(snprintf_ctx_t *ctx, const char *s, size_t len, int left_align, size_t width, char pad_char) {
  if (!left_align && width > len) {
    for (size_t i = 0; i < width - len; i++) {
      snprintf_putc(ctx, pad_char);
    }
  }
  for (size_t i = 0; i < len; i++) {
    snprintf_putc(ctx, s[i]);
  }
  if (left_align && width > len) {
    for (size_t i = 0; i < width - len; i++) {
      snprintf_putc(ctx, ' ');
    }
  }
}

int oops_vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
  if (fmt == NULL) {
    if (buf != NULL && size > 0) {
      buf[0] = '\0';
    }
    return 0;
  }

  snprintf_ctx_t ctx;
  ctx.buf = buf;
  ctx.size = size;
  ctx.written = 0;

  for (size_t i = 0; fmt[i] != '\0'; i++) {
    if (fmt[i] != '%') {
      snprintf_putc(&ctx, fmt[i]);
      continue;
    }

    i++; /* Skip '%' */
    if (fmt[i] == '\0') {
      break;
    }
    if (fmt[i] == '%') {
      snprintf_putc(&ctx, '%');
      continue;
    }

    /* Parse flags */
    int left_align = 0;
    int zero_pad = 0;
    int plus_sign = 0;
    int space_sign = 0;
    int alt_form = 0;

    int parsing_flags = 1;
    while (parsing_flags) {
      switch (fmt[i]) {
        case '-': left_align = 1; i++; break;
        case '0': zero_pad = 1; i++; break;
        case '+': plus_sign = 1; i++; break;
        case ' ': space_sign = 1; i++; break;
        case '#': alt_form = 1; i++; break;
        default: parsing_flags = 0; break;
      }
    }
    if (left_align) zero_pad = 0;

    /* Parse width */
    size_t width = 0;
    while (fmt[i] >= '0' && fmt[i] <= '9') {
      width = width * 10 + (size_t)(fmt[i] - '0');
      i++;
    }

    /* Parse length modifier */
    int length_mod = 0; /* 0: default, 1: l, 2: ll, 3: z, 4: h */
    if (fmt[i] == 'l') {
      i++;
      if (fmt[i] == 'l') {
        length_mod = 2;
        i++;
      } else {
        length_mod = 1;
      }
    } else if (fmt[i] == 'z') {
      length_mod = 3;
      i++;
    } else if (fmt[i] == 'h') {
      length_mod = 4;
      i++;
    }

    char spec = fmt[i];
    char scratch[64];
    size_t len = 0;

    if (spec == 's') {
      const char *s = va_arg(args, const char *);
      if (s == NULL) s = "(null)";
      len = obs_strlen(s);
      snprintf_puts(&ctx, s, len, left_align, width, ' ');
    } else if (spec == 'c') {
      char c = (char)va_arg(args, int);
      scratch[0] = c;
      snprintf_puts(&ctx, scratch, 1, left_align, width, ' ');
    } else if (spec == 'd' || spec == 'i') {
      int64_t val = 0;
      if (length_mod == 2) val = va_arg(args, long long);
      else if (length_mod == 1) val = va_arg(args, long);
      else if (length_mod == 3) val = (int64_t)va_arg(args, ssize_t);
      else val = va_arg(args, int);

      uint64_t uval;
      int negative = 0;
      if (val < 0) {
        negative = 1;
        uval = (uint64_t)(-(val + 1)) + 1;
      } else {
        uval = (uint64_t)val;
      }

      char num_buf[32];
      size_t num_len = 0;
      if (uval == 0) {
        num_buf[num_len++] = '0';
      } else {
        while (uval > 0 && num_len < sizeof(num_buf)) {
          num_buf[num_len++] = (char)('0' + (uval % 10));
          uval /= 10;
        }
      }

      /* Compute prefix */
      char pfx = '\0';
      if (negative) pfx = '-';
      else if (plus_sign) pfx = '+';
      else if (space_sign) pfx = ' ';

      size_t total_len = num_len + (pfx ? 1 : 0);
      if (zero_pad) {
        if (pfx) snprintf_putc(&ctx, pfx);
        if (width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, '0');
        }
        for (size_t p = 0; p < num_len; p++) snprintf_putc(&ctx, num_buf[num_len - 1 - p]);
      } else {
        if (!left_align && width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, ' ');
        }
        if (pfx) snprintf_putc(&ctx, pfx);
        for (size_t p = 0; p < num_len; p++) snprintf_putc(&ctx, num_buf[num_len - 1 - p]);
        if (left_align && width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, ' ');
        }
      }
    } else if (spec == 'u') {
      uint64_t uval = 0;
      if (length_mod == 2) uval = va_arg(args, unsigned long long);
      else if (length_mod == 1) uval = va_arg(args, unsigned long);
      else if (length_mod == 3) uval = (uint64_t)va_arg(args, size_t);
      else uval = va_arg(args, unsigned int);

      char num_buf[32];
      size_t num_len = 0;
      if (uval == 0) {
        num_buf[num_len++] = '0';
      } else {
        while (uval > 0 && num_len < sizeof(num_buf)) {
          num_buf[num_len++] = (char)('0' + (uval % 10));
          uval /= 10;
        }
      }

      char pad_ch = zero_pad ? '0' : ' ';
      if (!left_align && width > num_len) {
        for (size_t p = 0; p < width - num_len; p++) snprintf_putc(&ctx, pad_ch);
      }
      for (size_t p = 0; p < num_len; p++) snprintf_putc(&ctx, num_buf[num_len - 1 - p]);
      if (left_align && width > num_len) {
        for (size_t p = 0; p < width - num_len; p++) snprintf_putc(&ctx, ' ');
      }
    } else if (spec == 'x' || spec == 'X') {
      static const char hex_lower[] = "0123456789abcdef";
      static const char hex_upper[] = "0123456789ABCDEF";
      const char *digits = (spec == 'X') ? hex_upper : hex_lower;

      uint64_t uval = 0;
      if (length_mod == 2) uval = va_arg(args, unsigned long long);
      else if (length_mod == 1) uval = va_arg(args, unsigned long);
      else if (length_mod == 3) uval = (uint64_t)va_arg(args, size_t);
      else uval = va_arg(args, unsigned int);

      char num_buf[32];
      size_t num_len = 0;
      if (uval == 0) {
        num_buf[num_len++] = '0';
      } else {
        while (uval > 0 && num_len < sizeof(num_buf)) {
          num_buf[num_len++] = digits[uval & 0xf];
          uval >>= 4;
        }
      }

      size_t pfx_len = (alt_form && uval != 0) ? 2 : 0;
      size_t total_len = num_len + pfx_len;
      if (zero_pad) {
        if (pfx_len) {
          snprintf_putc(&ctx, '0');
          snprintf_putc(&ctx, spec);
        }
        if (width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, '0');
        }
        for (size_t p = 0; p < num_len; p++) snprintf_putc(&ctx, num_buf[num_len - 1 - p]);
      } else {
        if (!left_align && width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, ' ');
        }
        if (pfx_len) {
          snprintf_putc(&ctx, '0');
          snprintf_putc(&ctx, spec);
        }
        for (size_t p = 0; p < num_len; p++) snprintf_putc(&ctx, num_buf[num_len - 1 - p]);
        if (left_align && width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, ' ');
        }
      }
    } else if (spec == 'p') {
      void *ptr = va_arg(args, void *);
      if (ptr == NULL) {
        const char *nil_str = "(nil)";
        len = 5;
        snprintf_puts(&ctx, nil_str, len, left_align, width, ' ');
      } else {
        uint64_t uval = (uint64_t)(uintptr_t)ptr;
        static const char hex_lower[] = "0123456789abcdef";
        char num_buf[32];
        size_t num_len = 0;
        while (uval > 0 && num_len < sizeof(num_buf)) {
          num_buf[num_len++] = hex_lower[uval & 0xf];
          uval >>= 4;
        }
        size_t total_len = num_len + 2; /* "0x" */
        if (!left_align && width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, ' ');
        }
        snprintf_putc(&ctx, '0');
        snprintf_putc(&ctx, 'x');
        for (size_t p = 0; p < num_len; p++) snprintf_putc(&ctx, num_buf[num_len - 1 - p]);
        if (left_align && width > total_len) {
          for (size_t p = 0; p < width - total_len; p++) snprintf_putc(&ctx, ' ');
        }
      }
    } else {
      /* Unknown specifier: emit verbatim */
      snprintf_putc(&ctx, '%');
      snprintf_putc(&ctx, spec);
    }
  }

  if (buf != NULL && size > 0) {
    if (ctx.written < size) {
      buf[ctx.written] = '\0';
    } else {
      buf[size - 1] = '\0';
    }
  }

  return (int)ctx.written;
}

int oops_snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = oops_vsnprintf(buf, size, fmt, args);
  va_end(args);
  return ret;
}
