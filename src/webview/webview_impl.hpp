#ifndef OOPS_WEBVIEW_IMPL_HPP
#define OOPS_WEBVIEW_IMPL_HPP

#include "oops/webview.h"
#include "oops/js.h"
#include "oops/html.h"
#include "dom_bridge.hpp"
#include "host_env.hpp"
#include "html_container.hpp"

#include <map>
#include <memory>
#include <vector>

// The webview's state, shared by webview.cpp, dom_bridge.cpp and host_env.cpp. The JS
// context's opaque points at it.
struct oops_webview {
    oops_js_t *js;
    oops_html_t *html;
    int width;
    int height;
    bool dirty_layout;

    int next_timer_id;
    std::vector<webview_timer> timers;
    std::vector<webview_fetch_task> pending_fetches;
    std::vector<dom_event_listener> window_listeners;
    std::map<const litehtml::element *, dom_element_listeners> element_listeners;

    void mark_dirty() { dirty_layout = true; }
};

#endif // OOPS_WEBVIEW_IMPL_HPP
