#ifndef OOPS_WEBVIEW_H
#define OOPS_WEBVIEW_H

#include "oops/draw.h"
#include "oops/js.h"
#include "oops/html.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_webview oops_webview_t;

typedef enum oops_webview_event_type {
    OOPS_WEBVIEW_EVENT_PAD = 1,
    OOPS_WEBVIEW_EVENT_KEY_DOWN,
    OOPS_WEBVIEW_EVENT_KEY_UP,
    OOPS_WEBVIEW_EVENT_MOUSE_MOVE,
    OOPS_WEBVIEW_EVENT_MOUSE_BUTTON_DOWN,
    OOPS_WEBVIEW_EVENT_MOUSE_BUTTON_UP,
    OOPS_WEBVIEW_EVENT_MOUSE_WHEEL
} oops_webview_event_type_t;

typedef struct oops_webview_event {
    oops_webview_event_type_t type;
    union {
        struct {
            uint32_t buttons; /* Pad button bitmask */
            float lx, ly;     /* Left analog stick [-1.0, 1.0] */
            float rx, ry;     /* Right analog stick [-1.0, 1.0] */
        } pad;
        struct {
            uint32_t keycode;   /* Keycode / ASCII value */
            uint32_t modifiers; /* Modifiers (shift/ctrl/alt) */
        } key;
        struct {
            int32_t x, y;    /* Coordinates */
            int32_t dx, dy;  /* Relative delta */
            uint32_t button; /* 1=Left, 2=Right, 3=Middle */
        } mouse;
    } u;
} oops_webview_event_t;

/*
 * Create an assembled webview instance binding QuickJS and litehtml.
 * Initializes DOM prototype classes, console.*, fetch(), and timer wheel.
 * Returns NULL on error.
 */
oops_webview_t *oops_webview_create(int width, int height);

/*
 * Destroy a webview instance, aborting pending fetches and cleaning up
 * DOM trees, JS runtimes, and timers.
 */
void oops_webview_destroy(oops_webview_t *wv);

/*
 * Load a document from a URL over HTTP/HTTPS, parse HTML, execute embedded
 * scripts, and prepare initial layout.
 */
int oops_webview_load_url(oops_webview_t *wv, const char *url);

/*
 * Load an HTML string directly into the webview with a base URL for resolving
 * relative resources.
 */
int oops_webview_load_html(oops_webview_t *wv, const char *html, const char *base_url);

/*
 * Pump the webview execution loop:
 *   - Process active timer callbacks (setTimeout, setInterval).
 *   - Advance and drain pending asynchronous HTTP fetch operations.
 *   - Execute pending Promise microtasks in the QuickJS engine.
 *   - Re-flow and relayout the litehtml document if the DOM or styles changed.
 */
void oops_webview_pump(oops_webview_t *wv);

/*
 * Render the current page state into the provided target surface.
 */
void oops_webview_render(oops_webview_t *wv, oops_surface_t *surf);

/*
 * Send an input event (pad, keyboard, mouse) into the webview.
 * Dispatches to DOM focus, click, and keyboard event handlers.
 */
void oops_webview_send_input(oops_webview_t *wv, const oops_webview_event_t *ev);

/*
 * Access the underlying JS engine instance.
 */
oops_js_t *oops_webview_get_js(oops_webview_t *wv);

/*
 * Access the underlying HTML renderer instance.
 */
oops_html_t *oops_webview_get_html(oops_webview_t *wv);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_WEBVIEW_H */
