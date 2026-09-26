#ifndef OOPS_HTML_CONTAINER_HPP
#define OOPS_HTML_CONTAINER_HPP

#include "litehtml/document_container.h"
#include "litehtml/document.h"
#include "oops/draw.h"
#include "oops/system.h"

#include <string>
#include <vector>
#include <map>
#include <memory>

struct oops_font_desc {
    std::string name;
    int size;
    int weight;
    int style;
};

struct oops_anchor {
    litehtml::position pos;
    std::string href;
    litehtml::element::ptr el;
};

class oops_container : public litehtml::document_container {
  public:
    int m_width;
    int m_height;
    int m_scroll_x;
    int m_scroll_y;
    std::string m_caption;
    std::string m_base_url;

    std::vector<litehtml::position> m_clip_stack;
    std::vector<std::unique_ptr<oops_font_desc>> m_fonts;
    std::map<std::string, oops_surface_t> m_image_cache;
    std::vector<oops_anchor> m_anchors;

    litehtml::document::ptr m_doc;

    oops_container(int width, int height);
    virtual ~oops_container();

    // litehtml::document_container implementation
    litehtml::uint_ptr create_font(const litehtml::font_description &descr,
                                   const litehtml::document *doc,
                                   litehtml::font_metrics *fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char *text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position &pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override;
    litehtml::pixel_t get_default_font_size() const override;
    const char *get_default_font_name() const override;
    void draw_list_marker(litehtml::uint_ptr hdc,
                          const litehtml::list_marker &marker) override;
    void load_image(const char *src, const char *baseurl,
                    bool redraw_on_ready) override;
    void get_image_size(const char *src, const char *baseurl,
                        litehtml::size &sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                    const std::string &url, const std::string &base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc,
                         const litehtml::background_layer &layer,
                         const litehtml::web_color &color) override;
    void draw_linear_gradient(
        litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
        const litehtml::background_layer::linear_gradient &gradient) override;
    void draw_radial_gradient(
        litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
        const litehtml::background_layer::radial_gradient &gradient) override;
    void draw_conic_gradient(
        litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
        const litehtml::background_layer::conic_gradient &gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &borders,
                      const litehtml::position &draw_pos, bool root) override;

    void set_caption(const char *caption) override;
    void set_base_url(const char *base_url) override;
    void link(const std::shared_ptr<litehtml::document> &doc,
              const litehtml::element::ptr &el) override;
    void on_anchor_click(const char *url, const litehtml::element::ptr &el) override;
    void on_mouse_event(const litehtml::element::ptr &el,
                        litehtml::mouse_event event) override;
    void set_cursor(const char *cursor) override;
    void transform_text(std::string &text, litehtml::text_transform tt) override;
    void import_css(std::string &text, const std::string &url,
                    std::string &baseurl) override;
    void set_clip(const litehtml::position &pos,
                  const litehtml::border_radiuses &bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position &viewport) const override;
    litehtml::element::ptr
    create_element(const char *tag_name, const litehtml::string_map &attributes,
                   const std::shared_ptr<litehtml::document> &doc) override;

    void get_media_features(litehtml::media_features &media) const override;
    void get_language(std::string &language, std::string &culture) const override;

    // Helper methods
    void render(oops_surface_t *surf);
    void scroll(int dx, int dy);
    int hit_test(int x, int y, char *href_out, size_t href_len);
    int get_content_height() const;
};

#endif // OOPS_HTML_CONTAINER_HPP
