/*
 * oops-sdk: Freestanding ZIP archive extractor and Deflate inflator.
 *
 * Implements PKWARE ZIP container parsing and RFC 1951 Deflate decompression
 * entirely freestanding without libc dependencies. Files and directories are
 * written through oops/fs.h.
 */

#include "oops/zip.h"
#include "oops/fs.h"
#include "oops/freestd.h"
#include "oops/system.h"

#ifndef OOPS_HOST_BUILD
#include "oops/heap.h"
#define zip_alloc(sz) oops_malloc(sz)
#define zip_free(p) oops_free(p)
#else
#include <stdlib.h>
#define zip_alloc(sz) malloc(sz)
#define zip_free(p) free(p)
#endif

/* ---------------------------------------------------------------------------
 * RFC 1951 DEFLATE Inflator (Puff algorithm by Mark Adler, public domain)
 * --------------------------------------------------------------------------- */

#define PUFF_MAXBITS 15
#define PUFF_MAXLCODES 286
#define PUFF_MAXDCODES 30
#define PUFF_MAXCODES (PUFF_MAXLCODES + PUFF_MAXDCODES)
#define PUFF_FIXLCODES 288

typedef struct {
    unsigned char *out;
    unsigned long outlen;
    unsigned long outcnt;

    const unsigned char *in;
    unsigned long inlen;
    unsigned long incnt;
    int bitbuf;
    int bitcnt;
} puff_state_t;

typedef struct {
    short count[PUFF_MAXBITS + 1];
    short symbol[PUFF_MAXCODES];
} puff_huffman_t;

static int puff_bits(puff_state_t *s, int need) {
    int val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen)
            return -1; /* Out of input */
        val |= (int)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1 << need) - 1);
}

static int puff_decode(puff_state_t *s, const puff_huffman_t *h) {
    int len;
    int code = 0;
    int first = 0;
    int index = 0;

    for (len = 1; len <= PUFF_MAXBITS; len++) {
        int b = puff_bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10; /* Invalid Huffman code */
}

static int puff_build_huffman(puff_huffman_t *h, const short *lengths, int n) {
    int len;
    int symbol;
    short offsets[PUFF_MAXBITS + 1];

    for (len = 0; len <= PUFF_MAXBITS; len++)
        h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) {
        if (lengths[symbol] < 0 || lengths[symbol] > PUFF_MAXBITS)
            return -1;
        h->count[lengths[symbol]]++;
    }
    if (h->count[0] == n)
        return 0; /* Complete empty table */

    int left = 1;
    for (len = 1; len <= PUFF_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -10; /* Over-subscribed */
    }

    offsets[1] = 0;
    for (len = 1; len < PUFF_MAXBITS; len++) {
        offsets[len + 1] = (short)(offsets[len] + h->count[len]);
    }

    for (symbol = 0; symbol < n; symbol++) {
        if (lengths[symbol] != 0) {
            h->symbol[offsets[lengths[symbol]]++] = (short)symbol;
        }
    }
    return 0;
}

static int puff_stored(puff_state_t *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;

    if (s->incnt + 4 > s->inlen)
        return -2;
    unsigned int len =
        (unsigned int)s->in[s->incnt] | ((unsigned int)s->in[s->incnt + 1] << 8);
    s->incnt += 2;
    unsigned int nlen =
        (unsigned int)s->in[s->incnt] | ((unsigned int)s->in[s->incnt + 1] << 8);
    s->incnt += 2;

    if (len != (unsigned int)(~nlen & 0xffff))
        return -2;
    if (s->incnt + len > s->inlen)
        return -2;
    if (s->outcnt + len > s->outlen)
        return -1;

    for (unsigned int i = 0; i < len; i++) {
        s->out[s->outcnt++] = s->in[s->incnt++];
    }
    return 0;
}

static int puff_codes(puff_state_t *s, const puff_huffman_t *lencode,
                      const puff_huffman_t *distcode) {
    static const short lens[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                   15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                   67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1,    2,    3,    4,     5,     7,    9,    13,
                                    17,   25,   33,   49,    65,    97,   129,  193,
                                    257,  385,  513,  769,   1025,  1537, 2049, 3073,
                                    4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        int symbol = puff_decode(s, lencode);
        if (symbol < 0)
            return symbol;
        if (symbol < 256) {
            if (s->outcnt >= s->outlen)
                return -1;
            s->out[s->outcnt++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            break; /* End of block */
        } else {
            symbol -= 257;
            if (symbol >= 29)
                return -10;
            int len = lens[symbol];
            if (lext[symbol]) {
                int extra = puff_bits(s, lext[symbol]);
                if (extra < 0)
                    return extra;
                len += extra;
            }
            int dsym = puff_decode(s, distcode);
            if (dsym < 0)
                return dsym;
            if (dsym >= 30)
                return -10;
            int dist = dists[dsym];
            if (dext[dsym]) {
                int extra = puff_bits(s, dext[dsym]);
                if (extra < 0)
                    return extra;
                dist += extra;
            }
            if ((unsigned long)dist > s->outcnt)
                return -11; /* Distance too far back */
            if (s->outcnt + (unsigned long)len > s->outlen)
                return -1;
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - (unsigned long)dist];
                s->outcnt++;
            }
        }
    }
    return 0;
}

static int puff_fixed(puff_state_t *s) {
    static int virgin = 1;
    static puff_huffman_t lencode, distcode;

    if (virgin) {
        short lengths[PUFF_FIXLCODES];
        int symbol;
        for (symbol = 0; symbol < 144; symbol++)
            lengths[symbol] = 8;
        for (; symbol < 256; symbol++)
            lengths[symbol] = 9;
        for (; symbol < 280; symbol++)
            lengths[symbol] = 7;
        for (; symbol < PUFF_FIXLCODES; symbol++)
            lengths[symbol] = 8;
        puff_build_huffman(&lencode, lengths, PUFF_FIXLCODES);

        for (symbol = 0; symbol < PUFF_MAXDCODES; symbol++)
            lengths[symbol] = 5;
        puff_build_huffman(&distcode, lengths, PUFF_MAXDCODES);
        virgin = 0;
    }
    return puff_codes(s, &lencode, &distcode);
}

static int puff_dynamic(puff_state_t *s) {
    static const short order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                    11, 4,  12, 3, 13, 2, 14, 1, 15};
    int nlen = puff_bits(s, 5);
    int ndist = puff_bits(s, 5);
    int ncode = puff_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > PUFF_MAXLCODES || ndist > PUFF_MAXDCODES)
        return -10;

    short lengths[PUFF_MAXCODES];
    for (int index = 0; index < 19; index++)
        lengths[index] = 0;
    for (int index = 0; index < ncode; index++) {
        int b = puff_bits(s, 3);
        if (b < 0)
            return b;
        lengths[order[index]] = (short)b;
    }

    puff_huffman_t codec;
    int err = puff_build_huffman(&codec, lengths, 19);
    if (err)
        return err;

    int index = 0;
    while (index < nlen + ndist) {
        int symbol = puff_decode(s, &codec);
        if (symbol < 0)
            return symbol;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            int len = 0;
            if (symbol == 16) {
                if (index == 0)
                    return -10;
                len = lengths[index - 1];
                int count = puff_bits(s, 2);
                if (count < 0)
                    return count;
                count += 3;
                while (count--) {
                    if (index >= nlen + ndist)
                        return -10;
                    lengths[index++] = (short)len;
                }
            } else if (symbol == 17) {
                int count = puff_bits(s, 3);
                if (count < 0)
                    return count;
                count += 3;
                while (count--) {
                    if (index >= nlen + ndist)
                        return -10;
                    lengths[index++] = 0;
                }
            } else {
                int count = puff_bits(s, 7);
                if (count < 0)
                    return count;
                count += 11;
                while (count--) {
                    if (index >= nlen + ndist)
                        return -10;
                    lengths[index++] = 0;
                }
            }
        }
    }

    puff_huffman_t lencode, distcode;
    err = puff_build_huffman(&lencode, lengths, nlen);
    if (err)
        return err;
    err = puff_build_huffman(&distcode, lengths + nlen, ndist);
    if (err)
        return err;

    return puff_codes(s, &lencode, &distcode);
}

static int oops_inflate(unsigned char *dest, unsigned long *destlen,
                        const unsigned char *source, unsigned long *sourcelen) {
    puff_state_t s;
    s.out = dest;
    s.outlen = *destlen;
    s.outcnt = 0;
    s.in = source;
    s.inlen = *sourcelen;
    s.incnt = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;

    int last;
    int err = 0;
    do {
        last = puff_bits(&s, 1);
        int type = puff_bits(&s, 2);
        if (last < 0 || type < 0) {
            err = -1;
            break;
        }
        switch (type) {
        case 0:
            err = puff_stored(&s);
            break;
        case 1:
            err = puff_fixed(&s);
            break;
        case 2:
            err = puff_dynamic(&s);
            break;
        default:
            err = -10;
            break;
        }
        if (err != 0)
            break;
    } while (!last);

    *destlen = s.outcnt;
    *sourcelen = s.incnt;
    return err;
}

/* ---------------------------------------------------------------------------
 * ZIP Container Helpers
 * --------------------------------------------------------------------------- */

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                      ((uint32_t)p[3] << 24));
}

/* Recursive directory creator */
static int mkdir_recursive(const char *dir_path) {
    if (!dir_path || !*dir_path)
        return 0;
    char path[512];
    size_t len = obs_strlen(dir_path);
    if (len >= sizeof(path))
        return -1;
    obs_strncpy(path, dir_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    for (size_t i = 1; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            char sep = path[i];
            path[i] = '\0';
            if (path[0] != '\0' && !oops_fs_exists(path)) {
                (void)oops_fs_mkdir(path, 0755);
            }
            path[i] = sep;
        }
    }
    if (!oops_fs_exists(path)) {
        (void)oops_fs_mkdir(path, 0755);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public ZIP Extractor API
 * --------------------------------------------------------------------------- */

int oops_zip_extract_mem(const void *zip_data, size_t zip_size, const char *dest_dir) {
    if (!zip_data || zip_size < 22 || !dest_dir || !*dest_dir) {
        return OOPS_ZIP_ERR_PARAM;
    }

    const uint8_t *data = (const uint8_t *)zip_data;

    /* Find End of Central Directory (EOCD) signature 0x06054b50 */
    size_t max_search = (zip_size > 65557) ? 65557 : zip_size;
    const uint8_t *eocd = NULL;

    for (size_t i = 22; i <= max_search; i++) {
        const uint8_t *p = data + zip_size - i;
        if (read_u32_le(p) == 0x06054b50) {
            eocd = p;
            break;
        }
    }

    if (!eocd) {
        return OOPS_ZIP_ERR_BAD_HEADER;
    }

    uint16_t total_entries = read_u16_le(eocd + 10);
    uint32_t cd_size = read_u32_le(eocd + 12);
    uint32_t cd_offset = read_u32_le(eocd + 16);

    if ((size_t)cd_offset + (size_t)cd_size > zip_size) {
        return OOPS_ZIP_ERR_BAD_HEADER;
    }

    /* Ensure base destination directory exists */
    mkdir_recursive(dest_dir);
    oops_log_info("ZIP", "extracting archive (%zu bytes) with %u entries to '%s'",
                  zip_size, total_entries, dest_dir);

    const uint8_t *cd_ptr = data + cd_offset;

    for (uint16_t entry = 0; entry < total_entries; entry++) {
        if (cd_ptr + 46 > data + zip_size) {
            oops_log_warn("ZIP", "entry %u exceeds archive boundary", entry);
            return OOPS_ZIP_ERR_BAD_HEADER;
        }
        if (read_u32_le(cd_ptr) != 0x02014b50) {
            oops_log_warn("ZIP", "entry %u bad magic: 0x%08x", entry,
                          read_u32_le(cd_ptr));
            return OOPS_ZIP_ERR_BAD_HEADER;
        }

        uint16_t method = read_u16_le(cd_ptr + 10);
        uint32_t comp_size = read_u32_le(cd_ptr + 20);
        uint32_t uncomp_size = read_u32_le(cd_ptr + 24);
        uint16_t fname_len = read_u16_le(cd_ptr + 28);
        uint16_t extra_len = read_u16_le(cd_ptr + 30);
        uint16_t comment_len = read_u16_le(cd_ptr + 32);
        uint32_t local_hdr_offset = read_u32_le(cd_ptr + 42);

        if (cd_ptr + 46 + fname_len + extra_len + comment_len > data + zip_size) {
            return OOPS_ZIP_ERR_BAD_HEADER;
        }

        char fname[256];
        if (fname_len >= sizeof(fname)) {
            return OOPS_ZIP_ERR_PARAM;
        }
        for (uint16_t i = 0; i < fname_len; i++) {
            fname[i] = (char)cd_ptr[46 + i];
        }
        fname[fname_len] = '\0';
        oops_log_debug("ZIP", "entry %u/%u: '%s' (method=%u, comp=%u, uncomp=%u)",
                       entry + 1, total_entries, fname, method, comp_size, uncomp_size);

        /* Sanitize filename: reject path traversal */
        if (fname[0] == '/' || fname[0] == '\\' || obs_strstr(fname, "../") ||
            obs_strstr(fname, "..\\")) {
            return OOPS_ZIP_ERR_PARAM;
        }

        /* Build target path */
        char target_path[512];
        size_t dlen = obs_strlen(dest_dir);
        if (dlen + 1 + (size_t)fname_len >= sizeof(target_path)) {
            return OOPS_ZIP_ERR_PARAM;
        }
        obs_strncpy(target_path, dest_dir, sizeof(target_path) - 1);
        if (dlen > 0 && target_path[dlen - 1] != '/' && target_path[dlen - 1] != '\\') {
            target_path[dlen++] = '/';
        }
        obs_strncpy(target_path + dlen, fname, sizeof(target_path) - dlen - 1);
        target_path[sizeof(target_path) - 1] = '\0';

        /* Is it a directory? */
        int is_dir = (fname[fname_len - 1] == '/' || fname[fname_len - 1] == '\\');

        if (is_dir) {
            mkdir_recursive(target_path);
        } else {
            /* Ensure parent directory exists */
            char parent[512];
            obs_strncpy(parent, target_path, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            char *last_slash = (char *)0;
            for (char *c = parent; *c; c++) {
                if (*c == '/' || *c == '\\')
                    last_slash = c;
            }
            if (last_slash) {
                *last_slash = '\0';
                mkdir_recursive(parent);
            }

            /* Read local header */
            if ((size_t)local_hdr_offset + 30 > zip_size) {
                return OOPS_ZIP_ERR_BAD_HEADER;
            }
            const uint8_t *loc = data + local_hdr_offset;
            if (read_u32_le(loc) != 0x04034b50) {
                return OOPS_ZIP_ERR_BAD_HEADER;
            }
            uint16_t loc_fname_len = read_u16_le(loc + 26);
            uint16_t loc_extra_len = read_u16_le(loc + 28);
            const uint8_t *payload = loc + 30 + loc_fname_len + loc_extra_len;

            if (payload + comp_size > data + zip_size) {
                return OOPS_ZIP_ERR_BAD_HEADER;
            }

            if (method == 0) {
                /* STORED */
                if (comp_size != uncomp_size) {
                    return OOPS_ZIP_ERR_BAD_HEADER;
                }
                /* 0755, not 0644: an installed homebrew title's eboot.bin (and its .prx
                 * modules) must carry the execute bit or the console refuses to spawn
                 * the process (EACCES). A title .zip stores its files 0644, so the
                 * extractor grants execute here; the bit is harmless on the data files
                 * that share the tree. */
                int fd = oops_fs_open(
                    target_path, OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC, 0755);
                if (fd < 0)
                    return OOPS_ZIP_ERR_WRITE;
                if (comp_size > 0) {
                    int64_t w = oops_fs_write(fd, payload, comp_size);
                    oops_fs_close(fd);
                    if (w != (int64_t)comp_size)
                        return OOPS_ZIP_ERR_WRITE;
                } else {
                    oops_fs_close(fd);
                }
            } else if (method == 8) {
                /* DEFLATED */
                uint8_t *uncomp_buf = NULL;
                if (uncomp_size > 0) {
                    uncomp_buf = (uint8_t *)zip_alloc(uncomp_size);
                    if (!uncomp_buf)
                        return OOPS_ZIP_ERR_NOMEM;
                }

                unsigned long dest_len = uncomp_size;
                unsigned long src_len = comp_size;
                int ret = oops_inflate(uncomp_buf, &dest_len, payload, &src_len);
                if (ret != 0 || dest_len != uncomp_size) {
                    if (uncomp_buf)
                        zip_free(uncomp_buf);
                    return OOPS_ZIP_ERR_DECOMPRESS;
                }

                /* 0755 for the same reason as the STORED branch above: the eboot must
                 * be executable. */
                int fd = oops_fs_open(
                    target_path, OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC, 0755);
                if (fd < 0) {
                    if (uncomp_buf)
                        zip_free(uncomp_buf);
                    return OOPS_ZIP_ERR_WRITE;
                }
                if (uncomp_size > 0) {
                    int64_t w = oops_fs_write(fd, uncomp_buf, uncomp_size);
                    oops_fs_close(fd);
                    zip_free(uncomp_buf);
                    if (w != (int64_t)uncomp_size)
                        return OOPS_ZIP_ERR_WRITE;
                } else {
                    oops_fs_close(fd);
                }
            } else {
                return OOPS_ZIP_ERR_UNSUPPORTED;
            }
            /* Belt to the 0755 passed at open, which the kernel ignores when the file
             * already exists: enforce the execute bit so a re-installed title's eboot
             * stays runnable. */
            (void)oops_fs_chmod(target_path, 0755);
        }

        cd_ptr += 46 + fname_len + extra_len + comment_len;
    }

    return OOPS_ZIP_OK;
}

int oops_zip_extract(const char *zip_path, const char *dest_dir) {
    if (!zip_path || !*zip_path || !dest_dir || !*dest_dir) {
        return OOPS_ZIP_ERR_PARAM;
    }

    oops_log_info("ZIP", "extracting archive '%s' to '%s'", zip_path, dest_dir);
    void *data = NULL;
    size_t size = 0;
    if (oops_fs_read_all(zip_path, &data, &size) != 0 || !data) {
        oops_log_warn("ZIP", "archive '%s' not found or unreadable", zip_path);
        return OOPS_ZIP_ERR_NOT_FOUND;
    }

    int rc = oops_zip_extract_mem(data, size, dest_dir);
    oops_fs_free_data(data);
    if (rc == OOPS_ZIP_OK) {
        oops_log_info("ZIP", "extraction of '%s' completed successfully", zip_path);
    } else {
        oops_log_warn("ZIP", "extraction of '%s' failed rc=%d", zip_path, rc);
    }
    return rc;
}
