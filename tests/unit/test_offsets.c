#include "tests/test_common.h"
#include "oops/offsets.h"

static const char *s_test_toml =
    "# Sample test offsets\n"
    "[common]\n"
    "p_ucred = 0x40\n"
    "p_fd = 0x48\n"
    "p_pid = 0xBC\n"
    "fd_rdir = 0x10\n"
    "fd_jdir = 0x18\n"
    "cr_uid = 0x04\n"
    "cr_sceauthid = 0x58\n"
    "cr_scecaps = 0x60\n"
    "cr_sceattr0 = 0x83\n"
    "\n"
    "[04.03]\n"
    "kdata_base = 0xFFFFFFFF80E10000\n"
    "allproc = 0x27EDCB8\n"
    "rootvnode = 0x66E74C0\n"
    "security_flags = 0x6505474\n"
    "sysents = 0x1709C0\n"
    "sysentvec = 0xD11BB8\n"
    "\n"
    "[10.01]\n"
    "kdata_base = 0xFFFFFFFF80ED0000\n"
    "allproc = 0x2765D70\n"
    "rootvnode = 0x2FA3510\n"
    "security_flags = 0x0D79064\n"
    "sysents = 0x1AD100\n"
    "sysentvec = 0xDBA6D8\n";

static void test_offsets_parse_string(void) {
    int rc = oops_offsets_load_string(s_test_toml);
    ASSERT_EQ(rc, 0);

    const oops_common_offsets_t *common = oops_offsets_get_common();
    ASSERT_TRUE(common != NULL);
    ASSERT_EQ(common->p_ucred, 0x40);
    ASSERT_EQ(common->p_fd, 0x48);
    ASSERT_EQ(common->p_pid, 0xBC);
    ASSERT_EQ(common->fd_rdir, 0x10);
    ASSERT_EQ(common->fd_jdir, 0x18);
    ASSERT_EQ(common->cr_uid, 0x04);
    ASSERT_EQ(common->cr_sceauthid, 0x58);
    ASSERT_EQ(common->cr_scecaps, 0x60);
    ASSERT_EQ(common->cr_sceattr0, 0x83);

    /* Test lookup by raw BCD (0x04030000) */
    const oops_fw_offsets_t *fw403 = oops_offsets_get_fw(0x04030000);
    ASSERT_TRUE(fw403 != NULL);
    ASSERT_EQ(fw403->kdata_base, 0xFFFFFFFF80E10000ULL);
    ASSERT_EQ(fw403->allproc, 0x27EDCB8);
    ASSERT_EQ(fw403->rootvnode, 0x66E74C0);
    ASSERT_EQ(fw403->security_flags, 0x6505474);
    ASSERT_EQ(fw403->sysents, 0x1709C0);
    ASSERT_EQ(fw403->sysentvec, 0xD11BB8);

    /* Test lookup by string ("10.01") */
    const oops_fw_offsets_t *fw1001 = oops_offsets_get_fw_str("10.01");
    ASSERT_TRUE(fw1001 != NULL);
    ASSERT_EQ(fw1001->kdata_base, 0xFFFFFFFF80ED0000ULL);
    ASSERT_EQ(fw1001->allproc, 0x2765D70);
    ASSERT_EQ(fw1001->rootvnode, 0x2FA3510);
    ASSERT_EQ(fw1001->security_flags, 0x0D79064);
    ASSERT_EQ(fw1001->sysents, 0x1AD100);
    ASSERT_EQ(fw1001->sysentvec, 0xDBA6D8);

    /* Test unsupported firmware query */
    const oops_fw_offsets_t *fw_unsupported = oops_offsets_get_fw(0x99990000);
    ASSERT_TRUE(fw_unsupported == NULL);
}

static void test_offsets_load_file_on_disk(void) {
    /* Test loading the real data/offsets.toml shipped in repo */
    int rc = oops_offsets_load_file("data/offsets.toml");
    if (rc != 0) {
        /* Fallback if running from a different working dir */
        rc = oops_offsets_load_file("../data/offsets.toml");
    }
    ASSERT_EQ(rc, 0);

    const oops_common_offsets_t *common = oops_offsets_get_common();
    ASSERT_TRUE(common != NULL);
    ASSERT_EQ(common->p_ucred, 0x40);
    ASSERT_EQ(common->p_titleid, 0x470);
    ASSERT_EQ(common->p_comm, 0x5DC);

    /* Verify multiple firmwares present */
    ASSERT_TRUE(oops_offsets_get_fw_str("01.00") != NULL);
    ASSERT_TRUE(oops_offsets_get_fw_str("03.00") != NULL);
    ASSERT_TRUE(oops_offsets_get_fw_str("04.03") != NULL);
    ASSERT_TRUE(oops_offsets_get_fw_str("07.00") != NULL);
    ASSERT_TRUE(oops_offsets_get_fw_str("10.01") != NULL);
    ASSERT_TRUE(oops_offsets_get_fw_str("12.00") != NULL);
}

static void test_offsets_null_safety(void) {
    ASSERT_EQ(oops_offsets_load_string(NULL), -1);
    ASSERT_EQ(oops_offsets_load_file(NULL), -1);
    ASSERT_EQ(oops_offsets_load_file("nonexistent_file.toml"), -1);
    ASSERT_TRUE(oops_offsets_get_fw_str(NULL) == NULL);
}

void run_unit_tests_offsets(void) {
    TEST_SUITE_BEGIN("Runtime TOML Firmware Offsets");
    RUN_TEST(test_offsets_parse_string);
    RUN_TEST(test_offsets_load_file_on_disk);
    RUN_TEST(test_offsets_null_safety);
}

