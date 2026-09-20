/*
 * rx-check: whether the colour block draws 64KB_R_X exactly as the display scans it out, read
 * from obSCEne's REQ-20260919T1927Z-7e21 rows.
 *
 * # The question
 *
 * oops-gl can draw straight into the display's scanout buffers - as a title on this console
 * does - only if the colour block, told COLOR_SW_MODE 27 (ADDR_SW_64KB_R_X), writes the same
 * layout the display tiler produces and VideoOut scans. The display tiler's layout is
 * hardware-verified, and include/agc/tiler.h addresses it pixel by pixel (agc_tile_pixel). The
 * colour block's side is REQ-7e21: one 128 x 128 block drawn under each candidate
 * CB_COLOR0_ATTRIB3 value, and the same draw into a linear target as the control, every byte
 * of each dumped as `OBS|bytes|<check>|<arm>|<key>|<offset>|<hex>` rows.
 *
 * # What is checked
 *
 * 1. That each surface's dump is complete.
 * 2. That the control drew something, and not everything: a triangle over the 0x55555555 fill.
 * 3. For each tiled arm, **every pixel detiled through agc_tile_pixel equals the control's**,
 *    and the same bytes read as rows do *not* - so the comparison can tell the two layouts
 *    apart in this picture, rather than agreeing because the picture cannot show a difference.
 *
 * The verdict names the CB_COLOR0_ATTRIB3 value of the first arm that passes. That value, and
 * OOPS_GL_RX_MEASURED 1, are what oops-gl's src/gl/gl_rx.h takes from it; the output is tracked
 * beside this file as rx_check_7e21.txt once the rows exist.
 *
 * `--self-test` writes rows in obSCEne's format for a synthetic triangle - the control linear,
 * one arm tiled right, one arm written as rows - and requires the verdict to pick the right one,
 * and an incomplete dump to be reported as such, before real rows are trusted to it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agc/tiler.h"

#define FILL 0x55555555u
#define MAX_ARMS 8

typedef struct {
    const char *name;
    uint32_t attrib3; /* 0 for the control */
    int tiled;
    uint8_t *bytes;
    uint8_t *seen;
    size_t size;
} surface_t;

typedef struct {
    const char *check;
    const char *key;
    uint32_t w, h;
    surface_t control;
    surface_t arms[MAX_ARMS];
    int n_arms;
} job_t;

static size_t tiled_size(uint32_t w, uint32_t h) {
    return (size_t)((w + 127u) / 128u) * (size_t)((h + 127u) / 128u) * 65536u;
}

static size_t tiled_offset(uint32_t w, uint32_t x, uint32_t y) {
    const size_t block = (size_t)(y >> 7) * (size_t)((w + 127u) / 128u) + (size_t)(x >> 7);
    return block * 65536u + (size_t)agc_tile_pixel(x & 127u, y & 127u) * 4u;
}

static int surface_init(surface_t *s, const char *name, uint32_t attrib3, int tiled, size_t size) {
    s->name = name;
    s->attrib3 = attrib3;
    s->tiled = tiled;
    s->size = size;
    s->bytes = (uint8_t *)calloc(size, 1);
    s->seen = (uint8_t *)calloc(size, 1);
    return s->bytes && s->seen ? 0 : -1;
}

static uint32_t at32(const surface_t *s, size_t o) {
    if (o + 4u > s->size) return FILL;
    return (uint32_t)s->bytes[o] | (uint32_t)s->bytes[o + 1] << 8 |
           (uint32_t)s->bytes[o + 2] << 16 | (uint32_t)s->bytes[o + 3] << 24;
}

static uint32_t px(const surface_t *s, uint32_t w, uint32_t x, uint32_t y, int as_tiled) {
    return at32(s, as_tiled ? tiled_offset(w, x, y) : ((size_t)y * w + x) * 4u);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The count the check itself reports, for the shape mode to agree with: an independent figure,
 * since it comes from the probe's own sweep of the target rather than from these rows. */
static uint32_t s_modified[MAX_ARMS + 1];
static const char *s_modified_of[MAX_ARMS + 1];
static int s_modified_n;

/* `OBS|bytes|<check>|<arm>|<key>|<offset>|<hex>`, the way 166-agc/primitive-draw writes them. */
static void take_row(job_t *j, char *line) {
    char *f[8];
    int n = 0;
    for (char *p = line; n < 8;) {
        f[n++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = '\0';
        p = bar + 1;
    }
    if (n < 7 || strcmp(f[0], "OBS") != 0) return;
    if (strcmp(f[1], "measure") == 0 && strcmp(f[2], j->check) == 0 &&
        strcmp(f[4], "modified-pixels") == 0 && s_modified_n <= MAX_ARMS) {
        /* Kept by arm name, for the shape mode to check its own count against. */
        for (int a = 0; a < s_modified_n; a++) {
            if (s_modified_of[a] && strcmp(s_modified_of[a], f[3]) == 0) return;
        }
        static char names[MAX_ARMS + 1][64];
        size_t len = strlen(f[3]);
        if (len > 63u) len = 63u;
        memcpy(names[s_modified_n], f[3], len);
        names[s_modified_n][len] = '\0';
        s_modified_of[s_modified_n] = names[s_modified_n];
        s_modified[s_modified_n++] = (uint32_t)strtoul(f[5], NULL, 0);
        return;
    }
    if (strcmp(f[1], "bytes") != 0) return;
    if (strcmp(f[2], j->check) != 0 || strcmp(f[4], j->key) != 0) return;
    surface_t *s = NULL;
    if (strcmp(f[3], j->control.name) == 0) s = &j->control;
    for (int a = 0; a < j->n_arms && !s; a++) {
        if (strcmp(f[3], j->arms[a].name) == 0) s = &j->arms[a];
    }
    if (!s) return;
    const size_t off = (size_t)strtoull(f[5], NULL, 10);
    const char *hx = f[6];
    for (size_t i = 0; hx[2 * i] && hx[2 * i + 1]; i++) {
        const int hi = hexval(hx[2 * i]), lo = hexval(hx[2 * i + 1]);
        if (hi < 0 || lo < 0) break;
        if (off + i >= s->size) break;
        s->bytes[off + i] = (uint8_t)(hi << 4 | lo);
        s->seen[off + i] = 1;
    }
}

/*
 * The block order read out of one tiled dump, with no control to compare against.
 *
 * `-4b19`'s 256 x 256 arms are tiled only: there is no linear image of the same draw to detile
 * against, so block_map() has nothing to match. What there is instead is the *shape*. The fixture
 * draws one flat-colour triangle, and a triangle has exactly one run of drawn pixels in every row
 * it touches. Detile the dump with the wrong block order and the quadrants are shuffled: rows
 * acquire a second run where a block boundary falls, and the count collapses.
 *
 * So every block order is tried and the clean rows counted. This decides nothing by assumption -
 * it is decisive only if the order the tool uses is the *one* order that produces a triangle,
 * which is what the verdict says.
 */
static uint32_t at_block(const surface_t *s, uint32_t blk, uint32_t x, uint32_t y) {
    return at32(s, (size_t)blk * 65536u + (size_t)agc_tile_pixel(x & 127u, y & 127u) * 4u);
}

/*
 * Rows of the detiled image holding exactly one run, the pixels drawn, and the gaps - empty rows
 * with drawn rows above *and* below them - under `perm`: perm[i] is the dump block holding
 * picture block i.
 *
 * The gap count is what tells block orders apart vertically. An order that swaps whole rows of
 * blocks moves entire image rows without splitting any of them, so every row still holds one run
 * and "one run a row" says nothing; but a triangle's rows are contiguous, and moving half of them
 * to the other end of the image leaves a hole in the middle.
 */
static void shape_of(const job_t *j, const surface_t *s, const uint32_t *perm, uint32_t *clean,
                     uint32_t *drawn, uint32_t *touched, uint32_t *gaps) {
    const uint32_t nbx = (j->w + 127u) / 128u;
    *clean = 0;
    *drawn = 0;
    *touched = 0;
    *gaps = 0;
    uint32_t seen_any = 0, empty_since = 0;
    for (uint32_t y = 0; y < j->h; y++) {
        uint32_t runs = 0, in_run = 0, row_drawn = 0;
        for (uint32_t x = 0; x < j->w; x++) {
            const uint32_t pic = (y >> 7) * nbx + (x >> 7);
            const uint32_t v = at_block(s, perm[pic], x, y);
            const uint32_t on = (v != FILL);
            if (on && !in_run) runs++;
            in_run = on;
            row_drawn += on;
        }
        *drawn += row_drawn;
        if (row_drawn) {
            (*touched)++;
            if (runs == 1u) (*clean)++;
            if (seen_any && empty_since) (*gaps)++;
            seen_any = 1;
            empty_since = 0;
        } else if (seen_any) {
            empty_since = 1;
        }
    }
}

/* Every permutation of the blocks, for a surface small enough to enumerate them. Returns the
 * number that give a clean shape, and writes the first one into `found`. */
static int shape_search(const job_t *j, const surface_t *s, FILE *out, uint32_t *found) {
    const uint32_t nb = ((j->w + 127u) / 128u) * ((j->h + 127u) / 128u);
    if (nb == 0u || nb > 4u) {
        fprintf(out, "  block order: %u blocks is too many to enumerate; only the tool's own "
                     "order is checked\n", nb);
    }
    uint32_t perm[4] = {0, 1, 2, 3};
    uint32_t idx[4] = {0, 1, 2, 3};
    int clean_orders = 0;
    const uint32_t limit = (nb <= 4u) ? nb : 1u;
    /* Heap's algorithm, iterative, for at most four elements. */
    uint32_t c[4] = {0, 0, 0, 0};
    uint32_t i = 0;
    for (;;) {
        uint32_t clean = 0, drawn = 0, touched = 0, gaps = 0;
        shape_of(j, s, perm, &clean, &drawn, &touched, &gaps);
        const int is_identity = (perm[0] == 0u && (limit < 2u || perm[1] == 1u) &&
                                 (limit < 3u || perm[2] == 2u) && (limit < 4u || perm[3] == 3u));
        if (touched && clean == touched && gaps == 0u) {
            clean_orders++;
            if (found && clean_orders == 1) {
                for (uint32_t k = 0; k < limit; k++) found[k] = perm[k];
            }
            fprintf(out, "  block order %u %u %u %u: %u of %u rows are one run, no gaps, "
                         "%u pixels%s\n",
                    perm[0], perm[1], perm[2], perm[3], clean, touched, drawn,
                    is_identity ? "  <- the tool's own order" : "");
        }
        if (limit < 2u) break;
        /* Next permutation. */
        while (i < limit && c[i] >= i) { c[i] = 0; i++; }
        if (i >= limit) break;
        const uint32_t a = (i % 2u) ? c[i] : 0u;
        const uint32_t t = perm[a];
        perm[a] = perm[i];
        perm[i] = t;
        c[i]++;
        i = 0;
        (void)idx;
    }
    return clean_orders;
}

static size_t coverage(const surface_t *s) {
    size_t c = 0;
    for (size_t i = 0; i < s->size; i++) c += s->seen[i];
    return c;
}

/*
 * Which 64 KiB block of a tiled dump holds which 128 x 128 block of the picture.
 *
 * tiled_offset() lays the blocks out row-major across the surface, because that is what
 * addrlib computes for 64KB_R_X with pipeBankXor 0 - not because any row has shown it. Past
 * one block that assumption is the whole answer, so this reads the order back out of the dump:
 * each dumped block is compared against every block of the linear control, and the one it
 * equals everywhere is the picture block it holds. A block whose picture is all fill matches
 * anything, so it is reported with a `?` rather than counted as agreement.
 *
 * It decides nothing - the pixel comparison in judge() already fails an arm whose blocks are
 * out of order. It says *how* they are out of order, which is the difference between a
 * re-file and a fix.
 */
static void block_map(const job_t *j, const surface_t *s, FILE *out) {
    const uint32_t nbx = (j->w + 127u) / 128u, nby = (j->h + 127u) / 128u;
    int row_major = 1, undecided = 0;
    fprintf(out, "  block map (dump block -> picture block):");
    for (uint32_t d = 0; d < nbx * nby; d++) {
        int best = -1, ties = 0;
        uint32_t best_drawn = 0;
        for (uint32_t sb = 0; sb < nbx * nby; sb++) {
            const uint32_t sbx = sb % nbx, sby = sb / nbx;
            uint32_t cmp = 0, eq = 0, drawn = 0;
            for (uint32_t y = 0; y < 128u; y++) {
                for (uint32_t x = 0; x < 128u; x++) {
                    const uint32_t gx = sbx * 128u + x, gy = sby * 128u + y;
                    if (gx >= j->w || gy >= j->h) continue;
                    const uint32_t want = px(&j->control, j->w, gx, gy, 0);
                    cmp++;
                    eq += at32(s, (size_t)d * 65536u + (size_t)agc_tile_pixel(x, y) * 4u) == want;
                    drawn += want != FILL;
                }
            }
            if (cmp && eq == cmp) {
                if (best < 0) {
                    best = (int)sb;
                    best_drawn = drawn;
                }
                ties++;
            }
        }
        if (best < 0) {
            fprintf(out, " %u->none", d);
            undecided = 1;
            row_major = 0;
            continue;
        }
        fprintf(out, " %u->(%u,%u)%s", d, (uint32_t)best % nbx, (uint32_t)best / nbx,
                (ties > 1 || best_drawn == 0) ? "?" : "");
        if (ties > 1 || best_drawn == 0) undecided = 1;
        if ((uint32_t)best != d) row_major = 0;
    }
    fprintf(out, " [%s]\n",
            undecided ? "undecided - a block holds no drawn pixel of its own"
                      : row_major ? "row-major, as the tool assumes"
                                  : "NOT row-major - the tool's block order is wrong");
}

/* The report. Returns 0 with a verdict, 1 when an arm matched nothing, 2 when the rows cannot
 * decide (a dump incomplete, a control that drew nothing or everything). */
static int judge(job_t *j, FILE *out, uint32_t *chosen) {
    int missing = 0;
    const size_t cc = coverage(&j->control);
    fprintf(out, "control %s: %zu of %zu bytes dumped\n", j->control.name, cc, j->control.size);
    if (cc != j->control.size) missing = 1;
    uint32_t drawn = 0;
    for (uint32_t y = 0; y < j->h; y++) {
        for (uint32_t x = 0; x < j->w; x++) drawn += px(&j->control, j->w, x, y, 0) != FILL;
    }
    fprintf(out, "control %s: %u of %u pixels drawn over the fill\n", j->control.name, drawn,
            j->w * j->h);
    /* An eighth of the block at least: a layout is checked by where many pixels land, and the
     * 2026-09-16 run of this check drew one. */
    if (drawn < j->w * j->h / 8u || drawn == j->w * j->h) missing = 1;

    int verdict = -1;
    for (int a = 0; a < j->n_arms; a++) {
        surface_t *s = &j->arms[a];
        const size_t c = coverage(s);
        uint32_t tiled = 0, rows = 0;
        for (uint32_t y = 0; y < j->h; y++) {
            for (uint32_t x = 0; x < j->w; x++) {
                const uint32_t want = px(&j->control, j->w, x, y, 0);
                tiled += px(s, j->w, x, y, 1) == want;
                rows += px(s, j->w, x, y, 0) == want;
            }
        }
        const int complete = c == s->size;
        const int pass = complete && !missing && tiled == j->w * j->h && rows != j->w * j->h;
        fprintf(out,
                "%s (CB_COLOR0_ATTRIB3 0x%08x): %zu of %zu bytes dumped; detiled, %u of %u "
                "pixels equal the control; read as rows, %u; %s\n",
                s->name, s->attrib3, c, s->size, tiled, j->w * j->h, rows,
                !complete ? "INCOMPLETE" : pass ? "MATCHES" : "does not match");
        if (tiled_size(j->w, j->h) > 65536u) block_map(j, s, out);
        if (!complete) missing = 1;
        if (pass && verdict < 0) verdict = a;
    }
    if (j->n_arms == 0) {
        fprintf(out, "VERDICT: control only - no arm asked for\n");
        return missing ? 2 : 0;
    }
    if (missing) {
        fprintf(out, "VERDICT: undecided - a dump is incomplete, or the control drew too little "
                     "(under an eighth of the block) or everything\n");
        return 2;
    }
    if (verdict < 0) {
        fprintf(out, "VERDICT: no arm draws the display's layout\n");
        return 1;
    }
    fprintf(out, "VERDICT: the colour block draws 64KB_R_X as the display scans it; "
                 "CB_COLOR0_ATTRIB3 0x%08x (%s)\n",
            j->arms[verdict].attrib3, j->arms[verdict].name);
    if (chosen) *chosen = j->arms[verdict].attrib3;
    return 0;
}

static void read_rows(job_t *j, FILE *in) {
    static char line[4096];
    while (fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = '\0';
        take_row(j, line);
    }
}

static int job_init(job_t *j, const char *check, const char *key, uint32_t w, uint32_t h,
                    const char *control) {
    memset(j, 0, sizeof(*j));
    j->check = check;
    j->key = key;
    j->w = w;
    j->h = h;
    return surface_init(&j->control, control, 0u, 0, (size_t)w * h * 4u);
}

static int job_arm(job_t *j, const char *name, uint32_t attrib3) {
    if (j->n_arms >= MAX_ARMS) return -1;
    return surface_init(&j->arms[j->n_arms++], name, attrib3, 1, tiled_size(j->w, j->h));
}

static void job_free(job_t *j) {
    free(j->control.bytes);
    free(j->control.seen);
    for (int a = 0; a < j->n_arms; a++) {
        free(j->arms[a].bytes);
        free(j->arms[a].seen);
    }
}

/* Rows in obSCEne's own format, 16 bytes a row, for a surface of `size` bytes. */
static void emit_rows(FILE *f, const char *check, const char *arm, const char *key,
                      const uint8_t *bytes, size_t size, size_t skip_from) {
    for (size_t off = 0; off < size; off += 16) {
        if (off >= skip_from) break;
        fprintf(f, "OBS|bytes|%s|%s|%s|%zu|", check, arm, key, off);
        for (size_t i = 0; i < 16 && off + i < size; i++) fprintf(f, "%02x", bytes[off + i]);
        fputc('\n', f);
    }
}

static void put32(uint8_t *b, size_t o, uint32_t v) {
    b[o] = (uint8_t)v;
    b[o + 1] = (uint8_t)(v >> 8);
    b[o + 2] = (uint8_t)(v >> 16);
    b[o + 3] = (uint8_t)(v >> 24);
}

static int self_test_one_block(void) {
    enum { W = 128, H = 128 };
    const char *check = "166-agc/primitive-draw";
    static uint8_t lin[W * H * 4], tiled[65536];
    /* A triangle - no symmetry that could make rows and tiles agree - over the fill. */
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            const int in = x >= 10 && y >= 20 && (x - 10) * 2 < (y - 20) * 3 && x < 110;
            const uint32_t v = in ? 0xff0000ffu : FILL;
            put32(lin, ((size_t)y * W + x) * 4u, v);
            put32(tiled, tiled_offset(W, x, y) * 1u, v);
        }
    }
    int failures = 0;
    for (int round = 0; round < 2; round++) {
        FILE *f = tmpfile();
        if (!f) return 1;
        fprintf(f, "OBS|measure|%s|control-linear|fence-hit|0x1|bool\n", check);
        emit_rows(f, check, "control-linear", "color-target", lin, sizeof(lin), SIZE_MAX);
        /* The right layout, and the wrong one: the same picture written as rows. Round 1
         * drops the tiled arm's last rows, which has to read as undecided. */
        emit_rows(f, check, "arm1-rx", "color-target", tiled, sizeof(tiled),
                  round == 1 ? sizeof(tiled) - 64u : SIZE_MAX);
        emit_rows(f, check, "arm2-rx-mesa", "color-target", lin, sizeof(lin), SIZE_MAX);
        rewind(f);
        job_t j;
        if (job_init(&j, check, "color-target", W, H, "control-linear") != 0 ||
            job_arm(&j, "arm1-rx", 0x08c6c000u) != 0 || job_arm(&j, "arm2-rx-mesa", 0x0dc6c000u) != 0) {
            return 1;
        }
        read_rows(&j, f);
        fclose(f);
        uint32_t chosen = 0;
        const int rc = judge(&j, stdout, &chosen);
        if (round == 0 && !(rc == 0 && chosen == 0x08c6c000u)) failures++;
        if (round == 1 && rc != 2) failures++;
        job_free(&j);
    }
    return failures;
}

/*
 * The same, past one block: 256 x 256 is 2 x 2 blocks, the extent `-4b19` asks for. One arm
 * has the blocks row-major, the other has them transposed - the same 64 KiB pieces in the
 * other order, which is the mistake a single-block sweep cannot catch. The transposed arm has
 * to fail, and the block map has to name it.
 */
static int self_test_multiblock(void) {
    enum { W = 256, H = 256 };
    const char *check = "166-agc/primitive-draw";
    static uint8_t lin[W * H * 4], rowmajor[4 * 65536], transposed[4 * 65536];
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            /* Corners near (16,16), (240,40), (64,240): every block holds drawn and undrawn
             * texels, as the request requires. */
            const int in = (int)(x * 24u + y * 224u) > 16 * 24 + 16 * 224 &&
                           (int)(y * 176u) < (int)(x * 200u) + 40 * 176 - 16 * 200 && x + y < 360;
            const uint32_t v = in ? 0xff2040ffu : FILL;
            const uint32_t bx = x >> 7, by = y >> 7;
            const uint32_t within = agc_tile_pixel(x & 127u, y & 127u) * 4u;
            put32(lin, ((size_t)y * W + x) * 4u, v);
            put32(rowmajor, (size_t)(by * 2u + bx) * 65536u + within, v);
            put32(transposed, (size_t)(bx * 2u + by) * 65536u + within, v);
        }
    }
    FILE *f = tmpfile();
    if (!f) return 1;
    emit_rows(f, check, "control-linear-256", "color-target", lin, sizeof(lin), SIZE_MAX);
    emit_rows(f, check, "arm1-rx", "color-target", rowmajor, sizeof(rowmajor), SIZE_MAX);
    emit_rows(f, check, "arm2-transposed", "color-target", transposed, sizeof(transposed), SIZE_MAX);
    rewind(f);
    job_t j;
    if (job_init(&j, check, "color-target", W, H, "control-linear-256") != 0 ||
        job_arm(&j, "arm1-rx", 0x08c6c000u) != 0 || job_arm(&j, "arm2-transposed", 0x0dc6c000u) != 0) {
        fclose(f);
        return 1;
    }
    read_rows(&j, f);
    fclose(f);
    uint32_t chosen = 0;
    const int rc = judge(&j, stdout, &chosen);
    job_free(&j);
    return (rc == 0 && chosen == 0x08c6c000u) ? 0 : 1;
}

static int self_test(void) {
    int failures = self_test_one_block() + self_test_multiblock();
    printf("self-test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}

/*
 * The shape mode: one tiled arm, no control. Reports whether the tool's block order is the only
 * one that detiles the dump into a triangle, and whether the pixels it finds are the number the
 * check itself reported. Returns 0 when both hold, 1 when they do not.
 */
static int shape_report(job_t *j, const char *arm, FILE *out) {
    surface_t *s = NULL;
    for (int a = 0; a < j->n_arms; a++) {
        if (strcmp(j->arms[a].name, arm) == 0) s = &j->arms[a];
    }
    if (!s) {
        fprintf(out, "shape: no arm named %s\n", arm);
        return 1;
    }
    const size_t c = coverage(s);
    fprintf(out, "%s: %zu of %zu bytes dumped\n", s->name, c, s->size);
    if (c != s->size) {
        fprintf(out, "VERDICT: undecided - the dump is incomplete\n");
        return 1;
    }
    uint32_t found[4] = {0, 1, 2, 3};
    const int orders = shape_search(j, s, out, found);
    uint32_t clean = 0, drawn = 0, touched = 0, gaps = 0;
    const uint32_t identity[4] = {0, 1, 2, 3};
    shape_of(j, s, identity, &clean, &drawn, &touched, &gaps);
    uint32_t reported = 0;
    int have_reported = 0;
    for (int a = 0; a < s_modified_n; a++) {
        if (s_modified_of[a] && strcmp(s_modified_of[a], arm) == 0) {
            reported = s_modified[a];
            have_reported = 1;
        }
    }
    fprintf(out, "  the tool's order: %u of %u rows are one run, %u gap(s), %u pixels drawn",
            clean, touched, gaps, drawn);
    if (have_reported) fprintf(out, "; the check reported %u\n", reported);
    else fprintf(out, "; no modified-pixels row to compare\n");
    if (orders == 0) {
        fprintf(out, "VERDICT: no block order detiles this dump into one run a row\n");
        return 1;
    }
    if (clean != touched || gaps != 0u) {
        fprintf(out, "VERDICT: the tool's block order is NOT the one - %u of %u rows break, "
                     "%u gap(s)\n", touched - clean, touched, gaps);
        return 1;
    }
    if (orders > 1) {
        fprintf(out, "VERDICT: undecided - %d block orders give one run a row, so this picture "
                     "cannot tell them apart\n", orders);
        return 1;
    }
    if (have_reported && drawn != reported) {
        fprintf(out, "VERDICT: the shape is clean but holds %u pixels where the check counted "
                     "%u\n", drawn, reported);
        return 1;
    }
    fprintf(out, "VERDICT: the blocks run in the tool's own order, and it is the only order that "
                 "does\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *log = NULL, *check = "166-agc/primitive-draw", *key = "color-target";
    const char *control = "control-linear", *shape = NULL;
    uint32_t w = 128, h = 128;
    const char *arm_names[MAX_ARMS];
    uint32_t arm_attrib3[MAX_ARMS];
    int n_arms = 0, default_arms = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) return self_test();
        if (i + 1 >= argc) break;
        if (strcmp(argv[i], "--log") == 0) log = argv[++i];
        else if (strcmp(argv[i], "--check") == 0) check = argv[++i];
        else if (strcmp(argv[i], "--key") == 0) key = argv[++i];
        else if (strcmp(argv[i], "--control") == 0) control = argv[++i];
        else if (strcmp(argv[i], "--shape") == 0) shape = argv[++i];
        else if (strcmp(argv[i], "--extent") == 0) {
            if (sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h) return 2;
        } else if (strcmp(argv[i], "--arm") == 0) {
            /* NAME=ATTRIB3; --arm none leaves only the control. */
            char *spec = argv[++i];
            default_arms = 0;
            if (strcmp(spec, "none") == 0) continue;
            char *eq = strchr(spec, '=');
            if (!eq || n_arms >= MAX_ARMS) return 2;
            *eq = '\0';
            arm_names[n_arms] = spec;
            arm_attrib3[n_arms++] = (uint32_t)strtoul(eq + 1, NULL, 0);
        }
    }
    if (!log) {
        fprintf(stderr, "usage: rx_check --self-test | --log OBS_LOG [--check NAME] [--key KEY]\n"
                        "       [--control ARM] [--arm NAME=ATTRIB3 ... | --arm none] [--extent WxH]\n");
        return 2;
    }
    if (default_arms) {
        arm_names[0] = "arm1-rx";
        arm_attrib3[0] = 0x08c6c000u;
        arm_names[1] = "arm2-rx-mesa";
        arm_attrib3[1] = 0x0dc6c000u;
        n_arms = 2;
    }
    FILE *in = fopen(log, "r");
    if (!in) {
        fprintf(stderr, "rx_check: cannot open %s\n", log);
        return 2;
    }
    job_t j;
    if (job_init(&j, check, key, w, h, control) != 0) return 2;
    for (int a = 0; a < n_arms; a++) {
        if (job_arm(&j, arm_names[a], arm_attrib3[a]) != 0) return 2;
    }
    read_rows(&j, in);
    fclose(in);
    printf("rx-check: %s, %s rows, %ux%u\n", check, key, w, h);
    const int rc = shape ? shape_report(&j, shape, stdout) : judge(&j, stdout, NULL);
    job_free(&j);
    return rc;
}
