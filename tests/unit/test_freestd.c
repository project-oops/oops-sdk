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

/* NID hashing, pinned against SELFish (the format authority): SHA-1(name || 16-byte suffix
 * 518D64A635DED8C1E6B039B1C3E55230), first 8 digest bytes read little-endian, shifted left 2,
 * emitted as 11 six-bit groups MSB-first through base64 with + and - as the last two symbols.
 * The length-only check could not fail on a wrong suffix, byte order or alphabet; these values
 * can. sceKernelGetProcessId is the string obSCEne has not found the platform to export, but
 * the hash of the string is still well-defined and is what this pins.
 * (SELFish commit 6aa50c8, crates/selfish-nid/tests/depended_on.rs) */
static void test_freestd_nid(void) {
    char nid[12];

    obs_compute_nid("sceKernelLoadStartModule", nid);
    ASSERT_EQ(obs_strlen(nid), 11);
    ASSERT_STR_EQ(nid, "wzvqT4UqKX8");   /* one pair that breaks on any of the four mistakes */

    obs_compute_nid("sceKernelGetProcessId", nid);
    ASSERT_STR_EQ(nid, "ciYaJofC6tg");

    obs_compute_nid("sceKernelWrite", nid);
    ASSERT_STR_EQ(nid, "4wSze92BhLI");

    obs_compute_nid("scePadReadState", nid);
    ASSERT_STR_EQ(nid, "YndgXqQVV7c");
}

void run_unit_tests_freestd(void) {
    TEST_SUITE_BEGIN("Freestanding Runtime Helpers");
    RUN_TEST(test_freestd_strings);
    RUN_TEST(test_freestd_formatting);
    RUN_TEST(test_freestd_nid);
}

