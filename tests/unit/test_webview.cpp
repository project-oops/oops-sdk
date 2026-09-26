#include "oops/webview.h"
#include "oops/js.h"
#include "oops/html.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>

// Unit tests for `oops/webview.h`: page load, script DOM access, input events, timers
// and rendering. A standalone program; each test asserts and prints PASS.

// A webview creates its JS and HTML engines and destroys cleanly.
void test_webview_lifecycle() {
    oops_webview_t *wv = oops_webview_create(800, 600);
    assert(wv != nullptr);
    assert(oops_webview_get_js(wv) != nullptr);
    assert(oops_webview_get_html(wv) != nullptr);
    oops_webview_destroy(wv);
    printf("PASS: test_webview_lifecycle\n");
}

// A page's script changes and creates DOM nodes that a later eval reads back.
void test_webview_dom_mutation() {
    oops_webview_t *wv = oops_webview_create(800, 600);
    assert(wv != nullptr);

    const char *html = "<!DOCTYPE html>"
                       "<html><body>"
                       "<h1 id=\"title\">Before JS</h1>"
                       "<div id=\"container\"></div>"
                       "<script>"
                       "  const t = document.getElementById('title');"
                       "  t.textContent = 'After JS';"
                       "  const c = document.getElementById('container');"
                       "  const p = document.createElement('p');"
                       "  p.textContent = 'Created Child';"
                       "  c.appendChild(p);"
                       "</script>"
                       "</body></html>";

    int rc = oops_webview_load_html(wv, html, "https://example.com/");
    assert(rc == 0);

    oops_js_t *js = oops_webview_get_js(wv);
    assert(js != nullptr);

    oops_js_value_t title_val;
    int eval_rc = oops_js_eval(js, "document.getElementById('title').textContent",
                               "<test>", &title_val);
    assert(eval_rc == 0);
    assert(title_val.type == OOPS_JS_TYPE_STRING);
    assert(strcmp(title_val.u.string, "After JS") == 0);
    oops_js_free_value(js, &title_val);

    oops_js_value_t child_val;
    eval_rc = oops_js_eval(js, "document.getElementById('container').textContent",
                           "<test>", &child_val);
    assert(eval_rc == 0);
    assert(child_val.type == OOPS_JS_TYPE_STRING);
    assert(strcmp(child_val.u.string, "Created Child") == 0);
    oops_js_free_value(js, &child_val);

    oops_webview_destroy(wv);
    printf("PASS: test_webview_dom_mutation\n");
}

// Mouse and key input reach the page's window listeners once each.
void test_webview_events_and_input() {
    oops_webview_t *wv = oops_webview_create(800, 600);
    assert(wv != nullptr);

    const char *html = "<!DOCTYPE html>"
                       "<html><body>"
                       "<script>"
                       "  var clickCount = 0;"
                       "  window.addEventListener('click', () => { clickCount++; });"
                       "  var keyCount = 0;"
                       "  window.addEventListener('keydown', (e) => { keyCount++; });"
                       "</script>"
                       "</body></html>";

    int rc = oops_webview_load_html(wv, html, "https://example.com/");
    assert(rc == 0);

    oops_js_t *js = oops_webview_get_js(wv);

    // Send mouse click
    oops_webview_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = OOPS_WEBVIEW_EVENT_MOUSE_BUTTON_UP;
    ev.u.mouse.x = 100;
    ev.u.mouse.y = 100;
    ev.u.mouse.button = 1;
    oops_webview_send_input(wv, &ev);

    // Send keydown
    memset(&ev, 0, sizeof(ev));
    ev.type = OOPS_WEBVIEW_EVENT_KEY_DOWN;
    ev.u.key.keycode = 65;
    oops_webview_send_input(wv, &ev);

    oops_js_value_t click_val;
    int eval_rc = oops_js_eval(js, "clickCount", "<test>", &click_val);
    assert(eval_rc == 0);
    assert(click_val.type == OOPS_JS_TYPE_INT);
    assert(click_val.u.integer == 1);
    oops_js_free_value(js, &click_val);

    oops_js_value_t key_val;
    eval_rc = oops_js_eval(js, "keyCount", "<test>", &key_val);
    assert(eval_rc == 0);
    assert(key_val.type == OOPS_JS_TYPE_INT);
    assert(key_val.u.integer == 1);
    oops_js_free_value(js, &key_val);

    oops_webview_destroy(wv);
    printf("PASS: test_webview_events_and_input\n");
}

// A `setTimeout` callback fires from a pump after its delay has passed, and not from
// one before it.
void test_webview_timers_and_pump() {
    oops_webview_t *wv = oops_webview_create(800, 600);
    assert(wv != nullptr);

    const char *html = "<!DOCTYPE html>"
                       "<html><body>"
                       "<script>"
                       "  var timerFired = false;"
                       "  setTimeout(() => { timerFired = true; }, 200);"
                       "</script>"
                       "</body></html>";

    int rc = oops_webview_load_html(wv, html, "https://example.com/");
    assert(rc == 0);

    oops_js_t *js = oops_webview_get_js(wv);

    // One pump before the 200ms delay has passed, one after.
    oops_webview_pump(wv);
    oops_js_value_t early_val;
    int early_rc = oops_js_eval(js, "timerFired", "<test>", &early_val);
    assert(early_rc == 0);
    assert(early_val.type == OOPS_JS_TYPE_BOOL);
    assert(early_val.u.boolean == 0);
    oops_js_free_value(js, &early_val);

    usleep(250000);
    oops_webview_pump(wv);

    oops_js_value_t timer_val;
    int eval_rc = oops_js_eval(js, "timerFired", "<test>", &timer_val);
    assert(eval_rc == 0);
    assert(timer_val.type == OOPS_JS_TYPE_BOOL);
    assert(timer_val.u.boolean == 1);
    oops_js_free_value(js, &timer_val);

    oops_webview_destroy(wv);
    printf("PASS: test_webview_timers_and_pump\n");
}

// Rendering a styled page writes pixels to the surface.
void test_webview_render() {
    oops_webview_t *wv = oops_webview_create(800, 600);
    assert(wv != nullptr);

    const char *html = "<!DOCTYPE html>"
                       "<html><head><style>"
                       "body { margin: 0; background-color: #ffaa00; }"
                       "h1 { color: #0000ff; }"
                       "</style></head><body>"
                       "<h1>Hello Webview</h1>"
                       "</body></html>";

    int rc = oops_webview_load_html(wv, html, "https://example.com/");
    assert(rc == 0);

    uint32_t *pixels = (uint32_t *)calloc(800 * 600, sizeof(uint32_t));
    assert(pixels != nullptr);

    oops_surface_t surf;
    surf.pixels = pixels;
    surf.width = 800;
    surf.height = 600;
    surf.pitch = 800;
    surf.layout = OOPS_SURFACE_LINEAR;

    oops_webview_render(wv, &surf);

    bool has_pixels = false;
    for (int i = 0; i < 800 * 600; i++) {
        if (pixels[i] != 0) {
            has_pixels = true;
            break;
        }
    }
    assert(has_pixels);

    free(pixels);
    oops_webview_destroy(wv);
    printf("PASS: test_webview_render\n");
}

int main() {
    printf("=== RUNNING WEBVIEW UNIT TESTS ===\n");
    test_webview_lifecycle();
    test_webview_dom_mutation();
    test_webview_events_and_input();
    test_webview_timers_and_pump();
    test_webview_render();
    printf("ALL WEBVIEW UNIT TESTS PASSED!\n");
    return 0;
}
