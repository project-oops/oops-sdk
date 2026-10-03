#include "oops/zip.h"
#include "oops/fs.h"
#include "oops/freestd.h"
#include "tests/test_common.h"
#include <string.h>

/* Unit tests for the ZIP extractor, `oops/zip.h`, on archives built in memory. */

/* Missing or empty paths and buffers are refused with OOPS_ZIP_ERR_PARAM. */
static void test_zip_null_params(void) {
    ASSERT_EQ(oops_zip_extract(NULL, "/tmp"), OOPS_ZIP_ERR_PARAM);
    ASSERT_EQ(oops_zip_extract("", "/tmp"), OOPS_ZIP_ERR_PARAM);
    ASSERT_EQ(oops_zip_extract("/tmp/dummy.zip", NULL), OOPS_ZIP_ERR_PARAM);
    ASSERT_EQ(oops_zip_extract("/tmp/dummy.zip", ""), OOPS_ZIP_ERR_PARAM);

    ASSERT_EQ(oops_zip_extract_mem(NULL, 100, "/tmp"), OOPS_ZIP_ERR_PARAM);
    ASSERT_EQ(oops_zip_extract_mem("DATA", 0, "/tmp"), OOPS_ZIP_ERR_PARAM);
    ASSERT_EQ(oops_zip_extract_mem("DATA", 4, NULL), OOPS_ZIP_ERR_PARAM);
    ASSERT_EQ(oops_zip_extract_mem("DATA", 4, ""), OOPS_ZIP_ERR_PARAM);
}

/* A buffer too short for an end record, or without its signature, is refused. */
static void test_zip_bad_headers(void) {
    /* Too short to have an EOCD (< 22 bytes) is invalid parameter */
    char tiny[10] = {0};
    ASSERT_EQ(oops_zip_extract_mem(tiny, sizeof(tiny), "/tmp"), OOPS_ZIP_ERR_PARAM);

    /* Buffer >= 22 bytes without EOCD signature */
    char bad[64];
    memset(bad, 0xAA, sizeof(bad));
    ASSERT_EQ(oops_zip_extract_mem(bad, sizeof(bad), "/tmp"), OOPS_ZIP_ERR_BAD_HEADER);
}

/* Helper to write uint16_t little endian */
static void put_u16(uint8_t *p, uint16_t val) {
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
}

/* Helper to write uint32_t little endian */
static void put_u32(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

/* A STORED entry extracts byte for byte. */
static void test_zip_extract_stored(void) {
    /* Create a valid ZIP in memory with one STORED file: "test.txt" -> "Hello World!"
     */
    const char *fname = "test.txt";
    uint16_t fname_len = (uint16_t)strlen(fname);
    const char *content = "Hello World!";
    uint32_t content_len = (uint32_t)strlen(content);

    uint8_t zip[512];
    size_t pos = 0;

    /* Local File Header (offset 0) */
    put_u32(zip + pos, 0x04034b50);
    pos += 4;
    put_u16(zip + pos, 10);
    pos += 2; /* version needed */
    put_u16(zip + pos, 0);
    pos += 2; /* flags */
    put_u16(zip + pos, 0);
    pos += 2; /* method 0 = STORED */
    put_u16(zip + pos, 0);
    pos += 2; /* time */
    put_u16(zip + pos, 0);
    pos += 2; /* date */
    put_u32(zip + pos, 0);
    pos += 4; /* crc32 */
    put_u32(zip + pos, content_len);
    pos += 4; /* comp size */
    put_u32(zip + pos, content_len);
    pos += 4; /* uncomp size */
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2; /* extra len */
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;
    memcpy(zip + pos, content, content_len);
    pos += content_len;

    size_t cd_offset = pos;

    /* Central Directory Header */
    put_u32(zip + pos, 0x02014b50);
    pos += 4;
    put_u16(zip + pos, 20);
    pos += 2; /* version made by */
    put_u16(zip + pos, 10);
    pos += 2; /* version needed */
    put_u16(zip + pos, 0);
    pos += 2; /* flags */
    put_u16(zip + pos, 0);
    pos += 2; /* method 0 = STORED */
    put_u16(zip + pos, 0);
    pos += 2; /* time */
    put_u16(zip + pos, 0);
    pos += 2; /* date */
    put_u32(zip + pos, 0);
    pos += 4; /* crc32 */
    put_u32(zip + pos, content_len);
    pos += 4;
    put_u32(zip + pos, content_len);
    pos += 4;
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2; /* extra len */
    put_u16(zip + pos, 0);
    pos += 2; /* comment len */
    put_u16(zip + pos, 0);
    pos += 2; /* disk # */
    put_u16(zip + pos, 0);
    pos += 2; /* int attr */
    put_u32(zip + pos, 0);
    pos += 4; /* ext attr */
    put_u32(zip + pos, 0);
    pos += 4; /* local hdr offset = 0 */
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;

    size_t cd_size = pos - cd_offset;

    /* End of Central Directory */
    put_u32(zip + pos, 0x06054b50);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2; /* disk num */
    put_u16(zip + pos, 0);
    pos += 2; /* cd disk */
    put_u16(zip + pos, 1);
    pos += 2; /* entries on disk */
    put_u16(zip + pos, 1);
    pos += 2; /* total entries */
    put_u32(zip + pos, (uint32_t)cd_size);
    pos += 4;
    put_u32(zip + pos, (uint32_t)cd_offset);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2; /* comment len */

    /* Extract to test directory */
    const char *out_dir = "/tmp/test_oops_zip_out";
    int rc = oops_zip_extract_mem(zip, pos, out_dir);
    ASSERT_EQ(rc, OOPS_ZIP_OK);

    /* Verify extracted file exists and has correct content */
    char target_file[128];
    snprintf(target_file, sizeof(target_file), "%s/%s", out_dir, fname);
    ASSERT_EQ(oops_fs_exists(target_file), 1);
    ASSERT_EQ(oops_fs_file_size(target_file), (int64_t)content_len);

    void *read_buf = NULL;
    size_t read_sz = 0;
    ASSERT_EQ(oops_fs_read_all(target_file, &read_buf, &read_sz), 0);
    ASSERT_EQ(read_sz, (size_t)content_len);
    ASSERT_EQ(memcmp(read_buf, content, content_len), 0);
    oops_fs_free_data(read_buf);

    (void)oops_fs_unlink(target_file);
}

/* A DEFLATE entry inflates, and its subdirectory is created. */
static void test_zip_extract_deflated(void) {
    /* Create a valid ZIP in memory with one DEFLATED file (method 8):
     * Deflate uncompressed block: BFINAL=1, BTYPE=00 (raw stored block within deflate)
     * Header: 1 byte (0x01), 2 bytes LEN, 2 bytes ~LEN, then content.
     */
    const char *fname = "sub/deflate.txt";
    uint16_t fname_len = (uint16_t)strlen(fname);
    const char *content = "Decompressed data through puff inflator!";
    uint32_t uncomp_len = (uint32_t)strlen(content);

    uint8_t deflate_stream[256];
    deflate_stream[0] = 0x01; /* BFINAL=1, BTYPE=00 */
    put_u16(deflate_stream + 1, (uint16_t)uncomp_len);
    put_u16(deflate_stream + 3, (uint16_t)(~uncomp_len & 0xFFFF));
    memcpy(deflate_stream + 5, content, uncomp_len);
    uint32_t comp_len = 5 + uncomp_len;

    uint8_t zip[512];
    size_t pos = 0;

    /* Local File Header */
    put_u32(zip + pos, 0x04034b50);
    pos += 4;
    put_u16(zip + pos, 20);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 8);
    pos += 2; /* method 8 = DEFLATE */
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, comp_len);
    pos += 4;
    put_u32(zip + pos, uncomp_len);
    pos += 4;
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;
    memcpy(zip + pos, deflate_stream, comp_len);
    pos += comp_len;

    size_t cd_offset = pos;

    /* Central Directory Header */
    put_u32(zip + pos, 0x02014b50);
    pos += 4;
    put_u16(zip + pos, 20);
    pos += 2;
    put_u16(zip + pos, 20);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 8);
    pos += 2; /* method 8 = DEFLATE */
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, comp_len);
    pos += 4;
    put_u32(zip + pos, uncomp_len);
    pos += 4;
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, 0);
    pos += 4; /* offset of local header */
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;

    size_t cd_size = pos - cd_offset;

    /* EOCD */
    put_u32(zip + pos, 0x06054b50);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 1);
    pos += 2;
    put_u16(zip + pos, 1);
    pos += 2;
    put_u32(zip + pos, (uint32_t)cd_size);
    pos += 4;
    put_u32(zip + pos, (uint32_t)cd_offset);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;

    const char *out_dir = "/tmp/test_oops_zip_out";
    int rc = oops_zip_extract_mem(zip, pos, out_dir);
    ASSERT_EQ(rc, OOPS_ZIP_OK);

    char target_file[128];
    snprintf(target_file, sizeof(target_file), "%s/%s", out_dir, fname);
    ASSERT_EQ(oops_fs_exists(target_file), 1);
    ASSERT_EQ(oops_fs_file_size(target_file), (int64_t)uncomp_len);

    void *read_buf = NULL;
    size_t read_sz = 0;
    ASSERT_EQ(oops_fs_read_all(target_file, &read_buf, &read_sz), 0);
    ASSERT_EQ(read_sz, (size_t)uncomp_len);
    ASSERT_EQ(memcmp(read_buf, content, uncomp_len), 0);
    oops_fs_free_data(read_buf);

    (void)oops_fs_unlink(target_file);
}

/* An entry named "../escape.txt" is refused rather than written outside the target. */
static void test_zip_path_traversal_rejection(void) {
    const char *fname = "../escape.txt";
    uint16_t fname_len = (uint16_t)strlen(fname);

    uint8_t zip[512];
    size_t pos = 0;

    put_u32(zip + pos, 0x04034b50);
    pos += 4;
    put_u16(zip + pos, 10);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;

    size_t cd_offset = pos;

    put_u32(zip + pos, 0x02014b50);
    pos += 4;
    put_u16(zip + pos, 20);
    pos += 2;
    put_u16(zip + pos, 10);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, 0);
    pos += 4;
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;

    size_t cd_size = pos - cd_offset;

    put_u32(zip + pos, 0x06054b50);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 1);
    pos += 2;
    put_u16(zip + pos, 1);
    pos += 2;
    put_u32(zip + pos, (uint32_t)cd_size);
    pos += 4;
    put_u32(zip + pos, (uint32_t)cd_offset);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;

    int rc = oops_zip_extract_mem(zip, pos, "/tmp/test_oops_zip_out");
    ASSERT_EQ(rc, OOPS_ZIP_ERR_PARAM);
}

/*
 * Nested directories, and an entry that goes back to one already used.
 *
 * The extractor remembers the parent directory of the last file so it does not walk and
 * stat every path component again for the next one - which for an archive of 7,700
 * files five levels deep was 38,500 syscalls, of which about a hundred did anything,
 * and was the whole of the unpack time on hardware.
 *
 * The thing that optimisation can break is a file whose parent is *not* the previous
 * one, so this archive is deliberately ordered to do that: two files sharing a parent
 * (the case the cache is for), then a file in a different tree, then a fourth file back
 * in the first tree. If the cache were ever wrong about what exists, that fourth file
 * would be the one written into a directory that is not there.
 */
static void test_zip_extract_nested_out_of_order(void) {
    static const char *const names[] = {
        "deep/a/b/one.txt",
        "deep/a/b/two.txt",  /* same parent as the last - the cache hit */
        "other/c/three.txt", /* a different tree - the cache must move */
        "deep/a/b/four.txt", /* back to the first tree - the case that can break */
    };
    const size_t count = sizeof(names) / sizeof(names[0]);
    const char *content = "x";
    const uint32_t clen = 1;

    uint8_t zip[1024];
    size_t pos = 0;
    size_t local_off[4];

    for (size_t i = 0; i < count; i++) {
        uint16_t nlen = (uint16_t)strlen(names[i]);
        local_off[i] = pos;
        put_u32(zip + pos, 0x04034b50);
        pos += 4;
        put_u16(zip + pos, 10);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2; /* STORED */
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u32(zip + pos, 0);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u16(zip + pos, nlen);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        memcpy(zip + pos, names[i], nlen);
        pos += nlen;
        memcpy(zip + pos, content, clen);
        pos += clen;
    }

    size_t cd_offset = pos;
    for (size_t i = 0; i < count; i++) {
        uint16_t nlen = (uint16_t)strlen(names[i]);
        put_u32(zip + pos, 0x02014b50);
        pos += 4;
        put_u16(zip + pos, 20);
        pos += 2;
        put_u16(zip + pos, 10);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2; /* STORED */
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u32(zip + pos, 0);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u16(zip + pos, nlen);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u32(zip + pos, 0);
        pos += 4;
        put_u32(zip + pos, (uint32_t)local_off[i]);
        pos += 4;
        memcpy(zip + pos, names[i], nlen);
        pos += nlen;
    }
    size_t cd_size = pos - cd_offset;

    put_u32(zip + pos, 0x06054b50);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, (uint16_t)count);
    pos += 2;
    put_u16(zip + pos, (uint16_t)count);
    pos += 2;
    put_u32(zip + pos, (uint32_t)cd_size);
    pos += 4;
    put_u32(zip + pos, (uint32_t)cd_offset);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;

    const char *out_dir = "/tmp/test_oops_zip_nested";
    ASSERT_EQ(oops_zip_extract_mem(zip, pos, out_dir), OOPS_ZIP_OK);

    /* Every one of them, including the fourth. */
    for (size_t i = 0; i < count; i++) {
        char target[256];
        snprintf(target, sizeof(target), "%s/%s", out_dir, names[i]);
        ASSERT_EQ(oops_fs_exists(target), 1);
        ASSERT_EQ(oops_fs_file_size(target), (int64_t)clen);
        (void)oops_fs_unlink(target);
    }
}

static int filter_keep_prefix(const char *name, void *userdata) {
    const char *prefix = (const char *)userdata;
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

/* Filtered extraction extracts only matching entries and skips others. */
static void test_zip_extract_filter(void) {
    static const char *const names[] = {
        "keep/one.txt",
        "drop/two.txt",
        "keep/three.txt",
    };
    const size_t count = sizeof(names) / sizeof(names[0]);
    const char *content = "z";
    const uint32_t clen = 1;

    uint8_t zip[1024];
    size_t pos = 0;
    size_t local_off[3];

    for (size_t i = 0; i < count; i++) {
        uint16_t nlen = (uint16_t)strlen(names[i]);
        local_off[i] = pos;
        put_u32(zip + pos, 0x04034b50);
        pos += 4;
        put_u16(zip + pos, 10);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2; /* STORED */
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u32(zip + pos, 0);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u16(zip + pos, nlen);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        memcpy(zip + pos, names[i], nlen);
        pos += nlen;
        memcpy(zip + pos, content, clen);
        pos += clen;
    }

    size_t cd_offset = pos;
    for (size_t i = 0; i < count; i++) {
        uint16_t nlen = (uint16_t)strlen(names[i]);
        put_u32(zip + pos, 0x02014b50);
        pos += 4;
        put_u16(zip + pos, 20);
        pos += 2;
        put_u16(zip + pos, 10);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2; /* STORED */
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u32(zip + pos, 0);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u32(zip + pos, clen);
        pos += 4;
        put_u16(zip + pos, nlen);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u16(zip + pos, 0);
        pos += 2;
        put_u32(zip + pos, 0);
        pos += 4;
        put_u32(zip + pos, (uint32_t)local_off[i]);
        pos += 4;
        memcpy(zip + pos, names[i], nlen);
        pos += nlen;
    }
    size_t cd_size = pos - cd_offset;

    put_u32(zip + pos, 0x06054b50);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, (uint16_t)count);
    pos += 2;
    put_u16(zip + pos, (uint16_t)count);
    pos += 2;
    put_u32(zip + pos, (uint32_t)cd_size);
    pos += 4;
    put_u32(zip + pos, (uint32_t)cd_offset);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;

    const char *out_dir = "/tmp/test_oops_zip_filter";
    ASSERT_EQ(oops_zip_extract_mem_filter(zip, pos, out_dir, filter_keep_prefix,
                                          (void *)"keep/"),
              OOPS_ZIP_OK);

    char path_keep1[256];
    char path_drop2[256];
    char path_keep3[256];
    snprintf(path_keep1, sizeof(path_keep1), "%s/%s", out_dir, names[0]);
    snprintf(path_drop2, sizeof(path_drop2), "%s/%s", out_dir, names[1]);
    snprintf(path_keep3, sizeof(path_keep3), "%s/%s", out_dir, names[2]);

    ASSERT_EQ(oops_fs_exists(path_keep1), 1);
    ASSERT_EQ(oops_fs_exists(path_drop2), 0);
    ASSERT_EQ(oops_fs_exists(path_keep3), 1);

    (void)oops_fs_unlink(path_keep1);
    (void)oops_fs_unlink(path_keep3);
}

struct test_progress_state {
    int saw_zero;
    int saw_total;
    uint32_t calls;
};

static void test_progress_cb(uint32_t current, uint32_t total, void *userdata) {
    struct test_progress_state *st = (struct test_progress_state *)userdata;
    if (current == 0)
        st->saw_zero = 1;
    if (current == total && total > 0)
        st->saw_total = 1;
    st->calls++;
}

/* Progress callback is invoked from 0 through total entries during extraction. */
static void test_zip_extract_progress(void) {
    const char *fname = "prog.txt";
    uint16_t fname_len = (uint16_t)strlen(fname);
    const char *content = "progress_test";
    uint32_t content_len = (uint32_t)strlen(content);

    uint8_t zip[512];
    size_t pos = 0;

    put_u32(zip + pos, 0x04034b50);
    pos += 4;
    put_u16(zip + pos, 10);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, content_len);
    pos += 4;
    put_u32(zip + pos, content_len);
    pos += 4;
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;
    memcpy(zip + pos, content, content_len);
    pos += content_len;

    size_t cd_offset = pos;
    put_u32(zip + pos, 0x02014b50);
    pos += 4;
    put_u16(zip + pos, 20);
    pos += 2;
    put_u16(zip + pos, 10);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, content_len);
    pos += 4;
    put_u32(zip + pos, content_len);
    pos += 4;
    put_u16(zip + pos, fname_len);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u32(zip + pos, 0);
    pos += 4;
    put_u32(zip + pos, 0);
    pos += 4;
    memcpy(zip + pos, fname, fname_len);
    pos += fname_len;

    size_t cd_size = pos - cd_offset;
    put_u32(zip + pos, 0x06054b50);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 0);
    pos += 2;
    put_u16(zip + pos, 1);
    pos += 2;
    put_u16(zip + pos, 1);
    pos += 2;
    put_u32(zip + pos, (uint32_t)cd_size);
    pos += 4;
    put_u32(zip + pos, (uint32_t)cd_offset);
    pos += 4;
    put_u16(zip + pos, 0);
    pos += 2;

    struct test_progress_state st = {0};
    const char *out_dir = "/tmp/test_oops_zip_prog";
    ASSERT_EQ(oops_zip_extract_mem_filter_progress(zip, pos, out_dir, NULL,
                                                   test_progress_cb, &st),
              OOPS_ZIP_OK);

    ASSERT_EQ(st.saw_zero, 1);
    ASSERT_EQ(st.saw_total, 1);
    ASSERT_EQ((st.calls >= 2), 1);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", out_dir, fname);
    (void)oops_fs_unlink(path);
}

void run_unit_tests_zip(void) {
    TEST_SUITE_BEGIN("Freestanding ZIP Archive Extractor & Deflate");
    RUN_TEST(test_zip_null_params);
    RUN_TEST(test_zip_bad_headers);
    RUN_TEST(test_zip_extract_stored);
    RUN_TEST(test_zip_extract_deflated);
    RUN_TEST(test_zip_path_traversal_rejection);
    RUN_TEST(test_zip_extract_nested_out_of_order);
    RUN_TEST(test_zip_extract_filter);
    RUN_TEST(test_zip_extract_progress);
}
