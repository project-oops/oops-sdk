#ifndef OOPS_DOM_BRIDGE_HPP
#define OOPS_DOM_BRIDGE_HPP

// The `document`, `window` and `Element` objects a webview exposes to its scripts.

#include "oops/webview.h"
#include "quickjs/quickjs.h"
#include "litehtml/html.h"
#include "litehtml/document.h"
#include "litehtml/element.h"

#include <memory>
#include <string>
#include <vector>
#include <map>

class oops_container;

struct dom_event_listener {
    JSValue callback;
    std::string event_type;
};

struct dom_element_data {
    litehtml::element::ptr el;
    oops_webview_t *wv;
    std::vector<dom_event_listener> listeners;
};

// Initializes DOM classes and prototypes in the QuickJS context
int dom_bridge_init(JSContext *ctx, oops_webview_t *wv, oops_container *container);

// Wraps a litehtml::element into a QuickJS Element object
JSValue dom_bridge_wrap_element(JSContext *ctx, oops_webview_t *wv,
                                const litehtml::element::ptr &el);

// Dispatch DOM event to an element
void dom_bridge_dispatch_event(JSContext *ctx, JSValue elem_obj, const char *event_type,
                               JSValue event_obj);

#endif // OOPS_DOM_BRIDGE_HPP
