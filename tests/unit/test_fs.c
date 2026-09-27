#include "oops/fs.h"
#include "oops/freestd.h"
#include "tests/test_common.h"
#include <stdio.h>

/* Unit tests for the filesystem layer, `oops/fs.h`, against the host's /tmp. */

#define TEST_PATH "/tmp/test_oops_fs.tmp"

/* Every call refuses NULL paths and bad descriptors. */
static void test_fs_null_safety(void) {
    ASSERT_EQ(oops_fs_open(NULL, 0, 0), -1);
    ASSERT_EQ(oops_fs_close(-1), -1);
    ASSERT_EQ(oops_fs_read(-1, NULL, 0), -1);
    ASSERT_EQ(oops_fs_write(-1, NULL, 10), -1);
    ASSERT_EQ(oops_fs_seek(-1, 0, 0), -1);
    ASSERT_EQ(oops_fs_tell(-1), -1);
    ASSERT_EQ(oops_fs_exists(NULL), 0);
    ASSERT_EQ(oops_fs_file_size(NULL), -1);
    ASSERT_EQ(oops_fs_read_all(NULL, NULL, NULL), -1);
    ASSERT_EQ(oops_fs_write_all(NULL, NULL, 0), -1);
    ASSERT_EQ(oops_fs_mkdir(NULL, 0), -1);
    ASSERT_EQ(oops_fs_unlink(NULL), -1);
    oops_fs_free_data(NULL);
}

/* Write, read, seek, tell and the whole-file helpers agree on one file's contents. */
static void test_fs_read_write_seek(void) {
    /* Clean up any leftover file */
    (void)oops_fs_unlink(TEST_PATH);

    ASSERT_EQ(oops_fs_exists(TEST_PATH), 0);

    /* Open for write + create */
    int fd = oops_fs_open(TEST_PATH, OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC, 0644);
    ASSERT_TRUE(fd >= 0);

    const char payload[] = "Hello OOPS Filesystem!\nLine 2";
    size_t len = obs_strlen(payload);
    int64_t written = oops_fs_write(fd, payload, len);
    ASSERT_EQ(written, (int64_t)len);
    ASSERT_EQ(oops_fs_close(fd), 0);

    /* Verify existence and size */
    ASSERT_EQ(oops_fs_exists(TEST_PATH), 1);
    ASSERT_EQ(oops_fs_file_size(TEST_PATH), (int64_t)len);

    /* Open for read */
    fd = oops_fs_open(TEST_PATH, OOPS_O_RDONLY, 0);
    ASSERT_TRUE(fd >= 0);

    char buf[64];
    int64_t n = oops_fs_read(fd, buf, 5);
    ASSERT_EQ(n, 5);
    buf[5] = '\0';
    ASSERT_STR_EQ(buf, "Hello");
    ASSERT_EQ(oops_fs_tell(fd), 5);

    /* Seek to end and back */
    int64_t end_pos = oops_fs_seek(fd, 0, OOPS_SEEK_END);
    ASSERT_EQ(end_pos, (int64_t)len);

    int64_t cur_pos = oops_fs_seek(fd, 6, OOPS_SEEK_SET);
    ASSERT_EQ(cur_pos, 6);

    n = oops_fs_read(fd, buf, 4);
    ASSERT_EQ(n, 4);
    buf[4] = '\0';
    ASSERT_STR_EQ(buf, "OOPS");

    ASSERT_EQ(oops_fs_close(fd), 0);

    /* Whole file helpers */
    void *data = NULL;
    size_t size = 0;
    int rc = oops_fs_read_all(TEST_PATH, &data, &size);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(size, len);
    ASSERT_TRUE(data != NULL);
    ASSERT_STR_EQ((const char *)data, payload);
    oops_fs_free_data(data);

    /* Overwrite using write_all */
    const char new_payload[] = "Overwritten content";
    rc = oops_fs_write_all(TEST_PATH, new_payload, obs_strlen(new_payload));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(oops_fs_file_size(TEST_PATH), (int64_t)obs_strlen(new_payload));

    /* Unlink */
    ASSERT_EQ(oops_fs_unlink(TEST_PATH), 0);
    ASSERT_EQ(oops_fs_exists(TEST_PATH), 0);
}

/* The app-data directory exists, and a path resolved under it is writable. */
static void test_fs_storage_dir(void) {
    char dir[256];
    ASSERT_EQ(oops_fs_get_storage_dir(OOPS_STORAGE_APP_DATA, NULL, 0), -1);
    ASSERT_EQ(oops_fs_get_storage_dir(OOPS_STORAGE_APP_DATA, dir, sizeof(dir)), 0);
    ASSERT_TRUE(obs_strlen(dir) > 0);
    ASSERT_EQ(oops_fs_exists(dir), 1);

    char file_path[256];
    ASSERT_EQ(oops_fs_storage_path(OOPS_STORAGE_APP_DATA, "test.dat", file_path,
                                   sizeof(file_path)),
              0);
    ASSERT_TRUE(obs_strlen(file_path) > obs_strlen(dir));

    /* Write and read through the resolved path */
    const char test_data[] = "storage_test_payload";
    ASSERT_EQ(oops_fs_write_all(file_path, test_data, obs_strlen(test_data)), 0);
    ASSERT_EQ(oops_fs_exists(file_path), 1);

    void *read_back = NULL;
    size_t read_sz = 0;
    ASSERT_EQ(oops_fs_read_all(file_path, &read_back, &read_sz), 0);
    ASSERT_EQ(read_sz, obs_strlen(test_data));
    ASSERT_STR_EQ((const char *)read_back, test_data);
    oops_fs_free_data(read_back);

    (void)oops_fs_unlink(file_path);
}

/*
 * Path resolution, which every path crosses on its way to the kernel.
 *
 * A payload has no working directory the kernel resolves against, so a relative path
 * reaches it unchanged and fails. This is pure string work with no syscall in it, and
 * the only reason its mistakes ever needed a console to find is that nothing on this
 * side used to call it. Both bugs below were found that expensive way once.
 */
const char *oops_fs_resolve_path(const char *path, char *buf, size_t max);

static void test_fs_resolve_path(void) {
    char buf[256];

    /* A relative path is answered against the place the package is mounted. libzip
     * opened "./soh.o2r" and failed while the file sat at /app0/soh.o2r. */
    ASSERT_STR_EQ(oops_fs_resolve_path("./soh.o2r", buf, sizeof(buf)), "/app0/soh.o2r");
    ASSERT_STR_EQ(oops_fs_resolve_path("soh.o2r", buf, sizeof(buf)), "/app0/soh.o2r");

    /* `std::filesystem::absolute` leaves the dot in the middle when it joins a relative
     * path to the working directory, and the kernel refuses that too. */
    ASSERT_STR_EQ(oops_fs_resolve_path("/app0/./soh.o2r", buf, sizeof(buf)),
                  "/app0/soh.o2r");
    ASSERT_STR_EQ(oops_fs_resolve_path("/app0/mods/../soh.o2r", buf, sizeof(buf)),
                  "/app0/soh.o2r");

    /* An absolute path with nothing to resolve is unchanged. */
    ASSERT_STR_EQ(oops_fs_resolve_path("/app0/soh.o2r", buf, sizeof(buf)),
                  "/app0/soh.o2r");

    /*
     * A trailing `/.` survives, and this is the case that matters most: it asserts that
     * what precedes it is a directory, and `common/posix`'s `is_directory` asks this
     * kernel exactly that way - a file answers ENOTDIR. Collapsing it made every
     * regular file open as a directory, and libultraship then built a
     * `directory_iterator` over `soh.o2r` and threw.
     */
    ASSERT_STR_EQ(oops_fs_resolve_path("/app0/soh.o2r/.", buf, sizeof(buf)),
                  "/app0/soh.o2r/.");
    ASSERT_STR_EQ(oops_fs_resolve_path("./assets/.", buf, sizeof(buf)),
                  "/app0/assets/.");

    /* `..` never walks off the root, and the root itself stays a path. */
    ASSERT_STR_EQ(oops_fs_resolve_path("/../..", buf, sizeof(buf)), "/");
    ASSERT_STR_EQ(oops_fs_resolve_path("/", buf, sizeof(buf)), "/");

    /* Too long to rewrite is returned as written, so it fails as the caller spelled it
     * rather than resolving to some other file that happens to exist. */
    {
        char tiny[8];
        ASSERT_STR_EQ(
            oops_fs_resolve_path("/app0/a/very/long/path", tiny, sizeof(tiny)),
            "/app0/a/very/long/path");
    }

    /* NULL in, NULL out, like every other call in this file. */
    ASSERT_TRUE(oops_fs_resolve_path(NULL, buf, sizeof(buf)) == NULL);
}

void run_unit_tests_fs(void) {
    TEST_SUITE_BEGIN("High-Level Filesystem Subsystem");
    RUN_TEST(test_fs_null_safety);
    RUN_TEST(test_fs_read_write_seek);
    RUN_TEST(test_fs_storage_dir);
    RUN_TEST(test_fs_resolve_path);
}
