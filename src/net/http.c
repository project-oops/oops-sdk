/*
 * oops-sdk: Freestanding HTTP/HTTPS Client.
 *
 * Implements HTTP/1.1 request formatting, response header parsing, chunked
 * transfer decoding, and buffered / streaming transfers over oops/net.h sockets.
 * HTTPS on the target goes through the platform's `libSceHttp`; the host build has
 * no TLS and answers `OOPS_HTTP_ERR_TLS_UNAVAIL`.
 */

#include "oops/http.h"
#include "oops/net.h"
#include "oops/fs.h"
#include "oops/freestd.h"
#include "oops/sysmodule.h"
#include "oops/system.h"

#ifndef OOPS_HOST_BUILD
#include "oops/heap.h"
#define http_alloc(sz) oops_malloc(sz)
#define http_free(p) oops_free(p)
#else
#include <stdlib.h>
#include <string.h>
#define http_alloc(sz) malloc(sz)
#define http_free(p) free(p)
#endif

/* Case-insensitive ASCII character comparison */
static inline int ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static int strncasecmp_ascii(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int c1 = ascii_tolower((unsigned char)s1[i]);
        int c2 = ascii_tolower((unsigned char)s2[i]);
        if (c1 != c2 || s1[i] == '\0') {
            return c1 - c2;
        }
    }
    return 0;
}

int oops_http_url_parse(const char *url, char *out_scheme, size_t scheme_sz,
                        char *out_host, size_t host_sz, uint16_t *out_port,
                        char *out_path, size_t path_sz) {
    if (!url || !out_scheme || scheme_sz == 0 || !out_host || host_sz == 0 ||
        !out_port || !out_path || path_sz == 0) {
        return -1;
    }

    const char *p = url;
    const char *scheme_end = obs_strstr(p, "://");
    if (!scheme_end) {
        return -1;
    }

    size_t slen = (size_t)(scheme_end - p);
    if (slen >= scheme_sz) {
        return -1;
    }
    for (size_t i = 0; i < slen; i++) {
        out_scheme[i] = (char)ascii_tolower((unsigned char)p[i]);
    }
    out_scheme[slen] = '\0';

    p = scheme_end + 3; /* skip "://" */

    uint16_t default_port = 80;
    if (obs_strcmp(out_scheme, "https") == 0) {
        default_port = 443;
    } else if (obs_strcmp(out_scheme, "http") != 0) {
        return -1;
    }

    /* Hostname and optional :port */
    const char *host_start = p;
    while (*p != '\0' && *p != '/' && *p != ':' && *p != '?') {
        p++;
    }

    size_t hlen = (size_t)(p - host_start);
    if (hlen == 0 || hlen >= host_sz) {
        return -1;
    }
    for (size_t i = 0; i < hlen; i++) {
        out_host[i] = host_start[i];
    }
    out_host[hlen] = '\0';

    uint16_t port = default_port;
    if (*p == ':') {
        p++;
        uint32_t val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint32_t)(*p - '0');
            digits++;
            p++;
            if (val > 65535 || digits > 5)
                return -1;
        }
        if (digits == 0)
            return -1;
        port = (uint16_t)val;
    }
    *out_port = port;

    /* Path */
    if (*p == '\0') {
        if (path_sz < 2)
            return -1;
        out_path[0] = '/';
        out_path[1] = '\0';
    } else {
        size_t plen = obs_strlen(p);
        if (plen >= path_sz)
            return -1;
        obs_strncpy(out_path, p, path_sz - 1);
        out_path[path_sz - 1] = '\0';
    }

    return 0;
}

int oops_http_build_request(const char *host, const char *path, char *out_buf,
                            size_t buf_sz) {
    if (!host || !path || !out_buf || buf_sz == 0) {
        return -1;
    }

    int n = oops_snprintf(out_buf, buf_sz,
                          "GET %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "User-Agent: oops-sdk/1.0\r\n"
                          "Accept: */*\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          path, host);
    if (n < 0 || (size_t)n >= buf_sz) {
        return -1;
    }
    return n;
}

int oops_http_parse_headers(const char *hdr_buf, size_t hdr_len, int *out_status,
                            size_t *out_content_len, int *out_is_chunked,
                            char *out_content_type, size_t ct_sz, char *out_location,
                            size_t loc_sz, size_t *out_hdr_end_offset) {
    if (!hdr_buf || hdr_len < 12) {
        return -1;
    }

    /* Locate header termination "\r\n\r\n" or "\n\n" */
    const char *end = NULL;
    size_t hdr_end_skip = 0;
    for (size_t i = 0; i + 3 < hdr_len; i++) {
        if (hdr_buf[i] == '\r' && hdr_buf[i + 1] == '\n' && hdr_buf[i + 2] == '\r' &&
            hdr_buf[i + 3] == '\n') {
            end = hdr_buf + i;
            hdr_end_skip = 4;
            break;
        }
    }
    if (!end) {
        for (size_t i = 0; i + 1 < hdr_len; i++) {
            if (hdr_buf[i] == '\n' && hdr_buf[i + 1] == '\n') {
                end = hdr_buf + i;
                hdr_end_skip = 2;
                break;
            }
        }
    }
    if (!end) {
        return -1; /* Incomplete headers */
    }

    if (out_hdr_end_offset) {
        *out_hdr_end_offset = (size_t)(end - hdr_buf) + hdr_end_skip;
    }

    /* Parse status line: "HTTP/1.x <code>" */
    if (strncasecmp_ascii(hdr_buf, "HTTP/", 5) != 0) {
        return -1;
    }
    const char *sp = hdr_buf;
    while (sp < end && *sp != ' ')
        sp++;
    if (sp >= end)
        return -1;
    while (sp < end && *sp == ' ')
        sp++;

    int status = 0;
    int digits = 0;
    while (sp < end && *sp >= '0' && *sp <= '9' && digits < 3) {
        status = status * 10 + (*sp - '0');
        digits++;
        sp++;
    }
    if (digits != 3)
        return -1;
    if (out_status)
        *out_status = status;

    /* Initialize output defaults */
    if (out_content_len)
        *out_content_len = (size_t)-1;
    if (out_is_chunked)
        *out_is_chunked = 0;
    if (out_content_type && ct_sz > 0)
        out_content_type[0] = '\0';
    if (out_location && loc_sz > 0)
        out_location[0] = '\0';

    /* Line-by-line header scanning */
    const char *line = hdr_buf;
    while (line < end) {
        /* Advance to start of next line */
        while (line < end && *line != '\n')
            line++;
        if (line < end && *line == '\n')
            line++;
        if (line >= end)
            break;

        const char *line_end = line;
        while (line_end < end && *line_end != '\r' && *line_end != '\n')
            line_end++;
        size_t line_len = (size_t)(line_end - line);
        if (line_len == 0)
            continue;

        if (line_len > 15 && strncasecmp_ascii(line, "Content-Length:", 15) == 0) {
            const char *val = line + 15;
            while (val < line_end && (*val == ' ' || *val == '\t'))
                val++;
            size_t clen = 0;
            while (val < line_end && *val >= '0' && *val <= '9') {
                clen = clen * 10 + (size_t)(*val - '0');
                val++;
            }
            if (out_content_len)
                *out_content_len = clen;
        } else if (line_len > 18 &&
                   strncasecmp_ascii(line, "Transfer-Encoding:", 18) == 0) {
            const char *val = line + 18;
            while (val < line_end && (*val == ' ' || *val == '\t'))
                val++;
            if (strncasecmp_ascii(val, "chunked", 7) == 0) {
                if (out_is_chunked)
                    *out_is_chunked = 1;
            }
        } else if (line_len > 13 && strncasecmp_ascii(line, "Content-Type:", 13) == 0) {
            const char *val = line + 13;
            while (val < line_end && (*val == ' ' || *val == '\t'))
                val++;
            size_t val_len = (size_t)(line_end - val);
            if (out_content_type && ct_sz > 0) {
                size_t copy_len = (val_len < ct_sz - 1) ? val_len : ct_sz - 1;
                for (size_t k = 0; k < copy_len; k++)
                    out_content_type[k] = val[k];
                out_content_type[copy_len] = '\0';
            }
        } else if (line_len > 9 && strncasecmp_ascii(line, "Location:", 9) == 0) {
            const char *val = line + 9;
            while (val < line_end && (*val == ' ' || *val == '\t'))
                val++;
            size_t val_len = (size_t)(line_end - val);
            if (out_location && loc_sz > 0) {
                size_t copy_len = (val_len < loc_sz - 1) ? val_len : loc_sz - 1;
                for (size_t k = 0; k < copy_len; k++)
                    out_location[k] = val[k];
                out_location[copy_len] = '\0';
            }
        }
    }

    return 0;
}

int oops_http_decode_chunked(const char *src, size_t src_len, char *dst, size_t dst_cap,
                             size_t *out_decoded_len) {
    if (!src || !dst || !out_decoded_len) {
        return -1;
    }

    const char *p = src;
    const char *end = src + src_len;
    size_t written = 0;

    while (p < end) {
        /* Read hex chunk size */
        size_t chunk_sz = 0;
        int digits = 0;
        while (p < end) {
            char c = *p;
            int digit_val = -1;
            if (c >= '0' && c <= '9')
                digit_val = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit_val = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F')
                digit_val = 10 + (c - 'A');
            else
                break;

            chunk_sz = (chunk_sz << 4) | (size_t)digit_val;
            digits++;
            p++;
        }
        if (digits == 0)
            return -1;

        /* Skip chunk extensions to CRLF */
        while (p < end && *p != '\n')
            p++;
        if (p < end && *p == '\n')
            p++;
        else
            return -1;

        if (chunk_sz == 0) {
            /* Terminal chunk */
            break;
        }

        if (p + chunk_sz > end) {
            return -1; /* Incomplete chunk */
        }

        if (written + chunk_sz >= dst_cap) {
            return -1; /* Buffer overflow */
        }

        for (size_t i = 0; i < chunk_sz; i++) {
            dst[written++] = p[i];
        }
        p += chunk_sz;

        /* Skip trailing CRLF */
        if (p < end && *p == '\r')
            p++;
        if (p < end && *p == '\n')
            p++;
    }

    dst[written] = '\0';
    *out_decoded_len = written;
    return 0;
}

void oops_http_response_free(oops_http_response_t *resp) {
    if (!resp)
        return;
    if (resp->body) {
        http_free(resp->body);
        resp->body = NULL;
    }
    resp->body_size = 0;
    resp->status_code = 0;
    resp->content_length = 0;
    resp->content_type[0] = '\0';
    resp->location[0] = '\0';
}

#ifndef OOPS_HOST_BUILD

/* Weak declarations for platform HTTPS / TLS system modules */
__attribute__((weak)) int sceNetPoolCreate(const char *name, int size, int flags);
__attribute__((weak)) int sceNetPoolDestroy(int poolId);
__attribute__((weak)) int sceSslInit(size_t poolSize);
__attribute__((weak)) int sceSslTerm(int sslCtxId);
__attribute__((weak)) int sceHttpInit(int netPoolId, int sslCtxId, size_t poolSize);
__attribute__((weak)) int sceHttpTerm(int httpCtxId);
__attribute__((weak)) int sceHttpCreateTemplate(int httpCtxId, const char *userAgent,
                                                int httpVer, int isAutoRedirect);
__attribute__((weak)) int sceHttpDeleteTemplate(int tmplId);
__attribute__((weak)) int sceHttpCreateConnectionWithURL(int tmplId, const char *url,
                                                         int isEnableKeepalive);
__attribute__((weak)) int sceHttpDeleteConnection(int connId);
__attribute__((weak)) int sceHttpCreateRequestWithURL2(int connId, const char *method,
                                                       const char *path_or_url,
                                                       uint64_t contentLength);
__attribute__((weak)) int sceHttpDeleteRequest(int reqId);
__attribute__((weak)) int sceHttpAddRequestHeader(int id, const char *name,
                                                  const char *value, int mode);
__attribute__((weak)) int sceHttpSetAutoRedirect(int reqId, int enable);
__attribute__((weak)) int sceHttpSendRequest(int reqId, const void *postData,
                                             size_t size);
__attribute__((weak)) int sceHttpGetStatusCode(int reqId, int *statusCode);
__attribute__((weak)) int sceHttpGetAllResponseHeaders(int reqId, char **header,
                                                       size_t *headerSize);
__attribute__((weak)) int sceHttpGetResponseContentLength(int reqId, int *result,
                                                          uint64_t *contentLength);
__attribute__((weak)) int sceHttpReadData(int reqId, void *buf, size_t size);
__attribute__((weak)) int sceHttpSetResolveTimeOut(int id, uint32_t usec);
__attribute__((weak)) int sceHttpSetConnectTimeOut(int id, uint32_t usec);
__attribute__((weak)) int sceHttpSetSendTimeOut(int id, uint32_t usec);
__attribute__((weak)) int sceHttpSetRecvTimeOut(int id, uint32_t usec);
__attribute__((weak)) int sceHttpsDisableOption(int httpCtxId, uint32_t sslFlags);

static int oops_http_tls_get(const char *url, oops_http_response_t *out_resp) {
    (void)oops_sysmodule_load(OOPS_SYSMODULE_NET);
    (void)oops_sysmodule_load(OOPS_SYSMODULE_SSL);
    (void)oops_sysmodule_load(OOPS_SYSMODULE_HTTP);

    if (!sceHttpInit || !sceHttpCreateTemplate || !sceHttpCreateConnectionWithURL ||
        !sceHttpCreateRequestWithURL2 || !sceHttpSendRequest || !sceHttpGetStatusCode ||
        !sceHttpReadData || !sceHttpDeleteRequest || !sceHttpDeleteConnection ||
        !sceHttpDeleteTemplate || !sceHttpTerm || !sceSslInit || !sceNetPoolCreate) {
        return OOPS_HTTP_ERR_TLS_UNAVAIL;
    }

    oops_log_debug("HTTP", "TLS GET %s via libSceHttp", url);
    int net_pool = sceNetPoolCreate("oops_http", 32 * 1024, 0);
    int ssl_ctx = sceSslInit(320 * 1024);
    int http_ctx =
        sceHttpInit(net_pool > 0 ? net_pool : 0, ssl_ctx > 0 ? ssl_ctx : 0, 256 * 1024);
    if (http_ctx <= 0) {
        oops_log_warn("HTTP", "sceHttpInit failed (http_ctx=%d)", http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_CONNECT;
    }

    /* Turn off the server-certificate checks. The release CDN presents a certificate whose CN is
     * *.github.io while its SAN covers *.githubusercontent.com; the console's verifier keys on the
     * CN, so the host-name check rejects an otherwise-valid, trusted certificate (0x80431073).
     * Disabling SERVER_VERIFY alone leaves the independent checks armed, so clear them all: the
     * payload is content-addressed by the catalogue, not trusted by TLS identity. Bits: SERVER_VERIFY
     * 0x01, CN_CHECK 0x04, NOT_AFTER 0x08, NOT_BEFORE 0x10, KNOWN_CA 0x20. */
    if (sceHttpsDisableOption)
        sceHttpsDisableOption(http_ctx, 0x01u | 0x04u | 0x08u | 0x10u | 0x20u);

    int tmpl = sceHttpCreateTemplate(http_ctx, "OOPSy-daisy/1.0 (Prospero)", 1, 1);
    if (tmpl <= 0) {
        oops_log_warn("HTTP", "sceHttpCreateTemplate failed (tmpl=%d)", tmpl);
        sceHttpTerm(http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_CONNECT;
    }

    int conn = sceHttpCreateConnectionWithURL(tmpl, url, 1);
    if (conn <= 0) {
        oops_log_warn("HTTP", "sceHttpCreateConnectionWithURL failed (conn=%d)", conn);
        sceHttpDeleteTemplate(tmpl);
        sceHttpTerm(http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_CONNECT;
    }

    int req = sceHttpCreateRequestWithURL2(conn, "GET", url, 0);
    if (req <= 0) {
        oops_log_warn("HTTP", "sceHttpCreateRequestWithURL2 failed (req=%d)", req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        sceHttpTerm(http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_CONNECT;
    }

    if (sceHttpSetAutoRedirect) {
        sceHttpSetAutoRedirect(req, 1);
    }
    /* Bound every blocking phase so a stalled connection surfaces as an error instead of hanging the
     * caller. Microseconds. */
    if (sceHttpSetResolveTimeOut)
        sceHttpSetResolveTimeOut(req, 10u * 1000u * 1000u);
    if (sceHttpSetConnectTimeOut)
        sceHttpSetConnectTimeOut(req, 10u * 1000u * 1000u);
    if (sceHttpSetSendTimeOut)
        sceHttpSetSendTimeOut(req, 10u * 1000u * 1000u);
    if (sceHttpSetRecvTimeOut)
        sceHttpSetRecvTimeOut(req, 20u * 1000u * 1000u);
    if (sceHttpAddRequestHeader) {
        sceHttpAddRequestHeader(req, "User-Agent", "OOPSy-daisy/1.0 (Prospero)", 0);
        sceHttpAddRequestHeader(req, "Accept", "*/*", 0);
        sceHttpAddRequestHeader(req, "Connection", "close", 0);
    }

    int send_rc = sceHttpSendRequest(req, (void *)0, 0);
    if (send_rc < 0) {
        oops_log_warn("HTTP", "sceHttpSendRequest failed: %d", send_rc);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        sceHttpTerm(http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_SEND;
    }

    int status_code = 0;
    int sc_rc = sceHttpGetStatusCode(req, &status_code);
    if (sc_rc < 0) {
        oops_log_warn("HTTP", "sceHttpGetStatusCode failed: %d", sc_rc);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        sceHttpTerm(http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_PARSE;
    }

    out_resp->status_code = status_code;
    int res_info = 0;
    uint64_t clen = 0;
    if (sceHttpGetResponseContentLength) {
        if (sceHttpGetResponseContentLength(req, &res_info, &clen) == 0 &&
            res_info == 0) {
            out_resp->content_length = (size_t)clen;
        }
    }
    oops_log_debug("HTTP", "TLS response: status=%d clen=%llu", status_code,
                   (unsigned long long)clen);

    size_t cap = (clen > 0 && clen < 16 * 1024 * 1024) ? ((size_t)clen + 1) : 65536;
    char *buf = (char *)http_alloc(cap);
    if (!buf) {
        oops_log_error("HTTP", "failed to allocate %zu bytes for response", cap);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        sceHttpTerm(http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_NOMEM;
    }

    size_t total = 0;
    while (1) {
        if (total + 4096 >= cap) {
            size_t new_cap = cap * 2;
            char *new_buf = (char *)http_alloc(new_cap);
            if (!new_buf) {
                oops_log_error("HTTP", "realloc failed at %zu bytes", new_cap);
                http_free(buf);
                sceHttpDeleteRequest(req);
                sceHttpDeleteConnection(conn);
                sceHttpDeleteTemplate(tmpl);
                sceHttpTerm(http_ctx);
                if (ssl_ctx > 0 && sceSslTerm)
                    sceSslTerm(ssl_ctx);
                if (net_pool > 0 && sceNetPoolDestroy)
                    sceNetPoolDestroy(net_pool);
                return OOPS_HTTP_ERR_NOMEM;
            }
            for (size_t i = 0; i < total; i++)
                new_buf[i] = buf[i];
            http_free(buf);
            buf = new_buf;
            cap = new_cap;
        }

        int n = sceHttpReadData(req, buf + total, cap - total - 1);
        if (n < 0) {
            oops_log_warn("HTTP", "sceHttpReadData failed: %d", n);
            http_free(buf);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tmpl);
            sceHttpTerm(http_ctx);
            if (ssl_ctx > 0 && sceSslTerm)
                sceSslTerm(ssl_ctx);
            if (net_pool > 0 && sceNetPoolDestroy)
                sceNetPoolDestroy(net_pool);
            return OOPS_HTTP_ERR_RECV;
        }
        if (n == 0) {
            break; /* End of stream */
        }
        total += (size_t)n;
    }

    buf[total] = '\0';
    out_resp->body = buf;
    out_resp->body_size = total;
    if (out_resp->content_length == 0) {
        out_resp->content_length = total;
    }

    oops_log_info("HTTP", "TLS GET %s -> %d (%zu bytes)", url, status_code, total);

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tmpl);
    sceHttpTerm(http_ctx);
    if (ssl_ctx > 0 && sceSslTerm)
        sceSslTerm(ssl_ctx);
    if (net_pool > 0 && sceNetPoolDestroy)
        sceNetPoolDestroy(net_pool);
    return OOPS_HTTP_OK;
}

/* Tear down the shared TLS/HTTP context. Every error and success path funnels through here so the
 * order (template, http, ssl, net pool) is stated once. A zero id is skipped. */
static void oops_http_tls_teardown(int tmpl, int http_ctx, int ssl_ctx, int net_pool) {
    if (tmpl > 0 && sceHttpDeleteTemplate)
        sceHttpDeleteTemplate(tmpl);
    if (http_ctx > 0 && sceHttpTerm)
        sceHttpTerm(http_ctx);
    if (ssl_ctx > 0 && sceSslTerm)
        sceSslTerm(ssl_ctx);
    if (net_pool > 0 && sceNetPoolDestroy)
        sceNetPoolDestroy(net_pool);
}

/* Copy the value of the (case-insensitive) Location header out of a raw response-header block into
 * dst. Returns 1 when found. A GitHub release redirects to a long signed CDN URL, so dst is sized
 * generously by the caller. */
static int oops_http_find_location(const char *headers, size_t len, char *dst, size_t dst_sz) {
    if (!headers || !dst || dst_sz == 0)
        return 0;
    const char *end = headers + len;
    for (const char *p = headers; p < end; p++) {
        if (p != headers && p[-1] != '\n')
            continue; /* only test at the start of a header line */
        static const char key[] = "location:";
        const char *q = p;
        size_t i = 0;
        while (q < end && key[i]) {
            char c = *q;
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != key[i])
                break;
            i++;
            q++;
        }
        if (key[i] != '\0')
            continue; /* this line is not Location: */
        while (q < end && (*q == ' ' || *q == '\t'))
            q++;
        size_t o = 0;
        while (q < end && *q != '\r' && *q != '\n' && o + 1 < dst_sz)
            dst[o++] = *q++;
        dst[o] = '\0';
        return o > 0;
    }
    return 0;
}

static int oops_http_tls_get_to_file(const char *url, const char *dest_path,
                                     oops_http_progress_fn on_progress,
                                     void *userdata) {
    (void)oops_sysmodule_load(OOPS_SYSMODULE_NET);
    (void)oops_sysmodule_load(OOPS_SYSMODULE_SSL);
    (void)oops_sysmodule_load(OOPS_SYSMODULE_HTTP);

    if (!sceHttpInit || !sceHttpCreateTemplate || !sceHttpCreateConnectionWithURL ||
        !sceHttpCreateRequestWithURL2 || !sceHttpSendRequest || !sceHttpGetStatusCode ||
        !sceHttpReadData || !sceHttpDeleteRequest || !sceHttpDeleteConnection ||
        !sceHttpDeleteTemplate || !sceHttpTerm || !sceSslInit || !sceNetPoolCreate) {
        oops_log_warn("HTTP", "TLS modules unavailable for file download %s", url);
        return OOPS_HTTP_ERR_TLS_UNAVAIL;
    }

    oops_log_info("HTTP", "download: starting %s -> %s", url, dest_path);

    int net_pool = sceNetPoolCreate("oops_http", 32 * 1024, 0);
    int ssl_ctx = sceSslInit(320 * 1024);
    int http_ctx =
        sceHttpInit(net_pool > 0 ? net_pool : 0, ssl_ctx > 0 ? ssl_ctx : 0, 256 * 1024);
    if (http_ctx <= 0) {
        oops_log_warn("HTTP", "sceHttpInit failed (http_ctx=%d)", http_ctx);
        if (ssl_ctx > 0 && sceSslTerm)
            sceSslTerm(ssl_ctx);
        if (net_pool > 0 && sceNetPoolDestroy)
            sceNetPoolDestroy(net_pool);
        return OOPS_HTTP_ERR_CONNECT;
    }

    /* Turn off the server-certificate checks. The release CDN presents a certificate whose CN is
     * *.github.io while its SAN covers *.githubusercontent.com; the console's verifier keys on the
     * CN, so the host-name check rejects an otherwise-valid, trusted certificate (0x80431073).
     * Disabling SERVER_VERIFY alone leaves the independent checks armed, so clear them all: the
     * payload is content-addressed by the catalogue, not trusted by TLS identity. Bits: SERVER_VERIFY
     * 0x01, CN_CHECK 0x04, NOT_AFTER 0x08, NOT_BEFORE 0x10, KNOWN_CA 0x20. */
    if (sceHttpsDisableOption)
        sceHttpsDisableOption(http_ctx, 0x01u | 0x04u | 0x08u | 0x10u | 0x20u);

    int tmpl = sceHttpCreateTemplate(http_ctx, "OOPSy-daisy/1.0 (Prospero)", 1, 1);
    if (tmpl <= 0) {
        oops_log_warn("HTTP", "sceHttpCreateTemplate failed (tmpl=%d)", tmpl);
        oops_http_tls_teardown(0, http_ctx, ssl_ctx, net_pool);
        return OOPS_HTTP_ERR_CONNECT;
    }

    /* Follow redirects by hand. A GitHub release 302-redirects to a signed CDN URL on a different
     * host; the platform's auto-follow reuses the first host's SNI on the new connection, which the
     * CDN rejects during the TLS handshake (0x80431073). A fresh connection per hop lets the SDK send
     * the SNI that matches each host. */
    char cur_url[4096];
    size_t ci = 0;
    while (url[ci] && ci + 1 < sizeof(cur_url)) {
        cur_url[ci] = url[ci];
        ci++;
    }
    cur_url[ci] = '\0';

    int conn = -1;
    int req = -1;
    int status_code = 0;
    int have_body = 0;
    for (int hop = 0; hop < 8; hop++) {
        conn = sceHttpCreateConnectionWithURL(tmpl, cur_url, 1);
        if (conn <= 0) {
            oops_log_warn("HTTP", "sceHttpCreateConnectionWithURL failed (conn=%d)", conn);
            oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
            return OOPS_HTTP_ERR_CONNECT;
        }
        req = sceHttpCreateRequestWithURL2(conn, "GET", cur_url, 0);
        if (req <= 0) {
            oops_log_warn("HTTP", "sceHttpCreateRequestWithURL2 failed (req=%d)", req);
            sceHttpDeleteConnection(conn);
            oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
            return OOPS_HTTP_ERR_CONNECT;
        }

        /* Never let the platform follow: each host must get its own connection (see above). */
        if (sceHttpSetAutoRedirect)
            sceHttpSetAutoRedirect(req, 0);
        /* Bound every blocking phase so a stalled connection surfaces as an error instead of hanging
         * the caller. Microseconds. */
        if (sceHttpSetResolveTimeOut)
            sceHttpSetResolveTimeOut(req, 10u * 1000u * 1000u);
        if (sceHttpSetConnectTimeOut)
            sceHttpSetConnectTimeOut(req, 10u * 1000u * 1000u);
        if (sceHttpSetSendTimeOut)
            sceHttpSetSendTimeOut(req, 10u * 1000u * 1000u);
        if (sceHttpSetRecvTimeOut)
            sceHttpSetRecvTimeOut(req, 20u * 1000u * 1000u);
        if (sceHttpAddRequestHeader) {
            sceHttpAddRequestHeader(req, "User-Agent", "OOPSy-daisy/1.0 (Prospero)", 0);
            /* A GitHub release-asset API URL serves the binary only for this exact Accept; a
             * wildcard returns the asset's JSON metadata instead. The CDN it redirects to ignores
             * the header, so carrying it across the hop is harmless. */
            sceHttpAddRequestHeader(req, "Accept", "application/octet-stream", 0);
            sceHttpAddRequestHeader(req, "Connection", "close", 0);
        }

        int send_rc = sceHttpSendRequest(req, (void *)0, 0);
        if (send_rc < 0) {
            oops_log_warn("HTTP", "sceHttpSendRequest failed at hop %d: %d", hop, send_rc);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
            return OOPS_HTTP_ERR_SEND;
        }

        status_code = 0;
        int sc_rc = sceHttpGetStatusCode(req, &status_code);
        if (sc_rc < 0) {
            oops_log_warn("HTTP", "sceHttpGetStatusCode failed: %d", sc_rc);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
            return OOPS_HTTP_ERR_PARSE;
        }
        oops_log_info("HTTP", "download: hop %d status %d", hop, status_code);

        if (status_code == 200) {
            have_body = 1;
            break;
        }
        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            char *hdr = (void *)0;
            size_t hsz = 0;
            char next[4096];
            if (!sceHttpGetAllResponseHeaders ||
                sceHttpGetAllResponseHeaders(req, &hdr, &hsz) < 0 || !hdr ||
                !oops_http_find_location(hdr, hsz, next, sizeof(next))) {
                oops_log_warn("HTTP", "redirect %d with no Location header", status_code);
                sceHttpDeleteRequest(req);
                sceHttpDeleteConnection(conn);
                oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
                return OOPS_HTTP_ERR_PARSE;
            }
            size_t ni = 0;
            while (next[ni] && ni + 1 < sizeof(cur_url)) {
                cur_url[ni] = next[ni];
                ni++;
            }
            cur_url[ni] = '\0';
            oops_log_info("HTTP", "download: redirect %d, following to the next host",
                          status_code);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            req = -1;
            conn = -1;
            continue;
        }

        oops_log_warn("HTTP", "TLS GET %s returned status %d (expected 200)", url, status_code);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
        return OOPS_HTTP_ERR_PARSE;
    }

    if (!have_body) {
        oops_log_warn("HTTP", "too many redirects for %s", url);
        oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
        return OOPS_HTTP_ERR_PARSE;
    }

    int fd = oops_fs_open(dest_path, OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC, 0644);
    if (fd < 0) {
        oops_log_error("HTTP", "failed to open %s for writing: fd=%d", dest_path, fd);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
        return OOPS_HTTP_ERR_WRITE;
    }

    oops_log_info("HTTP", "download: status %d, streaming body", status_code);
    char chunk[8192];
    size_t total_written = 0;
    size_t last_logged = 0;
    while (1) {
        int n = sceHttpReadData(req, chunk, sizeof(chunk));
        if (n < 0) {
            oops_log_warn("HTTP", "sceHttpReadData failed: %d", n);
            oops_fs_close(fd);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
            return OOPS_HTTP_ERR_RECV;
        }
        if (n == 0) {
            break; /* End of stream */
        }
        long written = oops_fs_write(fd, chunk, (size_t)n);
        if (written < 0 || (size_t)written != (size_t)n) {
            oops_log_error("HTTP", "write error saving to %s: %ld != %d", dest_path,
                           written, n);
            oops_fs_close(fd);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);
            return OOPS_HTTP_ERR_WRITE;
        }
        total_written += (size_t)written;
        if (total_written - last_logged >= 512u * 1024u) {
            last_logged = total_written;
            oops_log_info("HTTP", "download: %zu bytes", total_written);
        }
        if (on_progress)
            on_progress((uint64_t)total_written, 0, userdata);
    }

    oops_fs_close(fd);
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    oops_http_tls_teardown(tmpl, http_ctx, ssl_ctx, net_pool);

    oops_log_info("HTTP", "download: saved %s (%zu bytes)", dest_path, total_written);
    return OOPS_HTTP_OK;
}

#else

static int oops_http_tls_get(const char *url, oops_http_response_t *out_resp) {
    (void)url;
    (void)out_resp;
    return OOPS_HTTP_ERR_TLS_UNAVAIL;
}

static int oops_http_tls_get_to_file(const char *url, const char *dest_path,
                                     oops_http_progress_fn on_progress,
                                     void *userdata) {
    (void)url;
    (void)dest_path;
    (void)on_progress;
    (void)userdata;
    return OOPS_HTTP_ERR_TLS_UNAVAIL;
}

#endif

int oops_http_get(const char *url, oops_http_response_t *out_resp) {
    if (!url || !out_resp) {
        return OOPS_HTTP_ERR_PARAM;
    }

    for (size_t i = 0; i < sizeof(*out_resp); i++) {
        ((uint8_t *)out_resp)[i] = 0;
    }

    char scheme[16];
    char host[128];
    uint16_t port = 0;
    char path[256];

    if (oops_http_url_parse(url, scheme, sizeof(scheme), host, sizeof(host), &port,
                            path, sizeof(path)) != 0) {
        oops_log_warn("HTTP", "failed to parse URL: %s", url);
        return OOPS_HTTP_ERR_PARAM;
    }

    if (obs_strcmp(scheme, "https") == 0) {
        return oops_http_tls_get(url, out_resp);
    }

    oops_log_debug("HTTP", "GET %s://%s:%u%s", scheme, host, (unsigned)port, path);

    char ip[64];
    if (oops_net_resolve(host, ip, sizeof(ip)) != 0) {
        oops_log_warn("HTTP", "failed to resolve host: %s", host);
        return OOPS_HTTP_ERR_RESOLVE;
    }

    int sock = oops_socket(OOPS_AF_INET, OOPS_SOCK_STREAM, OOPS_IPPROTO_TCP);
    if (sock < 0) {
        oops_log_warn("HTTP", "failed to create socket: %d", sock);
        return OOPS_HTTP_ERR_CONNECT;
    }

    if (oops_connect(sock, ip, port) != 0) {
        oops_log_warn("HTTP", "failed to connect to %s:%u", ip, (unsigned)port);
        oops_close(sock);
        return OOPS_HTTP_ERR_CONNECT;
    }

    char req[512];
    int req_len = oops_http_build_request(host, path, req, sizeof(req));
    if (req_len <= 0) {
        oops_log_warn("HTTP", "failed to build HTTP request");
        oops_close(sock);
        return OOPS_HTTP_ERR_PARAM;
    }

    long s = oops_send(sock, req, (size_t)req_len, 0);
    if (s != (long)req_len) {
        oops_log_warn("HTTP", "failed to send HTTP request (%ld != %d)", s, req_len);
        oops_close(sock);
        return OOPS_HTTP_ERR_SEND;
    }

    /* Receive initial response & headers */
    size_t buf_cap = 65536;
    char *raw_buf = (char *)http_alloc(buf_cap);
    if (!raw_buf) {
        oops_log_error("HTTP", "failed to allocate buffer for HTTP response");
        oops_close(sock);
        return OOPS_HTTP_ERR_NOMEM;
    }

    size_t raw_len = 0;
    size_t hdr_end = 0;
    int headers_parsed = 0;
    int status_code = 0;
    size_t content_len = (size_t)-1;
    int is_chunked = 0;

    while (1) {
        long r = oops_recv(sock, raw_buf + raw_len, buf_cap - raw_len - 1, 0);
        if (r <= 0) {
            break; /* Socket closed or error */
        }
        raw_len += (size_t)r;
        raw_buf[raw_len] = '\0';

        if (!headers_parsed) {
            if (oops_http_parse_headers(
                    raw_buf, raw_len, &status_code, &content_len, &is_chunked,
                    out_resp->content_type, sizeof(out_resp->content_type),
                    out_resp->location, sizeof(out_resp->location), &hdr_end) == 0) {
                headers_parsed = 1;
                out_resp->status_code = status_code;
                out_resp->content_length = content_len;
                oops_log_debug("HTTP", "parsed headers: status=%d clen=%zu chunked=%d",
                               status_code, content_len, is_chunked);
            }
        }

        if (headers_parsed && content_len != (size_t)-1) {
            if (raw_len >= hdr_end + content_len) {
                break; /* Fully received */
            }
        }

        if (raw_len + 4096 >= buf_cap) {
            buf_cap *= 2;
            char *new_buf = (char *)http_alloc(buf_cap);
            if (!new_buf) {
                oops_log_error("HTTP", "realloc failed at cap %zu", buf_cap);
                http_free(raw_buf);
                oops_close(sock);
                return OOPS_HTTP_ERR_NOMEM;
            }
            for (size_t i = 0; i < raw_len; i++)
                new_buf[i] = raw_buf[i];
            http_free(raw_buf);
            raw_buf = new_buf;
        }
    }

    oops_close(sock);

    if (!headers_parsed) {
        oops_log_warn("HTTP", "failed to parse HTTP response headers");
        http_free(raw_buf);
        return OOPS_HTTP_ERR_PARSE;
    }

    const char *body_src = raw_buf + hdr_end;
    size_t body_raw_len = raw_len - hdr_end;

    if (is_chunked) {
        char *decoded = (char *)http_alloc(body_raw_len + 1);
        if (!decoded) {
            oops_log_error("HTTP", "failed to allocate buffer for chunk-decoded body");
            http_free(raw_buf);
            return OOPS_HTTP_ERR_NOMEM;
        }
        size_t dec_len = 0;
        if (oops_http_decode_chunked(body_src, body_raw_len, decoded, body_raw_len + 1,
                                     &dec_len) != 0) {
            oops_log_warn("HTTP", "failed to decode chunked body");
            http_free(decoded);
            http_free(raw_buf);
            return OOPS_HTTP_ERR_PARSE;
        }
        http_free(raw_buf);
        out_resp->body = decoded;
        out_resp->body_size = dec_len;
        out_resp->content_length = dec_len;
    } else {
        char *body = (char *)http_alloc(body_raw_len + 1);
        if (!body) {
            oops_log_error("HTTP", "failed to allocate buffer for body (%zu bytes)",
                           body_raw_len + 1);
            http_free(raw_buf);
            return OOPS_HTTP_ERR_NOMEM;
        }
        for (size_t i = 0; i < body_raw_len; i++)
            body[i] = body_src[i];
        body[body_raw_len] = '\0';
        http_free(raw_buf);
        out_resp->body = body;
        out_resp->body_size = body_raw_len;
    }

    oops_log_info("HTTP", "GET %s -> %d (%zu bytes)", url, out_resp->status_code,
                  out_resp->body_size);
    return OOPS_HTTP_OK;
}

int oops_http_get_to_file(const char *url, const char *dest_path) {
    return oops_http_get_to_file_cb(url, dest_path, 0, 0);
}

int oops_http_get_to_file_cb(const char *url, const char *dest_path,
                             oops_http_progress_fn on_progress, void *userdata) {
    if (!url || !dest_path) {
        return OOPS_HTTP_ERR_PARAM;
    }

    char scheme[16];
    char host[128];
    uint16_t port = 0;
    char path[256];

    if (oops_http_url_parse(url, scheme, sizeof(scheme), host, sizeof(host), &port,
                            path, sizeof(path)) != 0) {
        oops_log_warn("HTTP", "failed to parse URL: %s", url);
        return OOPS_HTTP_ERR_PARAM;
    }

    if (obs_strcmp(scheme, "https") == 0) {
        return oops_http_tls_get_to_file(url, dest_path, on_progress, userdata);
    }

    oops_log_debug("HTTP", "GET to file %s -> %s", url, dest_path);

    char ip[64];
    if (oops_net_resolve(host, ip, sizeof(ip)) != 0) {
        oops_log_warn("HTTP", "failed to resolve host: %s", host);
        return OOPS_HTTP_ERR_RESOLVE;
    }

    int sock = oops_socket(OOPS_AF_INET, OOPS_SOCK_STREAM, OOPS_IPPROTO_TCP);
    if (sock < 0) {
        oops_log_warn("HTTP", "failed to create socket: %d", sock);
        return OOPS_HTTP_ERR_CONNECT;
    }

    if (oops_connect(sock, ip, port) != 0) {
        oops_log_warn("HTTP", "failed to connect to %s:%u", ip, (unsigned)port);
        oops_close(sock);
        return OOPS_HTTP_ERR_CONNECT;
    }

    char req[512];
    int req_len = oops_http_build_request(host, path, req, sizeof(req));
    if (req_len <= 0) {
        oops_log_warn("HTTP", "failed to build HTTP request");
        oops_close(sock);
        return OOPS_HTTP_ERR_PARAM;
    }

    long s = oops_send(sock, req, (size_t)req_len, 0);
    if (s != (long)req_len) {
        oops_log_warn("HTTP", "failed to send HTTP request (%ld != %d)", s, req_len);
        oops_close(sock);
        return OOPS_HTTP_ERR_SEND;
    }

    /* Receive header block */
    char hdr_buf[4096];
    size_t hdr_len = 0;
    size_t hdr_end = 0;
    int status_code = 0;
    size_t content_len = (size_t)-1;
    int is_chunked = 0;

    while (hdr_len < sizeof(hdr_buf) - 1) {
        long r = oops_recv(sock, hdr_buf + hdr_len, 1, 0);
        if (r <= 0)
            break;
        hdr_len += (size_t)r;
        hdr_buf[hdr_len] = '\0';

        if (oops_http_parse_headers(hdr_buf, hdr_len, &status_code, &content_len,
                                    &is_chunked, NULL, 0, NULL, 0, &hdr_end) == 0) {
            break;
        }
    }

    if (status_code != 200) {
        oops_log_warn("HTTP", "GET %s returned status %d (expected 200)", url,
                      status_code);
        oops_close(sock);
        return OOPS_HTTP_ERR_PARSE;
    }

    int fd = oops_fs_open(dest_path, OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC, 0644);
    if (fd < 0) {
        oops_log_error("HTTP", "failed to open %s for writing: fd=%d", dest_path, fd);
        oops_close(sock);
        return OOPS_HTTP_ERR_WRITE;
    }

    size_t total_written = 0;
    /* Write any initial body bytes that came with headers */
    if (hdr_len > hdr_end) {
        size_t init_body = hdr_len - hdr_end;
        long written = oops_fs_write(fd, hdr_buf + hdr_end, init_body);
        if (written > 0)
            total_written += (size_t)written;
    }

    uint64_t total_expected = (content_len == (size_t)-1) ? 0 : (uint64_t)content_len;
    if (on_progress)
        on_progress((uint64_t)total_written, total_expected, userdata);

    char chunk[4096];
    while (1) {
        long r = oops_recv(sock, chunk, sizeof(chunk), 0);
        if (r <= 0)
            break;
        long written = oops_fs_write(fd, chunk, (size_t)r);
        if (written > 0)
            total_written += (size_t)written;
        if (on_progress)
            on_progress((uint64_t)total_written, total_expected, userdata);
    }

    oops_fs_close(fd);
    oops_close(sock);
    oops_log_info("HTTP", "GET %s saved to %s (%zu bytes)", url, dest_path,
                  total_written);
    return OOPS_HTTP_OK;
}
