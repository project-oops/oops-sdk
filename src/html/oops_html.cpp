/* The C API of oops/html.h over litehtml: an `oops_html_t` owns one container, which
 * owns the loaded document. Loading and resizing lay the document out at the view's
 * width. */

#include "oops/html.h"
#include "html_container.hpp"
#include "litehtml/master_css.h"
#include "oops/system.h"
#include <cstring>

struct oops_html {
    std::unique_ptr<oops_container> container;
};

extern "C" {

oops_html_t *oops_html_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        oops_log_error("HTML", "Invalid dimensions: %dx%d", width, height);
        return NULL;
    }

    auto *html = new (std::nothrow) oops_html();
    if (!html) {
        oops_log_error("HTML", "Failed to allocate oops_html");
        return NULL;
    }

    html->container = std::make_unique<oops_container>(width, height);
    oops_log_info("HTML", "Initialized HTML renderer: %dx%d", width, height);
    return html;
}

void oops_html_destroy(oops_html_t *html) {
    if (!html)
        return;
    delete html;
    oops_log_debug("HTML", "Destroyed HTML renderer");
}

void oops_html_set_size(oops_html_t *html, int width, int height) {
    if (!html || !html->container || width <= 0 || height <= 0)
        return;
    html->container->m_width = width;
    html->container->m_height = height;
    if (html->container->m_doc) {
        html->container->m_doc->render(width);
    }
}

int oops_html_load(oops_html_t *html, const char *html_source, const char *base_url) {
    if (!html || !html->container || !html_source) {
        oops_log_error("HTML", "oops_html_load called with invalid arguments");
        return -1;
    }

    html->container->set_base_url(base_url);
    html->container->m_anchors.clear();

    oops_log_debug("HTML", "Parsing HTML document (length: %zu bytes)",
                   strlen(html_source));
    auto doc = litehtml::document::createFromString(html_source, html->container.get(),
                                                    litehtml::master_css);
    if (!doc) {
        oops_log_error("HTML", "Failed to parse HTML document");
        return -1;
    }

    html->container->m_doc = doc;
    doc->render(html->container->m_width);
    oops_log_info("HTML", "HTML layout complete. Content height: %d px",
                  (int)doc->height());
    return 0;
}

void oops_html_render(oops_html_t *html, oops_surface_t *surf) {
    if (!html || !html->container || !surf)
        return;
    html->container->render(surf);
}

void oops_html_scroll(oops_html_t *html, int dx, int dy) {
    if (!html || !html->container)
        return;
    html->container->scroll(dx, dy);
}

int oops_html_get_scroll_y(const oops_html_t *html) {
    if (!html || !html->container)
        return 0;
    return html->container->m_scroll_y;
}

int oops_html_get_content_height(const oops_html_t *html) {
    if (!html || !html->container)
        return 0;
    return html->container->get_content_height();
}

int oops_html_hit_test(oops_html_t *html, int x, int y, char *href_out,
                       size_t href_len) {
    if (!html || !html->container)
        return 0;
    return html->container->hit_test(x, y, href_out, href_len);
}

void *oops_html_get_document(oops_html_t *html) {
    if (!html || !html->container || !html->container->m_doc)
        return NULL;
    return html->container->m_doc.get();
}

void *oops_html_get_container(oops_html_t *html) {
    if (!html || !html->container)
        return NULL;
    return html->container.get();
}

} // extern "C"
