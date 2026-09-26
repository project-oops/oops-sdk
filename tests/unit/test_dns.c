#include "oops/net.h"
#include "oops/freestd.h"
#include "tests/test_common.h"

/* Unit tests for the RFC 1035 DNS client in `oops/net.h`, on in-memory packets. */

/* NULL, empty and too-small arguments are refused by every entry point. */
static void test_dns_null_and_bounds(void) {
    uint8_t buf[256];
    char ip[32];

    ASSERT_EQ(oops_dns_build_query(NULL, 0x1234, buf, sizeof(buf)), -1);
    ASSERT_EQ(oops_dns_build_query("example.com", 0x1234, NULL, sizeof(buf)), -1);
    ASSERT_EQ(oops_dns_build_query("example.com", 0x1234, buf, 10), -1);
    ASSERT_EQ(oops_dns_build_query("", 0x1234, buf, sizeof(buf)), -1);

    ASSERT_EQ(oops_dns_parse_response(NULL, 100, 0x1234, ip, sizeof(ip)), -1);
    ASSERT_EQ(oops_dns_parse_response(buf, 10, 0x1234, ip, sizeof(ip)), -1);
    ASSERT_EQ(oops_dns_parse_response(buf, 100, 0x1234, NULL, sizeof(ip)), -1);
    ASSERT_EQ(oops_dns_parse_response(buf, 100, 0x1234, ip, 10), -1);

    ASSERT_EQ(oops_net_resolve(NULL, ip, sizeof(ip)), -1);
    ASSERT_EQ(oops_net_resolve("example.com", NULL, sizeof(ip)), -1);
    ASSERT_EQ(oops_net_resolve("example.com", ip, 5), -1);
}

/* A query for an A record is laid out byte for byte as RFC 1035 4.1 says. */
static void test_dns_build_query(void) {
    uint8_t buf[256];
    int len = oops_dns_build_query("example.com", 0x1234, buf, sizeof(buf));

    /* Header (12) + QNAME (1 + 7 + 1 + 3 + 1 = 13) + QTYPE (2) + QCLASS (2) = 29 */
    ASSERT_EQ(len, 29);

    /* Transaction ID */
    ASSERT_EQ(buf[0], 0x12);
    ASSERT_EQ(buf[1], 0x34);

    /* Flags: RD = 1 */
    ASSERT_EQ(buf[2], 0x01);
    ASSERT_EQ(buf[3], 0x00);

    /* QDCOUNT = 1 */
    ASSERT_EQ(buf[4], 0x00);
    ASSERT_EQ(buf[5], 0x01);

    /* Labels */
    ASSERT_EQ(buf[12], 7);
    ASSERT_EQ(obs_strncmp((const char *)&buf[13], "example", 7), 0);
    ASSERT_EQ(buf[20], 3);
    ASSERT_EQ(obs_strncmp((const char *)&buf[21], "com", 3), 0);
    ASSERT_EQ(buf[24], 0);

    /* QTYPE = 1 (A) */
    ASSERT_EQ(buf[25], 0x00);
    ASSERT_EQ(buf[26], 0x01);

    /* QCLASS = 1 (IN) */
    ASSERT_EQ(buf[27], 0x00);
    ASSERT_EQ(buf[28], 0x01);
}

/* A response yields its A record's address; a wrong ID or an error RCODE is refused. */
static void test_dns_parse_response(void) {
    uint8_t pkt[128];
    char ip[32];

    /* 1. Build a synthetic RFC 1035 response for example.com -> 93.184.216.34 */
    size_t pos = 0;

    /* Header (12 bytes) */
    pkt[pos++] = 0x50;
    pkt[pos++] = 0x53; /* ID = 0x5053 */
    pkt[pos++] = 0x81;
    pkt[pos++] = 0x80; /* Flags: QR=1, RD=1, RA=1, RCODE=0 */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* QDCOUNT = 1 */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* ANCOUNT = 1 */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00; /* NSCOUNT = 0 */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00; /* ARCOUNT = 0 */

    /* Question: example.com, Type 1, Class 1 */
    pkt[pos++] = 7;
    memcpy(&pkt[pos], "example", 7);
    pos += 7;
    pkt[pos++] = 3;
    memcpy(&pkt[pos], "com", 3);
    pos += 3;
    pkt[pos++] = 0;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* QTYPE = A */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* QCLASS = IN */

    /* Answer: Pointer to example.com (0xC00C), Type 1, Class 1, TTL 300, Len 4, IP */
    pkt[pos++] = 0xC0;
    pkt[pos++] = 0x0C; /* Name pointer to offset 12 */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* TYPE = A */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* CLASS = IN */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01;
    pkt[pos++] = 0x2C; /* TTL = 300 */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x04; /* RDLENGTH = 4 */
    pkt[pos++] = 93;
    pkt[pos++] = 184;
    pkt[pos++] = 216;
    pkt[pos++] = 34; /* 93.184.216.34 */

    /* Parse with matching ID */
    ASSERT_EQ(oops_dns_parse_response(pkt, pos, 0x5053, ip, sizeof(ip)), 0);
    ASSERT_STR_EQ(ip, "93.184.216.34");

    /* Parse with ID mismatch */
    ASSERT_EQ(oops_dns_parse_response(pkt, pos, 0x9999, ip, sizeof(ip)), -1);

    /* Parse with error RCODE (e.g. NXDOMAIN = 3) */
    pkt[3] = 0x83;
    ASSERT_EQ(oops_dns_parse_response(pkt, pos, 0x5053, ip, sizeof(ip)), -1);
}

/* Dotted addresses resolve to themselves and "localhost" to 127.0.0.1 with no query. */
static void test_dns_passthrough_and_localhost(void) {
    char ip[32];

    ASSERT_EQ(oops_net_resolve("192.168.1.100", ip, sizeof(ip)), 0);
    ASSERT_STR_EQ(ip, "192.168.1.100");

    ASSERT_EQ(oops_net_resolve("10.0.0.1", ip, sizeof(ip)), 0);
    ASSERT_STR_EQ(ip, "10.0.0.1");

    ASSERT_EQ(oops_net_resolve("localhost", ip, sizeof(ip)), 0);
    ASSERT_STR_EQ(ip, "127.0.0.1");
}

void run_unit_tests_dns(void) {
    TEST_SUITE_BEGIN("DNS Hostname Resolution & RFC 1035 Client");
    RUN_TEST(test_dns_null_and_bounds);
    RUN_TEST(test_dns_build_query);
    RUN_TEST(test_dns_parse_response);
    RUN_TEST(test_dns_passthrough_and_localhost);
    TEST_SUITE_END();
}
