#include "tests/test_common.h"
#include "oops/net.h"

static void test_net_endian_conversions(void) {
    ASSERT_EQ(oops_htons(0x1234), 0x3412);
    ASSERT_EQ(oops_ntohs(0x3412), 0x1234);

    ASSERT_EQ(oops_htonl(0x12345678), 0x78563412);
    ASSERT_EQ(oops_ntohl(0x78563412), 0x12345678);
}

static void test_net_ipv4_parsing_valid(void) {
    uint32_t ip = 0;
    char out[32];

    ASSERT_EQ(oops_net_inet_pton("192.168.1.1", &ip), 0);
    ASSERT_EQ(oops_net_inet_ntop(ip, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "192.168.1.1");

    ASSERT_EQ(oops_net_inet_pton("127.0.0.1", &ip), 0);
    ASSERT_EQ(oops_net_inet_ntop(ip, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "127.0.0.1");

    ASSERT_EQ(oops_net_inet_pton("255.255.255.255", &ip), 0);
    ASSERT_EQ(oops_net_inet_ntop(ip, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "255.255.255.255");

    ASSERT_EQ(oops_net_inet_pton("0.0.0.0", &ip), 0);
    ASSERT_EQ(oops_net_inet_ntop(ip, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "0.0.0.0");
}

static void test_net_ipv4_parsing_invalid(void) {
    uint32_t ip = 0;
    ASSERT_NE(oops_net_inet_pton(NULL, &ip), 0);
    ASSERT_NE(oops_net_inet_pton("192.168.1", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("192.168.1.1.1", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("192.168.1.256", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("192.168.1.-1", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("abc.def.ghi.jkl", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("", &ip), 0);
    /* Empty, over-long, or trailing-dot octets are malformed, not silently zero. */
    ASSERT_NE(oops_net_inet_pton("1..2.3", &ip), 0);
    ASSERT_NE(oops_net_inet_pton(".1.2.3", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("1.2.3.", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("1.2.3.0004", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("1.2.3.4 ", &ip), 0);
    ASSERT_NE(oops_net_inet_pton("1.2.3.4.", &ip), 0);
}

void run_unit_tests_net(void) {
    TEST_SUITE_BEGIN("BSD Sockets & Networking");
    RUN_TEST(test_net_endian_conversions);
    RUN_TEST(test_net_ipv4_parsing_valid);
    RUN_TEST(test_net_ipv4_parsing_invalid);
}

