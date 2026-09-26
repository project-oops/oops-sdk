#include "oops/html.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

// Unit tests for the HTML renderer, `oops/html.h`. A standalone program; each test
// asserts and prints PASS.

// A document creates and destroys cleanly.
void test_html_lifecycle() {
    oops_html_t *html = oops_html_create(800, 600);
    assert(html != nullptr);
    oops_html_destroy(html);
    printf("PASS: test_html_lifecycle\n");
}

// A styled page lays out to a non-zero height, renders pixels and scrolls.
void test_html_load_and_render() {
    oops_html_t *html = oops_html_create(800, 600);
    assert(html != nullptr);

    const char *source =
        "<!DOCTYPE html>"
        "<html><head><style>"
        "body { margin: 10px; background-color: #202020; color: #ffffff; }"
        "h1 { font-size: 24px; color: #ffcc00; margin: 0; }"
        "p { font-size: 16px; margin: 5px 0; }"
        ".card { width: 200px; height: 100px; background-color: #333333; border: 2px "
        "solid #00ff00; }"
        "a { color: #00ccff; text-decoration: underline; }"
        "</style></head>"
        "<body>"
        "<h1>OOPS Web Stack</h1>"
        "<p>Static layout rendered via litehtml</p>"
        "<div class=\"card\"></div>"
        "<p><a href=\"https://oops.org/catalogue\">Open Catalogue</a></p>"
        "</body></html>";

    int rc = oops_html_load(html, source, "https://oops.org/");
    assert(rc == 0);

    int content_h = oops_html_get_content_height(html);
    assert(content_h > 0);

    uint32_t *pixels = (uint32_t *)calloc(800 * 600, sizeof(uint32_t));
    assert(pixels != nullptr);

    oops_surface_t surf;
    surf.pixels = pixels;
    surf.width = 800;
    surf.height = 600;
    surf.pitch = 800;
    surf.layout = OOPS_SURFACE_LINEAR;

    oops_html_render(html, &surf);

    // The background was drawn: not every pixel is zero.
    bool has_rendered_pixels = false;
    for (int i = 0; i < 800 * 600; i++) {
        if (pixels[i] != 0) {
            has_rendered_pixels = true;
            break;
        }
    }
    assert(has_rendered_pixels);

    assert(oops_html_get_scroll_y(html) == 0);
    oops_html_scroll(html, 0, 50);
    int sy = oops_html_get_scroll_y(html);
    assert(sy >= 0);

    free(pixels);
    oops_html_destroy(html);
    printf("PASS: test_html_load_and_render\n");
}

// A link's box answers a hit test with its href, and a point outside every link does
// not. The link is a sized block so its position does not depend on the font.
void test_html_hit_test_finds_the_link() {
    oops_html_t *html = oops_html_create(800, 600);
    assert(html != nullptr);
    const char *source = "<html><head><style>"
                         "body { margin: 0; }"
                         "a { display: block; width: 200px; height: 50px; }"
                         "</style></head><body>"
                         "<div style=\"height: 100px\"></div>"
                         "<a href=\"https://oops.org/catalogue\"></a>"
                         "</body></html>";
    int rc = oops_html_load(html, source, "https://oops.org/");
    assert(rc == 0);

    char href[128] = {0};
    int hit = oops_html_hit_test(html, 20, 120, href, sizeof(href));
    if (hit != 1 || strcmp(href, "https://oops.org/catalogue") != 0) {
        printf("FAIL: test_html_hit_test_finds_the_link (hit=%d href=\"%s\")\n", hit,
               href);
        exit(1);
    }
    char none[128] = {0};
    if (oops_html_hit_test(html, 20, 20, none, sizeof(none)) != 0) {
        printf("FAIL: test_html_hit_test_finds_the_link (hit above the link)\n");
        exit(1);
    }
    oops_html_destroy(html);
    printf("PASS: test_html_hit_test_finds_the_link\n");
}

int main() {
    printf("=== RUNNING HTML UNIT TESTS ===\n");
    test_html_lifecycle();
    test_html_load_and_render();
    test_html_hit_test_finds_the_link();
    printf("ALL HTML UNIT TESTS PASSED!\n");
    return 0;
}
