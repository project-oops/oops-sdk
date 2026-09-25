# OOPS Web Stack Developer Guide: JS, HTML/CSS & Webview

This document is a dedicated reference guide for the OOPS on-device web runtime. It covers the architecture, APIs, build configuration, and usage patterns for the three standalone tiers:

1. **`oops/js.h`**: Standalone JavaScript engine (QuickJS) — pure scripting with zero rendering dependencies.
2. **`oops/html.h`**: Standalone HTML5/CSS layout and paint engine (litehtml) — static/templated UI rendering with zero JavaScript dependencies.
3. **`oops/webview.h`**: Assembled webview browser — combines the JS engine and HTML layout with a live DOM bridge, host bindings (`fetch()`, `console.*`, timers), and hardware input routing.

---

## 1. Architectural Tiers

```
+-----------------------------------------------------------------------------------+
|                                oops/webview.h                                     |
|  - Live DOM bridge: document.getElementById, createElement, appendChild, etc.     |
|  - Host environment: fetch() -> Promise, console.log, setTimeout / setInterval    |
|  - Event loop pump: timer wheel, network drain, microtasks, layout reflow         |
|  - Hardware input dispatch: Pad / Keyboard / Mouse -> DOM events (click, keydown) |
+-----------------------------------------+-----------------------------------------+
                                          |
                    +---------------------+---------------------+
                    |                                           |
                    v                                           v
+---------------------------------------+   +---------------------------------------+
|              oops/js.h                |   |              oops/html.h              |
|  - QuickJS ES2020 embeddable engine   |   |  - litehtml v0.9 + Gumbo HTML5 parser |
|  - Native C bindings (register_fn)    |   |  - CSS cascade, selector engine       |
|  - Standalone runtime & garbage coll. |   |  - Block, inline, flex & table layout |
|  - Promise microtask queue            |   |  - Rasterization to oops_surface_t    |
|  - ZERO rendering dependencies        |   |  - ZERO JavaScript dependencies       |
+---------------------------------------+   +---------------------------------------+
```

---

## 2. Build Integration (`OOPS_FEATURES`)

Applications request subsystems declaratively in their `Makefile` using `OOPS_FEATURES`:

### For Standalone JavaScript Scripting:
```makefile
APP_NAME      := my-script-app
OOPS_FEATURES := js
```

### For Standalone HTML/CSS Menus or HUDs:
```makefile
APP_NAME      := my-menu-app
OOPS_FEATURES := html
```

### For Full Live Webview:
```makefile
APP_NAME      := my-browser-app
OOPS_FEATURES := webview
```
*Note*: `OOPS_FEATURE_webview_DEPS := js html http input` automatically pulls in the JS engine, HTML renderer, HTTP/HTTPS networking, and gamepad/keyboard input drivers.

---

## 3. Layer 1: Standalone JavaScript Engine (`<oops/js.h>`)

The JavaScript subsystem provides a standalone ECMAScript 2020 runtime powered by QuickJS. It has no dependencies on graphics, display, or windowing subsystems.

### 3.1 Public API Overview

```c
#include "oops/js.h"

// Lifecycle
oops_js_t *oops_js_create(void);
void       oops_js_destroy(oops_js_t *js);

// Evaluation
int oops_js_eval(oops_js_t *js, const char *src, const char *name, oops_js_value_t *out);

// Native C Bindings
typedef oops_js_value_t (*oops_js_native_fn)(oops_js_t *js, int argc, oops_js_value_t *argv, void *userdata);
int oops_js_register_fn(oops_js_t *js, const char *name, oops_js_native_fn fn, void *userdata);

// Globals
int             oops_js_set_global(oops_js_t *js, const char *name, oops_js_value_t val);
oops_js_value_t oops_js_get_global(oops_js_t *js, const char *name);

// Microtasks
int oops_js_execute_pending_jobs(oops_js_t *js);

// Value Construction & Memory
oops_js_value_t oops_js_make_undefined(void);
oops_js_value_t oops_js_make_null(void);
oops_js_value_t oops_js_make_bool(int val);
oops_js_value_t oops_js_make_int(int val);
oops_js_value_t oops_js_make_number(double val);
oops_js_value_t oops_js_make_string(oops_js_t *js, const char *str);
void            oops_js_free_value(oops_js_t *js, oops_js_value_t *val);

// Low-Level QuickJS Context Access
void *oops_js_get_context(oops_js_t *js);
void *oops_js_get_runtime(oops_js_t *js);
```

### 3.2 Example: Evaluating Scripts and Native C Callbacks

```c
#include "oops/js.h"
#include <stdio.h>

static oops_js_value_t native_add(oops_js_t *js, int argc, oops_js_value_t *argv, void *userdata) {
    int a = (argc > 0 && argv[0].type == OOPS_JS_TYPE_INT) ? argv[0].u.integer : 0;
    int b = (argc > 1 && argv[1].type == OOPS_JS_TYPE_INT) ? argv[1].u.integer : 0;
    return oops_js_make_int(a + b);
}

void run_js_example(void) {
    oops_js_t *js = oops_js_create();

    // Bind C function
    oops_js_register_fn(js, "addNumbers", native_add, NULL);

    // Evaluate JavaScript
    oops_js_value_t result;
    const char *code = "const sum = addNumbers(40, 2); sum * 2;";
    if (oops_js_eval(js, code, "script.js", &result) == 0) {
        printf("Result: %d\n", result.u.integer); // 84
        oops_js_free_value(js, &result);
    }

    oops_js_destroy(js);
}
```

---

## 4. Layer 2: Standalone HTML5/CSS Layout Engine (`<oops/html.h>`)

The HTML subsystem parses HTML5 and CSS stylesheets, computes box layout metrics, and paints the page into an `oops_surface_t` (linear software frame buffer or OpenGL texture target). It has zero JavaScript dependencies.

### 4.1 Public API Overview

```c
#include "oops/html.h"
#include "oops/draw.h"

// Lifecycle
oops_html_t *oops_html_create(int width, int height);
void         oops_html_destroy(oops_html_t *html);
void         oops_html_set_size(oops_html_t *html, int width, int height);

// Document Loading & Layout
int  oops_html_load(oops_html_t *html, const char *html_source, const char *base_url);

// Rendering & Viewport Interaction
void oops_html_render(oops_html_t *html, oops_surface_t *surf);
void oops_html_scroll(oops_html_t *html, int dx, int dy);
int  oops_html_get_scroll_y(const oops_html_t *html);
int  oops_html_get_content_height(const oops_html_t *html);
int  oops_html_hit_test(oops_html_t *html, int x, int y, char *href_out, size_t href_len);

// Advanced Pointers
void *oops_html_get_document(oops_html_t *html);
void *oops_html_get_container(oops_html_t *html);
```

### 4.2 Example: Rendering a Static HUD/Menu

```c
#include "oops/html.h"
#include "oops/draw.h"
#include <stdlib.h>

void render_hud_example(oops_surface_t *surf) {
    oops_html_t *html = oops_html_create(1920, 1080);

    const char *markup =
        "<!DOCTYPE html>"
        "<html><head><style>"
        "  body { margin: 0; background-color: rgba(10, 10, 20, 0.85); color: #ffffff; font-family: sans-serif; }"
        "  .hud-panel { display: flex; justify-content: space-between; padding: 20px; border-bottom: 2px solid #00ffff; }"
        "  .score { font-size: 32px; color: #ffcc00; font-weight: bold; }"
        "  .health-bar { width: 300px; height: 24px; background: #330000; border: 1px solid #ff0000; }"
        "  .health-fill { width: 75%; height: 100%; background: #ff2222; }"
        "</style></head><body>"
        "  <div class=\"hud-panel\">"
        "    <div class=\"score\">SCORE: 125,000</div>"
        "    <div class=\"health-bar\"><div class=\"health-fill\"></div></div>"
        "  </div>"
        "</body></html>";

    // Load and lay out
    oops_html_load(html, markup, "https://local.app/");

    // Paint into surface
    oops_html_render(html, surf);

    oops_html_destroy(html);
}
```

---

## 5. Layer 3: Assembled Webview Browser (`<oops/webview.h>`)

The webview brings `oops/js.h` and `oops/html.h` together with a live DOM bridge, browser host environment (`fetch`, `console`, timers), and hardware input dispatching. It is designed to run dynamic client-side rendered web pages.

### 5.1 Public API Overview

```c
#include "oops/webview.h"

// Lifecycle
oops_webview_t *oops_webview_create(int width, int height);
void            oops_webview_destroy(oops_webview_t *wv);

// Document Loading
int  oops_webview_load_url(oops_webview_t *wv, const char *url);
int  oops_webview_load_html(oops_webview_t *wv, const char *html, const char *base_url);

// Frame Pump & Rendering
void oops_webview_pump(oops_webview_t *wv);
void oops_webview_render(oops_webview_t *wv, oops_surface_t *surf);

// Hardware Input Routing
void oops_webview_send_input(oops_webview_t *wv, const oops_webview_event_t *ev);

// Subsystem Accessors
oops_js_t   *oops_webview_get_js(oops_webview_t *wv);
oops_html_t *oops_webview_get_html(oops_webview_t *wv);
```

### 5.2 Input Event Structure

```c
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
        struct { uint32_t buttons; float lx, ly, rx, ry; } pad;
        struct { uint32_t keycode; uint32_t modifiers; } key;
        struct { int32_t x, y, dx, dy; uint32_t button; } mouse;
    } u;
} oops_webview_event_t;
```

---

## 6. JavaScript Host Environment & DOM Bridge

When running in `oops_webview_t`, JavaScript code has access to standard web APIs:

### 6.1 DOM APIs
- **Document**:
  - `document.getElementById(id)`
  - `document.querySelector(selector)`
  - `document.querySelectorAll(selector)`
  - `document.createElement(tagName)`
  - `document.body`, `document.head`
  - `document.addEventListener(event, callback)`
- **Element**:
  - `element.appendChild(child)`
  - `element.removeChild(child)`
  - `element.setAttribute(name, value)`
  - `element.getAttribute(name)`
  - `element.hasAttribute(name)`
  - `element.textContent` (getter & setter; mutates DOM text nodes)
  - `element.innerHTML` (getter & setter; parses fragment and re-flows)
  - `element.className`, `element.id`
  - `element.getBoundingClientRect()` -> `{ x, y, width, height, top, bottom, left, right }`
  - `element.addEventListener('click', callback)`
- **Window**:
  - `window.addEventListener('DOMContentLoaded', callback)`
  - `window.addEventListener('load', callback)`
  - `window.addEventListener('click', callback)`
  - `window.addEventListener('keydown', callback)`
  - `window.addEventListener('keyup', callback)`

### 6.2 Host Functions
- **Networking (`fetch`)**:
  - `fetch(url)` returns a Promise.
  - Automatically fetches over plain HTTP or native HTTPS (TLS 1.2/1.3).
  - Response object contains:
    - `res.ok`: Boolean (`true` for HTTP 200-299).
    - `res.status`: HTTP status code integer.
    - `res.statusText`: Status text string.
    - `res.text()`: Returns Promise resolving to body text string.
    - `res.json()`: Returns Promise resolving to parsed JSON object.
- **Console**:
  - `console.log(...)`, `console.info(...)` -> `oops_log_info("WEBVIEW", ...)`
  - `console.warn(...)` -> `oops_log_warn("WEBVIEW", ...)`
  - `console.error(...)` -> `oops_log_error("WEBVIEW", ...)`
  - `console.debug(...)` -> `oops_log_debug("WEBVIEW", ...)`
- **Timers**:
  - `setTimeout(callback, delayMs)` -> Returns integer timer ID.
  - `clearTimeout(id)`
  - `setInterval(callback, intervalMs)` -> Returns integer timer ID.
  - `clearInterval(id)`

---

## 7. Example: Dynamic Client-Side Page with `fetch()`

This demonstrates the day-0 capability: an HTML page that executes JavaScript, fetches dynamic data over HTTPS, and renders a list into the DOM:

```html
<!DOCTYPE html>
<html>
<head>
  <style>
    body { background-color: #121218; color: #f0f0f0; font-family: sans-serif; padding: 20px; }
    h1 { color: #00ddff; border-bottom: 2px solid #00ddff; padding-bottom: 8px; }
    .app-card { background: #1f1f2e; border-radius: 8px; padding: 12px; margin-bottom: 10px; border: 1px solid #33334d; }
    .app-title { font-size: 20px; font-weight: bold; color: #ffcc00; }
    .app-desc { font-size: 14px; color: #bbbbbb; margin-top: 4px; }
    .loading { color: #888888; font-style: italic; }
  </style>
</head>
<body>
  <h1>App Catalogue</h1>
  <div id="catalog-list"><p class="loading">Fetching applications...</p></div>

  <script>
    console.log("Catalogue script running...");

    fetch("https://api.example.com/apps.json")
      .then(res => res.json())
      .then(apps => {
        const list = document.getElementById("catalog-list");
        list.innerHTML = ""; // Clear loading message

        for (const app of apps) {
          const card = document.createElement("div");
          card.setAttribute("class", "app-card");

          const title = document.createElement("div");
          title.setAttribute("class", "app-title");
          title.textContent = app.name;
          card.appendChild(title);

          const desc = document.createElement("div");
          desc.setAttribute("class", "app-desc");
          desc.textContent = app.summary;
          card.appendChild(desc);

          card.addEventListener("click", () => {
            console.log("Selected application: " + app.id);
          });

          list.appendChild(card);
        }
      })
      .catch(err => {
        console.error("Failed to fetch catalogue: " + err.message);
        document.getElementById("catalog-list").textContent = "Error loading apps.";
      });
  </script>
</body>
</html>
```

### C Application Loop

```c
#include "oops/webview.h"
#include "oops/display.h"
#include "oops/input.h"

int main(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 1920, 1080);
    oops_webview_t *wv = oops_webview_create(1920, 1080);

    // Load initial URL
    oops_webview_load_url(wv, "https://local.app/index.html");

    while (1) {
        // 1. Process Hardware Input
        oops_pad_data_t pad;
        if (oops_input_poll(0, &pad) == 0) {
            oops_webview_event_t ev;
            ev.type = OOPS_WEBVIEW_EVENT_PAD;
            ev.u.pad.buttons = pad.buttons;
            ev.u.pad.lx = pad.lx;
            ev.u.pad.ly = pad.ly;
            oops_webview_send_input(wv, &ev);
        }

        // 2. Pump Timers, Network Fetches, Microtasks, Relayout
        oops_webview_pump(wv);

        // 3. Paint to Framebuffer
        uint32_t *fb = oops_display_get_framebuffer(disp);
        oops_surface_t surf = {
            .pixels = fb,
            .width = 1920,
            .height = 1080,
            .pitch = 1920,
            .layout = OOPS_SURFACE_LINEAR
        };
        oops_webview_render(wv, &surf);

        // 4. Present Frame
        oops_display_flip(disp);
    }

    oops_webview_destroy(wv);
    return 0;
}
```
