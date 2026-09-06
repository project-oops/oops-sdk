#include "tests/test_common.h"
#include "oops/freestd.h"

static void test_freestd_strings(void) {
    ASSERT_EQ(obs_strlen("hello"), 5);
    ASSERT_EQ(obs_strlen(""), 0);
    ASSERT_EQ(obs_strlen(NULL), 0);

    ASSERT_EQ(obs_strcmp("abc", "abc"), 0);
    ASSERT_TRUE(obs_strcmp("abc", "abd") < 0);
    ASSERT_TRUE(obs_strcmp("abd", "abc") > 0);

    ASSERT_EQ(obs_strncmp("abcdef", "abcxyz", 3), 0);
    ASSERT_TRUE(obs_strncmp("abcdef", "abcxyz", 4) < 0);

    char dest[16];
    obs_strncpy(dest, "test", sizeof(dest));
    ASSERT_EQ(obs_strcmp(dest, "test"), 0);
}

static void test_freestd_formatting(void) {
    char buf[64];
    size_t len;

    len = obs_format_u64(buf, 12345);
    buf[len] = '\0';
    ASSERT_EQ(obs_strcmp(buf, "12345"), 0);

    len = obs_format_i64(buf, -42);
    buf[len] = '\0';
    ASSERT_EQ(obs_strcmp(buf, "-42"), 0);

    len = obs_format_hex(buf, 0x1a2b);
    buf[len] = '\0';
    ASSERT_EQ(obs_strcmp(buf, "0x1a2b"), 0);
}

static void test_freestd_nid(void) {
    char nid[12];
    obs_compute_nid("sceKernelGetProcessId", nid);
    ASSERT_EQ(obs_strlen(nid), 11);
}

void run_unit_tests_freestd(void) {
    TEST_SUITE_BEGIN("Freestanding Runtime Helpers");
    RUN_TEST(test_freestd_strings);
    RUN_TEST(test_freestd_formatting);
    RUN_TEST(test_freestd_nid);
}

