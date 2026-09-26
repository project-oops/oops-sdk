/*
 * litehtml's document container: the drawing and metrics back end, onto an
 * `oops_surface_t` through oops/draw.h.
 *
 * Text uses the built-in bitmap font scaled by size/8, and its width is measured to match
 * exactly what draw_text renders (both quantise to 8*scale per glyph). Radial and conic gradients
 * fill with their first colour, linear ones with their end colours. Images are fetched over HTTP
 * and PNG-decoded into `m_image_cache` by `load_image`; clip rectangles are tracked but not applied.
 */

#include "html_container.hpp"
#include "oops/http.h"
#include "oops/system.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>

oops_container::oops_container(int width, int height)
    : m_width(width), m_height(height), m_scroll_x(0), m_scroll_y(0) {
    oops_log_debug("HTML", "oops_container created with size %dx%d", width, height);
}

oops_container::~oops_container() {
    for (auto &pair : m_image_cache) {
        if (pair.second.pixels) {
            free(pair.second.pixels);
        }
    }
    m_image_cache.clear();
    oops_log_debug("HTML", "oops_container destroyed");
}

litehtml::uint_ptr oops_container::create_font(const litehtml::font_description &descr,
                                               const litehtml::document * /*doc*/,
                                               litehtml::font_metrics *fm) {
    auto font = std::make_unique<oops_font_desc>();
    font->name = descr.family;
    int fsz = (int)descr.size;
    font->size = fsz > 0 ? fsz : 16;
    font->weight = descr.weight;
    font->style = descr.style;

    if (fm) {
        fm->font_size = font->size;
        fm->ascent = (font->size * 4) / 5;
        fm->descent = std::max(1, font->size / 5);
        fm->height = font->size;
        fm->x_height = font->size / 2;
    }

    litehtml::uint_ptr handle = reinterpret_cast<litehtml::uint_ptr>(font.get());
    m_fonts.push_back(std::move(font));
    return handle;
}

void oops_container::delete_font(litehtml::uint_ptr /*hFont*/) {
    // Fonts are owned by m_fonts and freed with the container.
}

litehtml::pixel_t oops_container::text_width(const char *text,
                                             litehtml::uint_ptr hFont) {
    if (!text || !text[0])
        return 0;
    auto *font = reinterpret_cast<oops_font_desc *>(hFont);
    int size = font ? font->size : 16;
    // Mirror draw_text exactly. The 8x8 bitmap font is drawn at scale = max(1, size/8) and advances
    // 8*scale pixels per character (draw.c: `cur_x += FONT_WIDTH * scale`, FONT_WIDTH == 8). Measuring
    // with any other metric (the old 0.6*size) makes glyphs render wider than their measured box, so
    // the following inline box overlaps them - visible as the 30px brand drawing "index" on top of
    // "oops-apps". Quantising the width the same way keeps measurement and drawing in lockstep.
    int scale = size / 8;
    if (scale < 1)
        scale = 1;
    int char_w = 8 * scale;
    size_t len = strlen(text);
    return litehtml::pixel_t(static_cast<int>(len * (size_t)char_w));
}

void oops_container::draw_text(litehtml::uint_ptr hdc, const char *text,
                               litehtml::uint_ptr hFont, litehtml::web_color color,
                               const litehtml::position &pos) {
    if (!hdc || !text || !text[0])
        return;
    auto *surf = reinterpret_cast<oops_surface_t *>(hdc);
    auto *font = reinterpret_cast<oops_font_desc *>(hFont);
    int size = font ? font->size : 16;
    int scale = size / 8;
    if (scale < 1)
        scale = 1;

    oops_color_t col = OOPS_RGBA(color.red, color.green, color.blue, color.alpha);
    oops_draw_text(surf, (int)pos.x, (int)pos.y, text, col, scale);
}

litehtml::pixel_t oops_container::pt_to_px(float pt) const {
    return litehtml::pixel_t(static_cast<int>(std::round(pt * 96.0f / 72.0f)));
}

litehtml::pixel_t oops_container::get_default_font_size() const {
    return 16;
}

const char *oops_container::get_default_font_name() const {
    return "sans-serif";
}

void oops_container::draw_list_marker(litehtml::uint_ptr hdc,
                                      const litehtml::list_marker &marker) {
    if (!hdc)
        return;
    auto *surf = reinterpret_cast<oops_surface_t *>(hdc);
    oops_color_t col = OOPS_RGBA(marker.color.red, marker.color.green,
                                 marker.color.blue, marker.color.alpha);
    int cx = (int)marker.pos.x + (int)marker.pos.width / 2;
    int cy = (int)marker.pos.y + (int)marker.pos.height / 2;
    int r = std::max(2, (int)marker.pos.width / 4);
    oops_draw_circle_blend(surf, cx, cy, r, col, 1);
}

void oops_container::load_image(const char *src, const char *baseurl,
                                bool /*redraw_on_ready*/) {
    if (!src || !src[0])
        return;
    std::string key = src;
    if (m_image_cache.find(key) != m_image_cache.end())
        return;   // already loaded, or a prior attempt failed - either way do not refetch

    // The page uses absolute https URLs for icons; fall back to base+src for anything relative.
    std::string url;
    if (std::strncmp(src, "http://", 7) == 0 || std::strncmp(src, "https://", 8) == 0)
        url = src;
    else
        url = std::string(baseurl ? baseurl : "") + src;

    // An empty surface is cached whatever happens, so a failed fetch is attempted once, not on
    // every re-render. Decode straight to a fixed square; draw_image resamples to the element box.
    oops_surface_t cached = {};
    const uint32_t TW = 64, TH = 64;

    oops_http_response_t resp;
    int rc = oops_http_get(url.c_str(), &resp);
    if (rc == OOPS_HTTP_OK && resp.status_code == 200 && resp.body && resp.body_size > 0) {
        uint32_t *pix = (uint32_t *)std::malloc((size_t)TW * TH * 4u);
        if (pix) {
            uint32_t ow = 0, oh = 0;
            int dr = oops_png_decode(resp.body, resp.body_size, pix, TW, TH, &ow, &oh);
            if (dr == 0) {
                cached.pixels = pix;
                cached.width = TW;
                cached.height = TH;
                cached.pitch = TW;
                oops_kprintf_level(OOPS_LOG_INFO, "HTML", "load_image ok %s (%ux%u->%ux%u)",
                                   url.c_str(), ow, oh, TW, TH);
            } else {
                std::free(pix);
                oops_kprintf_level(OOPS_LOG_ERROR, "HTML", "load_image decode failed rc=%d %s",
                                   dr, url.c_str());
            }
        }
    } else {
        oops_kprintf_level(OOPS_LOG_ERROR, "HTML", "load_image fetch failed rc=%d status=%d %s",
                           rc, rc == OOPS_HTTP_OK ? resp.status_code : -1, url.c_str());
    }
    if (rc == OOPS_HTTP_OK)
        oops_http_response_free(&resp);

    m_image_cache[key] = cached;
}

void oops_container::get_image_size(const char *src, const char * /*baseurl*/,
                                    litehtml::size &sz) {
    if (!src) {
        sz.width = 0;
        sz.height = 0;
        return;
    }
    auto it = m_image_cache.find(src);
    if (it != m_image_cache.end()) {
        sz.width = litehtml::pixel_t(static_cast<int>(it->second.width));
        sz.height = litehtml::pixel_t(static_cast<int>(it->second.height));
    } else {
        sz.width = 16;
        sz.height = 16;
    }
}

void oops_container::draw_image(litehtml::uint_ptr hdc,
                                const litehtml::background_layer &layer,
                                const std::string &url,
                                const std::string & /*base_url*/) {
    if (!hdc)
        return;
    auto *dst = reinterpret_cast<oops_surface_t *>(hdc);
    auto it = m_image_cache.find(url);
    if (it != m_image_cache.end() && it->second.pixels) {
        oops_surface_t *src = &it->second;
        oops_draw_blit_scaled_blend(
            dst, (int)layer.border_box.x, (int)layer.border_box.y,
            (int)layer.border_box.width, (int)layer.border_box.height, src, 0, 0,
            (int)src->width, (int)src->height);
    }
}

void oops_container::draw_solid_fill(litehtml::uint_ptr hdc,
                                     const litehtml::background_layer &layer,
                                     const litehtml::web_color &color) {
    if (!hdc || (int)layer.border_box.width <= 0 || (int)layer.border_box.height <= 0)
        return;
    auto *surf = reinterpret_cast<oops_surface_t *>(hdc);
    oops_color_t col = OOPS_RGBA(color.red, color.green, color.blue, color.alpha);
    oops_draw_rect_blend(surf, (int)layer.border_box.x, (int)layer.border_box.y,
                         (int)layer.border_box.width, (int)layer.border_box.height,
                         col);
}

void oops_container::draw_linear_gradient(
    litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
    const litehtml::background_layer::linear_gradient &gradient) {
    if (!hdc || (int)layer.border_box.width <= 0 || (int)layer.border_box.height <= 0)
        return;
    auto *surf = reinterpret_cast<oops_surface_t *>(hdc);

    if (gradient.color_points.empty())
        return;

    const auto &c0 = gradient.color_points.front().color;
    const auto &c1 = gradient.color_points.back().color;
    oops_color_t col_a = OOPS_RGBA(c0.red, c0.green, c0.blue, c0.alpha);
    oops_color_t col_b = OOPS_RGBA(c1.red, c1.green, c1.blue, c1.alpha);

    oops_draw_rect_gradient(surf, (int)layer.border_box.x, (int)layer.border_box.y,
                            (int)layer.border_box.width, (int)layer.border_box.height,
                            col_a, col_b, 1);
}

void oops_container::draw_radial_gradient(
    litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
    const litehtml::background_layer::radial_gradient &gradient) {
    if (!hdc || gradient.color_points.empty())
        return;
    const auto &c = gradient.color_points.front().color;
    draw_solid_fill(hdc, layer, c);
}

void oops_container::draw_conic_gradient(
    litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
    const litehtml::background_layer::conic_gradient &gradient) {
    if (!hdc || gradient.color_points.empty())
        return;
    const auto &c = gradient.color_points.front().color;
    draw_solid_fill(hdc, layer, c);
}

void oops_container::draw_borders(litehtml::uint_ptr hdc,
                                  const litehtml::borders &borders,
                                  const litehtml::position &draw_pos, bool /*root*/) {
    if (!hdc)
        return;
    auto *surf = reinterpret_cast<oops_surface_t *>(hdc);

    int top_w = (int)borders.top.width;
    if (top_w > 0 && borders.top.style > litehtml::border_style_hidden) {
        oops_color_t c = OOPS_RGBA(borders.top.color.red, borders.top.color.green,
                                   borders.top.color.blue, borders.top.color.alpha);
        oops_draw_rect_blend(surf, (int)draw_pos.x, (int)draw_pos.y,
                             (int)draw_pos.width, top_w, c);
    }
    int bottom_w = (int)borders.bottom.width;
    if (bottom_w > 0 && borders.bottom.style > litehtml::border_style_hidden) {
        oops_color_t c =
            OOPS_RGBA(borders.bottom.color.red, borders.bottom.color.green,
                      borders.bottom.color.blue, borders.bottom.color.alpha);
        oops_draw_rect_blend(surf, (int)draw_pos.x, (int)draw_pos.bottom() - bottom_w,
                             (int)draw_pos.width, bottom_w, c);
    }
    int left_w = (int)borders.left.width;
    if (left_w > 0 && borders.left.style > litehtml::border_style_hidden) {
        oops_color_t c = OOPS_RGBA(borders.left.color.red, borders.left.color.green,
                                   borders.left.color.blue, borders.left.color.alpha);
        oops_draw_rect_blend(surf, (int)draw_pos.x, (int)draw_pos.y, left_w,
                             (int)draw_pos.height, c);
    }
    int right_w = (int)borders.right.width;
    if (right_w > 0 && borders.right.style > litehtml::border_style_hidden) {
        oops_color_t c = OOPS_RGBA(borders.right.color.red, borders.right.color.green,
                                   borders.right.color.blue, borders.right.color.alpha);
        oops_draw_rect_blend(surf, (int)draw_pos.right() - right_w, (int)draw_pos.y,
                             right_w, (int)draw_pos.height, c);
    }
}

void oops_container::set_caption(const char *caption) {
    m_caption = caption ? caption : "";
    oops_log_debug("HTML", "Document title set to: %s", m_caption.c_str());
}

void oops_container::set_base_url(const char *base_url) {
    m_base_url = base_url ? base_url : "";
}

// Called for a `<link>` element, which has no box to hit; links are `<a>` elements,
// found after layout by `hit_test`.
void oops_container::link(const std::shared_ptr<litehtml::document> & /*doc*/,
                          const litehtml::element::ptr & /*el*/) {}

void oops_container::on_anchor_click(const char *url,
                                     const litehtml::element::ptr & /*el*/) {
    oops_log_info("HTML", "Anchor clicked: %s", url ? url : "(null)");
}

void oops_container::on_mouse_event(const litehtml::element::ptr & /*el*/,
                                    litehtml::mouse_event /*event*/) {}

void oops_container::set_cursor(const char * /*cursor*/) {}

void oops_container::transform_text(std::string &text, litehtml::text_transform tt) {
    if (tt == litehtml::text_transform_uppercase) {
        std::transform(text.begin(), text.end(), text.begin(), ::toupper);
    } else if (tt == litehtml::text_transform_lowercase) {
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    } else if (tt == litehtml::text_transform_capitalize) {
        bool cap = true;
        for (char &ch : text) {
            if (::isspace(static_cast<unsigned char>(ch))) {
                cap = true;
            } else if (cap) {
                ch = static_cast<char>(::toupper(static_cast<unsigned char>(ch)));
                cap = false;
            }
        }
    }
}

void oops_container::import_css(std::string & /*text*/, const std::string &url,
                                std::string & /*baseurl*/) {
    oops_log_debug("HTML", "import_css: %s", url.c_str());
}

void oops_container::set_clip(const litehtml::position &pos,
                              const litehtml::border_radiuses & /*bdr_radius*/) {
    m_clip_stack.push_back(pos);
}

void oops_container::del_clip() {
    if (!m_clip_stack.empty()) {
        m_clip_stack.pop_back();
    }
}

void oops_container::get_viewport(litehtml::position &viewport) const {
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = m_width;
    viewport.height = m_height;
}

litehtml::element::ptr
oops_container::create_element(const char * /*tag_name*/,
                               const litehtml::string_map & /*attributes*/,
                               const std::shared_ptr<litehtml::document> & /*doc*/) {
    return nullptr; // litehtml creates standard default elements
}

void oops_container::get_media_features(litehtml::media_features &media) const {
    media.type = litehtml::media_type_screen;
    media.width = m_width;
    media.height = m_height;
    media.device_width = m_width;
    media.device_height = m_height;
    media.color = 8;
    media.monochrome = 0;
    media.color_index = 256;
    media.resolution = 96;
}

void oops_container::get_language(std::string &language, std::string &culture) const {
    language = "en";
    culture = "US";
}

void oops_container::render(oops_surface_t *surf) {
    if (!surf || !m_doc)
        return;
    litehtml::position clip(0, 0, m_width, m_height);
    m_doc->draw(reinterpret_cast<litehtml::uint_ptr>(surf), -m_scroll_x, -m_scroll_y,
                &clip);
}

void oops_container::scroll(int dx, int dy) {
    m_scroll_x += dx;
    m_scroll_y += dy;
    int max_y = std::max(0, get_content_height() - m_height);
    if (m_scroll_y < 0)
        m_scroll_y = 0;
    if (m_scroll_y > max_y)
        m_scroll_y = max_y;
}

// Scroll vertically the minimum needed so the document range [top, top+h] is inside the viewport.
// A keyboard/pad-driven page has no mouse wheel, so it moves a highlight and asks the view to keep
// the highlighted element on screen.
void oops_container::ensure_visible(int top, int h) {
    int max_y = std::max(0, get_content_height() - m_height);
    int target = m_scroll_y;
    if (top < m_scroll_y)
        target = top;                        // range starts above the viewport: reveal its top
    else if (top + h > m_scroll_y + m_height)
        target = top + h - m_height;         // range ends below the viewport: reveal its bottom
    if (target < 0)
        target = 0;
    if (target > max_y)
        target = max_y;
    m_scroll_y = target;
}

int oops_container::hit_test(int x, int y, char *href_out, size_t href_len) {
    int doc_x = x + m_scroll_x;
    int doc_y = y + m_scroll_y;
    litehtml::position pt(doc_x, doc_y, 1, 1);
    if (!m_doc || !m_doc->root())
        return 0;
    // Placements are read now, so they are the current layout's.
    for (const auto &el : m_doc->root()->select_all("a[href]")) {
        const litehtml::position pos = el->get_placement();
        if (!pos.does_intersect(&pt))
            continue;
        if (href_out && href_len > 0) {
            strncpy(href_out, el->get_attr("href", ""), href_len - 1);
            href_out[href_len - 1] = '\0';
        }
        return 1;
    }
    return 0;
}

int oops_container::get_content_height() const {
    return m_doc ? (int)m_doc->height() : m_height;
}
