/*
 * The webview (oops/webview.h): litehtml for layout and drawing, QuickJS for scripts.
 *
 * Loading a document parses it, runs its `<script>` elements in document order (a
 * `src` is fetched over oops/http.h, relative to the base URL), then fires
 * `DOMContentLoaded` and `load` on the window. `oops_webview_pump` runs due timers,
 * pending fetches and promise jobs. A DOM change from a script marks the layout dirty,
 * and the next pump or render lays the document out again at the view's width.
 */

#include "webview_impl.hpp"
#include "dom_bridge.hpp"
#include "host_env.hpp"
#include "oops/webview.h"
#include "oops/http.h"
#include "oops/time.h"
#include "oops/system.h"
#include "litehtml/document.h"
#include "litehtml/render_item.h"
#include "litehtml/el_script.h"

#include <cstring>
#include <vector>
#include <string>

static void collect_scripts(const litehtml::element::ptr &el,
                            std::vector<litehtml::element::ptr> &out) {
    if (!el)
        return;
    if (el->tag() == litehtml::_script_) {
        out.push_back(el);
    }
    for (const auto &child : el->children()) {
        collect_scripts(child, out);
    }
}

static void execute_scripts(oops_webview_t *wv, const char *base_url) {
    if (!wv || !wv->html)
        return;
    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    if (!doc || !doc->root())
        return;

    std::vector<litehtml::element::ptr> scripts;
    collect_scripts(doc->root(), scripts);

    for (const auto &s : scripts) {
        const char *src = s->get_attr("src");
        if (src && strlen(src) > 0) {
            std::string full_url = src;
            if (src[0] != '/' && !strstr(src, "://") && base_url &&
                strlen(base_url) > 0) {
                full_url = std::string(base_url) + "/" + src;
            }
            oops_http_response_t resp;
            memset(&resp, 0, sizeof(resp));
            if (oops_http_get(full_url.c_str(), &resp) == OOPS_HTTP_OK) {
                if (resp.body) {
                    oops_js_eval(wv->js, resp.body, full_url.c_str(), NULL);
                }
                oops_http_response_free(&resp);
            } else {
                oops_log_warn("WEBVIEW", "Failed to fetch external script: %s",
                              full_url.c_str());
            }
        } else {
            std::string code;
            s->get_text(code);
            if (!code.empty()) {
                oops_js_eval(wv->js, code.c_str(), "<script>", NULL);
            }
        }
    }
}

static void dispatch_window_event(oops_webview_t *wv, const char *type) {
    if (!wv || !wv->js)
        return;
    auto *ctx = static_cast<JSContext *>(oops_js_get_context(wv->js));
    if (!ctx)
        return;

    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    dom_bridge_call_listeners(ctx, wv->window_listeners, type, JS_UNDEFINED, ev);
    JS_FreeValue(ctx, ev);
}

// A view coordinate as a document one: the view shows the document from its scroll
// offset, and litehtml hit-tests in document coordinates.
static void view_to_document(oops_webview_t *wv, int *x, int *y) {
    const auto *c =
        wv->html
            ? static_cast<const oops_container *>(oops_html_get_container(wv->html))
            : nullptr;
    if (c) {
        *x += c->m_scroll_x;
        *y += c->m_scroll_y;
    }
}

static void dispatch_mouse_event(oops_webview_t *wv, const char *type, int32_t x,
                                 int32_t y, uint32_t button) {
    if (!wv || !wv->js || !wv->html)
        return;
    auto *ctx = static_cast<JSContext *>(oops_js_get_context(wv->js));
    if (!ctx)
        return;

    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    litehtml::element::ptr hit_el = nullptr;
    if (doc && doc->root_render()) {
        int doc_x = x, doc_y = y;
        view_to_document(wv, &doc_x, &doc_y);
        hit_el = doc->root_render()->get_element_by_point(doc_x, doc_y, x, y, nullptr);
    }

    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, ev, "clientX", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, ev, "clientY", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, ev, "button", JS_NewInt32(ctx, button));

    if (hit_el) {
        JSValue el_val = dom_bridge_wrap_element(ctx, wv, hit_el);
        dom_bridge_dispatch_event(ctx, el_val, type, ev);
        JS_FreeValue(ctx, el_val);
    }

    dom_bridge_call_listeners(ctx, wv->window_listeners, type, JS_UNDEFINED, ev);
    JS_FreeValue(ctx, ev);
}

static void dispatch_key_event(oops_webview_t *wv, const char *type, uint32_t keycode,
                               uint32_t modifiers) {
    if (!wv || !wv->js)
        return;
    auto *ctx = static_cast<JSContext *>(oops_js_get_context(wv->js));
    if (!ctx)
        return;

    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, ev, "keyCode", JS_NewInt32(ctx, keycode));
    JS_SetPropertyStr(ctx, ev, "which", JS_NewInt32(ctx, keycode));
    JS_SetPropertyStr(ctx, ev, "modifiers", JS_NewInt32(ctx, modifiers));

    dom_bridge_call_listeners(ctx, wv->window_listeners, type, JS_UNDEFINED, ev);
    JS_FreeValue(ctx, ev);
}

extern "C" {

oops_webview_t *oops_webview_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        oops_log_error("WEBVIEW", "Invalid webview dimensions: %dx%d", width, height);
        return NULL;
    }

    auto *wv = new (std::nothrow) oops_webview();
    if (!wv) {
        oops_log_error("WEBVIEW", "Failed to allocate oops_webview");
        return NULL;
    }

    wv->width = width;
    wv->height = height;
    wv->dirty_layout = false;
    wv->next_timer_id = 0;

    wv->js = oops_js_create();
    if (!wv->js) {
        oops_log_error("WEBVIEW", "Failed to create JS context");
        delete wv;
        return NULL;
    }

    wv->html = oops_html_create(width, height);
    if (!wv->html) {
        oops_log_error("WEBVIEW", "Failed to create HTML renderer");
        oops_js_destroy(wv->js);
        delete wv;
        return NULL;
    }

    auto *ctx = static_cast<JSContext *>(oops_js_get_context(wv->js));
    JS_SetContextOpaque(ctx, wv);

    dom_bridge_init(ctx, wv,
                    static_cast<oops_container *>(oops_html_get_container(wv->html)));
    host_env_init(ctx, wv);

    oops_log_info("WEBVIEW", "Created webview instance (%dx%d)", width, height);
    return wv;
}

void oops_webview_destroy(oops_webview_t *wv) {
    if (!wv)
        return;

    if (wv->js) {
        auto *ctx = static_cast<JSContext *>(oops_js_get_context(wv->js));
        if (ctx) {
            host_env_cleanup(ctx, wv);
            dom_bridge_free_listeners(ctx, wv->window_listeners);
            for (auto &entry : wv->element_listeners)
                dom_bridge_free_listeners(ctx, entry.second.list);
            wv->element_listeners.clear();
        }
    }

    if (wv->html) {
        oops_html_destroy(wv->html);
        wv->html = NULL;
    }

    if (wv->js) {
        oops_js_destroy(wv->js);
        wv->js = NULL;
    }

    delete wv;
    oops_log_debug("WEBVIEW", "Destroyed webview instance");
}

int oops_webview_load_url(oops_webview_t *wv, const char *url) {
    if (!wv || !url) {
        oops_log_error("WEBVIEW", "oops_webview_load_url called with NULL arguments");
        return -1;
    }

    oops_log_info("WEBVIEW", "Fetching URL: %s", url);
    oops_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int err = oops_http_get(url, &resp);
    if (err != OOPS_HTTP_OK) {
        oops_log_error("WEBVIEW", "Failed to fetch URL %s: error %d", url, err);
        return -1;
    }

    int ret = oops_webview_load_html(wv, resp.body ? resp.body : "", url);
    oops_http_response_free(&resp);
    return ret;
}

int oops_webview_load_html(oops_webview_t *wv, const char *html, const char *base_url) {
    if (!wv || !html) {
        oops_log_error("WEBVIEW", "oops_webview_load_html called with NULL arguments");
        return -1;
    }

    // The old document's elements go with it, and so do their listeners.
    auto *ctx = static_cast<JSContext *>(oops_js_get_context(wv->js));
    if (ctx) {
        for (auto &entry : wv->element_listeners)
            dom_bridge_free_listeners(ctx, entry.second.list);
    }
    wv->element_listeners.clear();

    int ret = oops_html_load(wv->html, html, base_url);
    if (ret != 0) {
        oops_log_error("WEBVIEW", "Failed to parse HTML document");
        return ret;
    }

    execute_scripts(wv, base_url);
    oops_js_execute_pending_jobs(wv->js);

    dispatch_window_event(wv, "DOMContentLoaded");
    oops_js_execute_pending_jobs(wv->js);

    dispatch_window_event(wv, "load");
    oops_js_execute_pending_jobs(wv->js);

    if (wv->dirty_layout && wv->html) {
        auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
        if (doc) {
            doc->render(wv->width);
        }
        wv->dirty_layout = false;
    }

    oops_log_info("WEBVIEW", "Loaded HTML document successfully");
    return 0;
}

void oops_webview_pump(oops_webview_t *wv) {
    if (!wv)
        return;

    auto *ctx = static_cast<JSContext *>(oops_js_get_context(wv->js));
    if (ctx) {
        host_env_pump_timers(ctx, wv, (int64_t)oops_time_get_ms());
        host_env_pump_fetches(ctx, wv);
    }

    if (wv->js) {
        oops_js_execute_pending_jobs(wv->js);
    }

    if (wv->dirty_layout && wv->html) {
        auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
        if (doc) {
            doc->render(wv->width);
        }
        wv->dirty_layout = false;
    }
}

void oops_webview_render(oops_webview_t *wv, oops_surface_t *surf) {
    if (!wv || !surf)
        return;

    if (wv->dirty_layout && wv->html) {
        auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
        if (doc) {
            doc->render(wv->width);
        }
        wv->dirty_layout = false;
    }

    if (wv->html) {
        oops_html_render(wv->html, surf);
    }
}

void oops_webview_send_input(oops_webview_t *wv, const oops_webview_event_t *ev) {
    if (!wv || !ev)
        return;

    auto *doc =
        wv->html ? static_cast<litehtml::document *>(oops_html_get_document(wv->html))
                 : nullptr;
    int doc_x = ev->u.mouse.x, doc_y = ev->u.mouse.y;
    view_to_document(wv, &doc_x, &doc_y);

    switch (ev->type) {
    case OOPS_WEBVIEW_EVENT_MOUSE_MOVE:
        if (doc) {
            doc->on_mouse_over(doc_x, doc_y, ev->u.mouse.x, ev->u.mouse.y,
                               [](const litehtml::position &) {});
        }
        dispatch_mouse_event(wv, "mousemove", ev->u.mouse.x, ev->u.mouse.y,
                             ev->u.mouse.button);
        break;
    case OOPS_WEBVIEW_EVENT_MOUSE_BUTTON_DOWN:
        if (doc) {
            doc->on_lbutton_down(doc_x, doc_y, ev->u.mouse.x, ev->u.mouse.y,
                                 [](const litehtml::position &) {});
        }
        dispatch_mouse_event(wv, "mousedown", ev->u.mouse.x, ev->u.mouse.y,
                             ev->u.mouse.button);
        break;
    case OOPS_WEBVIEW_EVENT_MOUSE_BUTTON_UP:
        if (doc) {
            doc->on_lbutton_up(doc_x, doc_y, ev->u.mouse.x, ev->u.mouse.y,
                               [](const litehtml::position &) {});
        }
        dispatch_mouse_event(wv, "mouseup", ev->u.mouse.x, ev->u.mouse.y,
                             ev->u.mouse.button);
        dispatch_mouse_event(wv, "click", ev->u.mouse.x, ev->u.mouse.y,
                             ev->u.mouse.button);
        break;
    case OOPS_WEBVIEW_EVENT_MOUSE_WHEEL:
        if (wv->html) {
            oops_html_scroll(wv->html, 0, -ev->u.mouse.dy);
        }
        dispatch_mouse_event(wv, "wheel", ev->u.mouse.x, ev->u.mouse.y, 0);
        break;
    case OOPS_WEBVIEW_EVENT_KEY_DOWN:
        dispatch_key_event(wv, "keydown", ev->u.key.keycode, ev->u.key.modifiers);
        break;
    case OOPS_WEBVIEW_EVENT_KEY_UP:
        dispatch_key_event(wv, "keyup", ev->u.key.keycode, ev->u.key.modifiers);
        break;
    case OOPS_WEBVIEW_EVENT_PAD:
        break;
    }

    if (wv->js) {
        oops_js_execute_pending_jobs(wv->js);
    }
}

oops_js_t *oops_webview_get_js(oops_webview_t *wv) {
    return wv ? wv->js : nullptr;
}

oops_html_t *oops_webview_get_html(oops_webview_t *wv) {
    return wv ? wv->html : nullptr;
}

} // extern "C"
