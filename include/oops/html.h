#ifndef OOPS_HTML_H
#define OOPS_HTML_H

#include "oops/draw.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_html oops_html_t;

/*
 * Create an HTML renderer viewport of size width x height.
 * Initializes default CSS styling rules and layout engine.
 * Returns NULL on failure.
 */
oops_html_t *oops_html_create(int width, int height);

/*
 * Destroy the HTML renderer and release all DOM, CSS, and layout trees.
 */
void oops_html_destroy(oops_html_t *html);

/*
 * Update viewport dimensions and reflow the document.
 */
void oops_html_set_size(oops_html_t *html, int width, int height);

/*
 * Parse HTML5 text and compute stylesheet cascades and layout boxes.
 * `base_url` is used for resolving relative URLs (images, external CSS).
 * Returns 0 on success, or -1 on error.
 */
int oops_html_load(oops_html_t *html, const char *html_source, const char *base_url);

/*
 * Render the rendered HTML document into the provided surface.
 * Respects scroll offsets and clips drawing to viewport bounds.
 */
void oops_html_render(oops_html_t *html, oops_surface_t *surf);

/*
 * Scroll viewport by delta (dx, dy) pixels. Clamps to document dimensions.
 */
void oops_html_scroll(oops_html_t *html, int dx, int dy);

/*
 * Get the current vertical scroll position.
 */
int oops_html_get_scroll_y(const oops_html_t *html);

/*
 * Get the total content height of the laid out document.
 */
int oops_html_get_content_height(const oops_html_t *html);

/*
 * Hit-test a viewport coordinate (x, y).
 * If a link (anchor <a>) is present under the point, copies its href
 * attribute to `href_out` (up to href_len bytes) and returns 1.
 * Returns 0 if no link is hit.
 */
int oops_html_hit_test(oops_html_t *html, int x, int y, char *href_out, size_t href_len);

/* Advanced / C++ bridge: retrieve underlying litehtml pointers */
void *oops_html_get_document(oops_html_t *html);
void *oops_html_get_container(oops_html_t *html);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_HTML_H */
