#include "oops/http.h"
#include "oops/freestd.h"
#include "tests/test_common.h"
#include <string.h>

static void test_http_url_parse(void) {
    char scheme[16];
    char host[64];
    uint16_t port = 0;
    char path[128];

    /* 1. Basic HTTP URL */
    ASSERT_EQ(oops_http_url_parse("http://example.com/test/index.html", scheme,
                                  sizeof(scheme), host, sizeof(host), &port, path,
                                  sizeof(path)),
              0);
    ASSERT_STR_EQ(scheme, "http");
    ASSERT_STR_EQ(host, "example.com");
    ASSERT_EQ(port, 80);
    ASSERT_STR_EQ(path, "/test/index.html");

    /* 2. Custom Port */
    ASSERT_EQ(oops_http_url_parse("http://192.168.1.50:8080/api/apps.json", scheme,
                                  sizeof(scheme), host, sizeof(host), &port, path,
                                  sizeof(path)),
              0);
    ASSERT_STR_EQ(scheme, "http");
    ASSERT_STR_EQ(host, "192.168.1.50");
    ASSERT_EQ(port, 8080);
    ASSERT_STR_EQ(path, "/api/apps.json");

    /* 3. HTTPS default port 443 */
    ASSERT_EQ(oops_http_url_parse("https://api.github.com/repos/releases", scheme,
                                  sizeof(scheme), host, sizeof(host), &port, path,
                                  sizeof(path)),
              0);
    ASSERT_STR_EQ(scheme, "https");
    ASSERT_STR_EQ(host, "api.github.com");
    ASSERT_EQ(port, 443);
    ASSERT_STR_EQ(path, "/repos/releases");

    /* 4. Bare host without trailing slash */
    ASSERT_EQ(oops_http_url_parse("http://localhost", scheme, sizeof(scheme), host,
                                  sizeof(host), &port, path, sizeof(path)),
              0);
    ASSERT_STR_EQ(scheme, "http");
    ASSERT_STR_EQ(host, "localhost");
    ASSERT_EQ(port, 80);
    ASSERT_STR_EQ(path, "/");

    /* 5. Invalid URLs */
    ASSERT_EQ(oops_http_url_parse(NULL, scheme, sizeof(scheme), host, sizeof(host),
                                  &port, path, sizeof(path)),
              -1);
    ASSERT_EQ(oops_http_url_parse("ftp://example.com/file", scheme, sizeof(scheme),
                                  host, sizeof(host), &port, path, sizeof(path)),
              -1);
    ASSERT_EQ(oops_http_url_parse("not_a_url", scheme, sizeof(scheme), host,
                                  sizeof(host), &port, path, sizeof(path)),
              -1);
    ASSERT_EQ(oops_http_url_parse("http://", scheme, sizeof(scheme), host, sizeof(host),
                                  &port, path, sizeof(path)),
              -1);
    ASSERT_EQ(oops_http_url_parse("http://host:999999/path", scheme, sizeof(scheme),
                                  host, sizeof(host), &port, path, sizeof(path)),
              -1);
}

static void test_http_build_request(void) {
    char buf[256];
    int n = oops_http_build_request("example.com", "/index.html", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(obs_strstr(buf, "GET /index.html HTTP/1.1\r\n") != NULL);
    ASSERT_TRUE(obs_strstr(buf, "Host: example.com\r\n") != NULL);
    ASSERT_TRUE(obs_strstr(buf, "Connection: close\r\n\r\n") != NULL);

    /* Buffer too small */
    char tiny[10];
    ASSERT_EQ(oops_http_build_request("example.com", "/index.html", tiny, sizeof(tiny)),
              -1);
}

static void test_http_parse_headers(void) {
    const char resp1[] = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: 42\r\n"
                         "Server: test\r\n"
                         "\r\n"
                         "{\"status\":\"ok\",\"count\":5}";

    int status = 0;
    size_t content_len = 0;
    int is_chunked = 0;
    char ct[64];
    char loc[64];
    size_t hdr_end = 0;

    ASSERT_EQ(oops_http_parse_headers(resp1, strlen(resp1), &status, &content_len,
                                      &is_chunked, ct, sizeof(ct), loc, sizeof(loc),
                                      &hdr_end),
              0);
    ASSERT_EQ(status, 200);
    ASSERT_EQ(content_len, 42);
    ASSERT_EQ(is_chunked, 0);
    ASSERT_STR_EQ(ct, "application/json");
    ASSERT_STR_EQ(resp1 + hdr_end, "{\"status\":\"ok\",\"count\":5}");

    /* 302 Redirect with Location header */
    const char resp2[] = "HTTP/1.1 302 Found\r\n"
                         "Location: https://example.com/new_path\r\n"
                         "Content-Length: 0\r\n"
                         "\r\n";

    ASSERT_EQ(oops_http_parse_headers(resp2, strlen(resp2), &status, &content_len,
                                      &is_chunked, ct, sizeof(ct), loc, sizeof(loc),
                                      &hdr_end),
              0);
    ASSERT_EQ(status, 302);
    ASSERT_EQ(content_len, 0);
    ASSERT_STR_EQ(loc, "https://example.com/new_path");

    /* Chunked Transfer-Encoding */
    const char resp3[] = "HTTP/1.1 200 OK\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "Content-Type: text/plain\r\n"
                         "\r\n";

    ASSERT_EQ(oops_http_parse_headers(resp3, strlen(resp3), &status, &content_len,
                                      &is_chunked, ct, sizeof(ct), loc, sizeof(loc),
                                      &hdr_end),
              0);
    ASSERT_EQ(status, 200);
    ASSERT_EQ(is_chunked, 1);

    /* Incomplete header without terminal CRLF CRLF */
    const char incomplete[] = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n";
    ASSERT_EQ(oops_http_parse_headers(incomplete, strlen(incomplete), &status,
                                      &content_len, &is_chunked, ct, sizeof(ct), loc,
                                      sizeof(loc), &hdr_end),
              -1);
}

static void test_http_decode_chunked(void) {
    /* RFC 7230 Chunked stream:
     * 4\r\nWiki\r\n6\r\npedia \r\n9\r\nin chunks.\r\n0\r\n\r\n
     */
    const char chunked[] = "4\r\nWiki\r\n"
                           "6\r\npedia \r\n"
                           "a\r\nin chunks!\r\n"
                           "0\r\n\r\n";

    char decoded[128];
    size_t dec_len = 0;

    ASSERT_EQ(oops_http_decode_chunked(chunked, strlen(chunked), decoded,
                                       sizeof(decoded), &dec_len),
              0);
    ASSERT_EQ(dec_len, 20);
    ASSERT_STR_EQ(decoded, "Wikipedia in chunks!");

    /* Malformed chunked stream */
    const char bad_chunk[] = "ZZZ\r\nBad chunk\r\n0\r\n\r\n";
    ASSERT_EQ(oops_http_decode_chunked(bad_chunk, strlen(bad_chunk), decoded,
                                       sizeof(decoded), &dec_len),
              -1);
}

static void test_http_api_contracts(void) {
    ASSERT_EQ(oops_http_get(NULL, NULL), OOPS_HTTP_ERR_PARAM);

    oops_http_response_t resp;
    ASSERT_EQ(oops_http_get("not_a_url", &resp), OOPS_HTTP_ERR_PARAM);

    /* HTTPS without live hardware TLS reports TLS unavailable */
    ASSERT_EQ(oops_http_get("https://example.com/test", &resp),
              OOPS_HTTP_ERR_TLS_UNAVAIL);
    ASSERT_EQ(oops_http_get_to_file("https://example.com/test", "/tmp/out"),
              OOPS_HTTP_ERR_TLS_UNAVAIL);

    /* oops_http_response_free is safe with empty struct and NULL */
    oops_http_response_free(NULL);
    memset(&resp, 0, sizeof(resp));
    oops_http_response_free(&resp);
}

void run_unit_tests_http(void) {
    TEST_SUITE_BEGIN("Freestanding HTTP/HTTPS Client Engine");
    RUN_TEST(test_http_url_parse);
    RUN_TEST(test_http_build_request);
    RUN_TEST(test_http_parse_headers);
    RUN_TEST(test_http_decode_chunked);
    RUN_TEST(test_http_api_contracts);
}
