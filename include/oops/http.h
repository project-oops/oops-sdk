#ifndef OOPS_HTTP_H
#define OOPS_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result status codes */
#define OOPS_HTTP_OK 0
#define OOPS_HTTP_ERR_PARAM (-1)
#define OOPS_HTTP_ERR_RESOLVE (-2)
#define OOPS_HTTP_ERR_CONNECT (-3)
#define OOPS_HTTP_ERR_SEND (-4)
#define OOPS_HTTP_ERR_RECV (-5)
#define OOPS_HTTP_ERR_PARSE (-6)
#define OOPS_HTTP_ERR_NOMEM (-7)
#define OOPS_HTTP_ERR_WRITE (-8)
#define OOPS_HTTP_ERR_UNSUPPORTED (-9)
#define OOPS_HTTP_ERR_TLS_UNAVAIL (-10)

/**
 * Parsed HTTP response data.
 */
typedef struct oops_http_response {
    int status_code;       /* HTTP status code, e.g. 200, 301, 404 */
    size_t content_length; /* Content-Length header or decoded body length */
    char *body;            /* Null-terminated response payload (caller frees via
                              oops_http_response_free) */
    size_t body_size;      /* Size of body payload in bytes */
    char content_type[64]; /* Content-Type header string */
    char location[256];    /* Location header for HTTP redirects */
} oops_http_response_t;

/**
 * Fetch a URL via HTTP GET and store the response in `out_resp`.
 * Allocates `out_resp->body` on success; caller must release via
 * `oops_http_response_free()`.
 *
 * Returns OOPS_HTTP_OK (0) on success, or a negative OOPS_HTTP_ERR_* code.
 */
int oops_http_get(const char *url, oops_http_response_t *out_resp);

/**
 * Fetch a URL via HTTP GET and stream the response body directly to `dest_path`.
 * Files and directories are written using oops/fs.h.
 *
 * Returns OOPS_HTTP_OK (0) on success, or a negative OOPS_HTTP_ERR_* code.
 */
int oops_http_get_to_file(const char *url, const char *dest_path);

/**
 * Download progress callback: invoked as the body streams to disk, with the bytes
 * written so far and the total expected (`total` is 0 when the server sends no
 * Content-Length). Called on the same thread as the download, so it may draw a frame;
 * keep it quick. `userdata` is passed through unchanged.
 */
typedef void (*oops_http_progress_fn)(uint64_t downloaded, uint64_t total,
                                      void *userdata);

/**
 * Like `oops_http_get_to_file`, but reports progress: `on_progress` (may be NULL) is
 * called after each chunk is written, and once at the end. Lets a caller draw a live
 * progress bar during an otherwise-blocking download without a worker thread.
 *
 * Returns OOPS_HTTP_OK (0) on success, or a negative OOPS_HTTP_ERR_* code.
 */
int oops_http_get_to_file_cb(const char *url, const char *dest_path,
                             oops_http_progress_fn on_progress, void *userdata);

/**
 * Free any memory buffers allocated inside an `oops_http_response_t`.
 */
void oops_http_response_free(oops_http_response_t *resp);

/**
 * Parse an HTTP/HTTPS URL into scheme, hostname, port and request path.
 * If port is omitted in the URL, defaults to 80 for "http" and 443 for "https".
 * If path is omitted, defaults to "/".
 *
 * Returns 0 on success, or -1 on invalid parameter/URL.
 */
int oops_http_url_parse(const char *url, char *out_scheme, size_t scheme_sz,
                        char *out_host, size_t host_sz, uint16_t *out_port,
                        char *out_path, size_t path_sz);

/**
 * Construct a standard HTTP/1.1 GET request into `out_buf`.
 *
 * Returns number of bytes formatted on success, or -1 if buffer is too small.
 */
int oops_http_build_request(const char *host, const char *path, char *out_buf,
                            size_t buf_sz);

/**
 * Parse HTTP response status line and headers.
 * Extracts status code, Content-Length, chunked transfer flag, Content-Type,
 * and Location header. `out_hdr_end_offset` receives the byte offset immediately
 * following the "\r\n\r\n" delimiter.
 *
 * Returns 0 on success, or -1 on parse failure.
 */
int oops_http_parse_headers(const char *hdr_buf, size_t hdr_len, int *out_status,
                            size_t *out_content_len, int *out_is_chunked,
                            char *out_content_type, size_t ct_sz, char *out_location,
                            size_t loc_sz, size_t *out_hdr_end_offset);

/**
 * Decode an RFC 7230 chunked transfer-encoded stream in `src` into `dst`.
 *
 * Returns 0 on success, or -1 on invalid chunked stream.
 */
int oops_http_decode_chunked(const char *src, size_t src_len, char *dst, size_t dst_cap,
                             size_t *out_decoded_len);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_HTTP_H */
