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

// One element's listeners, kept by the webview rather than by a wrapper, since each
// lookup of an element makes a new wrapper. `el` keeps the element, and so the key's
// address, alive while it has listeners.
struct dom_element_listeners {
    litehtml::element::ptr el;
    std::vector<dom_event_listener> list;
};

struct dom_element_data {
    litehtml::element::ptr el;
    oops_webview_t *wv;
};

// Calls the listeners in `list` for `type` that existed when the call began; one added
// by a listener waits for the next dispatch, as in the DOM.
void dom_bridge_call_listeners(JSContext *ctx,
                               const std::vector<dom_event_listener> &list,
                               const char *type, JSValueConst this_val,
                               JSValueConst event_obj);

// Frees every listener's callback and empties `list`.
void dom_bridge_free_listeners(JSContext *ctx, std::vector<dom_event_listener> &list);

// Initializes DOM classes and prototypes in the QuickJS context
int dom_bridge_init(JSContext *ctx, oops_webview_t *wv, oops_container *container);

// Wraps a litehtml::element into a QuickJS Element object
JSValue dom_bridge_wrap_element(JSContext *ctx, oops_webview_t *wv,
                                const litehtml::element::ptr &el);

// Dispatch DOM event to an element
void dom_bridge_dispatch_event(JSContext *ctx, JSValue elem_obj, const char *event_type,
                               JSValue event_obj);

#endif // OOPS_DOM_BRIDGE_HPP
