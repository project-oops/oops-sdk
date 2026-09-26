/*
 * The DOM subset a page's scripts see: `document`, `window` and an `Element` class
 * over litehtml elements.
 *
 * Each wrap of a litehtml element creates a new JS object holding a shared pointer to
 * the element, so the element stays alive while a script still references it. Its
 * listeners are the webview's, keyed by element, so every wrapper sees them. A mutation
 * marks the webview's layout dirty.
 */

#include "dom_bridge.hpp"
#include "webview_impl.hpp"
#include "html_container.hpp"
#include "oops/system.h"

#include <cstring>
#include <algorithm>

static JSClassID js_element_class_id = 0;

static void js_element_finalizer(JSRuntime * /*rt*/, JSValue val) {
    delete static_cast<dom_element_data *>(JS_GetOpaque(val, js_element_class_id));
}

static JSClassDef js_element_class = {"Element", js_element_finalizer, nullptr, nullptr,
                                      nullptr};

static JSValue js_element_append_child(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
    if (argc < 1)
        return JS_UNDEFINED;
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    auto *child_data =
        static_cast<dom_element_data *>(JS_GetOpaque(argv[0], js_element_class_id));
    if (!data || !child_data || !data->el || !child_data->el) {
        return JS_ThrowTypeError(ctx, "Invalid element in appendChild");
    }

    data->el->appendChild(child_data->el);
    if (data->wv) {
        data->wv->mark_dirty();
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_element_remove_child(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
    if (argc < 1)
        return JS_UNDEFINED;
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    auto *child_data =
        static_cast<dom_element_data *>(JS_GetOpaque(argv[0], js_element_class_id));
    if (!data || !child_data || !data->el || !child_data->el) {
        return JS_ThrowTypeError(ctx, "Invalid element in removeChild");
    }

    data->el->removeChild(child_data->el);
    if (data->wv) {
        data->wv->mark_dirty();
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_element_set_attribute(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
    if (argc < 2)
        return JS_UNDEFINED;
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_UNDEFINED;

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (name && val) {
        data->el->set_attr(name, val);
        if (data->wv) {
            data->wv->mark_dirty();
        }
    }
    if (name)
        JS_FreeCString(ctx, name);
    if (val)
        JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue js_element_get_attribute(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
    if (argc < 1)
        return JS_NULL;
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NULL;

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_NULL;

    const char *val = data->el->get_attr(name);
    JS_FreeCString(ctx, name);
    return val ? JS_NewString(ctx, val) : JS_NULL;
}

static JSValue js_element_has_attribute(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
    if (argc < 1)
        return JS_NewBool(ctx, 0);
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NewBool(ctx, 0);

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_NewBool(ctx, 0);

    bool has = (data->el->get_attr(name) != nullptr);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, has ? 1 : 0);
}

static JSValue js_element_add_event_listener(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    if (argc < 2)
        return JS_UNDEFINED;
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->wv || !data->el || !JS_IsFunction(ctx, argv[1]))
        return JS_UNDEFINED;

    const char *event_type = JS_ToCString(ctx, argv[0]);
    if (event_type) {
        dom_element_listeners &entry = data->wv->element_listeners[data->el.get()];
        entry.el = data->el;
        entry.list.push_back({JS_DupValue(ctx, argv[1]), std::string(event_type)});
        JS_FreeCString(ctx, event_type);
    }
    return JS_UNDEFINED;
}

static JSValue js_element_query_selector(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_NULL;
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NULL;

    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel)
        return JS_NULL;

    auto res = data->el->select_one(sel);
    JS_FreeCString(ctx, sel);
    if (!res)
        return JS_NULL;

    return dom_bridge_wrap_element(ctx, data->wv, res);
}

static JSValue js_element_query_selector_all(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_NewArray(ctx);
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NewArray(ctx);

    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel)
        return JS_NewArray(ctx);

    auto list = data->el->select_all(sel);
    JS_FreeCString(ctx, sel);

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (const auto &item : list) {
        JSValue item_val = dom_bridge_wrap_element(ctx, data->wv, item);
        JS_SetPropertyUint32(ctx, arr, idx++, item_val);
    }
    return arr;
}

static JSValue js_element_get_bounding_client_rect(JSContext *ctx,
                                                   JSValueConst this_val, int /*argc*/,
                                                   JSValueConst * /*argv*/) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_UNDEFINED;

    litehtml::position pos = data->el->get_placement();
    JSValue rect = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rect, "x", JS_NewFloat64(ctx, (double)pos.x));
    JS_SetPropertyStr(ctx, rect, "y", JS_NewFloat64(ctx, (double)pos.y));
    JS_SetPropertyStr(ctx, rect, "width", JS_NewFloat64(ctx, (double)pos.width));
    JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, (double)pos.height));
    JS_SetPropertyStr(ctx, rect, "left", JS_NewFloat64(ctx, (double)pos.x));
    JS_SetPropertyStr(ctx, rect, "top", JS_NewFloat64(ctx, (double)pos.y));
    JS_SetPropertyStr(ctx, rect, "right", JS_NewFloat64(ctx, (double)pos.right()));
    JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, (double)pos.bottom()));
    return rect;
}

static JSValue js_element_get_text_content(JSContext *ctx, JSValueConst this_val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NewString(ctx, "");

    std::string text;
    data->el->get_text(text);
    return JS_NewString(ctx, text.c_str());
}

static JSValue js_element_set_text_content(JSContext *ctx, JSValueConst this_val,
                                           JSValueConst val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_UNDEFINED;

    const char *s = JS_ToCString(ctx, val);
    if (s) {
        auto doc = data->el->get_document();
        if (doc) {
            doc->append_children_from_string(*data->el, s, true);
        }
        JS_FreeCString(ctx, s);
        if (data->wv) {
            data->wv->mark_dirty();
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_inner_html(JSContext *ctx, JSValueConst this_val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NewString(ctx, "");
    std::string text;
    data->el->get_text(text);
    return JS_NewString(ctx, text.c_str());
}

static JSValue js_element_set_inner_html(JSContext *ctx, JSValueConst this_val,
                                         JSValueConst val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_UNDEFINED;

    const char *html = JS_ToCString(ctx, val);
    if (html) {
        auto doc = data->el->get_document();
        if (doc) {
            doc->append_children_from_string(*data->el, html, true);
        }
        JS_FreeCString(ctx, html);
        if (data->wv) {
            data->wv->mark_dirty();
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_class_name(JSContext *ctx, JSValueConst this_val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NewString(ctx, "");
    const char *c = data->el->get_attr("class", "");
    return JS_NewString(ctx, c ? c : "");
}

static JSValue js_element_set_class_name(JSContext *ctx, JSValueConst this_val,
                                         JSValueConst val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_UNDEFINED;
    const char *c = JS_ToCString(ctx, val);
    if (c) {
        data->el->set_attr("class", c);
        JS_FreeCString(ctx, c);
        if (data->wv) {
            data->wv->mark_dirty();
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_id(JSContext *ctx, JSValueConst this_val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_NewString(ctx, "");
    const char *id = data->el->get_attr("id", "");
    return JS_NewString(ctx, id ? id : "");
}

static JSValue js_element_set_id(JSContext *ctx, JSValueConst this_val,
                                 JSValueConst val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_UNDEFINED;
    const char *id = JS_ToCString(ctx, val);
    if (id) {
        data->el->set_attr("id", id);
        JS_FreeCString(ctx, id);
        if (data->wv) {
            data->wv->mark_dirty();
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_style(JSContext *ctx, JSValueConst this_val) {
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(this_val, js_element_class_id));
    if (!data || !data->el)
        return JS_UNDEFINED;

    JSValue style_obj = JS_NewObject(ctx);
    // An empty object: writes to it do not reach the element's style.
    return style_obj;
}

static const JSCFunctionListEntry js_element_proto_funcs[] = {
    JS_CFUNC_DEF("appendChild", 1, js_element_append_child),
    JS_CFUNC_DEF("removeChild", 1, js_element_remove_child),
    JS_CFUNC_DEF("setAttribute", 2, js_element_set_attribute),
    JS_CFUNC_DEF("getAttribute", 1, js_element_get_attribute),
    JS_CFUNC_DEF("hasAttribute", 1, js_element_has_attribute),
    JS_CFUNC_DEF("addEventListener", 2, js_element_add_event_listener),
    JS_CFUNC_DEF("querySelector", 1, js_element_query_selector),
    JS_CFUNC_DEF("querySelectorAll", 1, js_element_query_selector_all),
    JS_CFUNC_DEF("getBoundingClientRect", 0, js_element_get_bounding_client_rect),
    JS_CGETSET_DEF("textContent", js_element_get_text_content,
                   js_element_set_text_content),
    JS_CGETSET_DEF("innerHTML", js_element_get_inner_html, js_element_set_inner_html),
    JS_CGETSET_DEF("className", js_element_get_class_name, js_element_set_class_name),
    JS_CGETSET_DEF("id", js_element_get_id, js_element_set_id),
    JS_CGETSET_DEF("style", js_element_get_style, nullptr),
};

JSValue dom_bridge_wrap_element(JSContext *ctx, oops_webview_t *wv,
                                const litehtml::element::ptr &el) {
    if (!el)
        return JS_NULL;

    JSValue obj = JS_NewObjectClass(ctx, js_element_class_id);
    if (JS_IsException(obj))
        return obj;

    auto *data = new dom_element_data();
    data->el = el;
    data->wv = wv;
    JS_SetOpaque(obj, data);
    return obj;
}

// Document functions
static JSValue js_document_get_element_by_id(JSContext *ctx, JSValueConst /*this_val*/,
                                             int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_NULL;
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv || !wv->html)
        return JS_NULL;

    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    if (!doc || !doc->root())
        return JS_NULL;

    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id)
        return JS_NULL;

    std::string selector = "#" + std::string(id);
    auto el = doc->root()->select_one(selector);
    JS_FreeCString(ctx, id);

    return el ? dom_bridge_wrap_element(ctx, wv, el) : JS_NULL;
}

static JSValue js_document_query_selector(JSContext *ctx, JSValueConst /*this_val*/,
                                          int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_NULL;
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv || !wv->html)
        return JS_NULL;

    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    if (!doc || !doc->root())
        return JS_NULL;

    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel)
        return JS_NULL;

    auto el = doc->root()->select_one(sel);
    JS_FreeCString(ctx, sel);

    return el ? dom_bridge_wrap_element(ctx, wv, el) : JS_NULL;
}

static JSValue js_document_query_selector_all(JSContext *ctx, JSValueConst /*this_val*/,
                                              int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_NewArray(ctx);
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv || !wv->html)
        return JS_NewArray(ctx);

    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    if (!doc || !doc->root())
        return JS_NewArray(ctx);

    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel)
        return JS_NewArray(ctx);

    auto list = doc->root()->select_all(sel);
    JS_FreeCString(ctx, sel);

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (const auto &item : list) {
        JSValue item_val = dom_bridge_wrap_element(ctx, wv, item);
        JS_SetPropertyUint32(ctx, arr, idx++, item_val);
    }
    return arr;
}

static JSValue js_document_create_element(JSContext *ctx, JSValueConst /*this_val*/,
                                          int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_NULL;
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv || !wv->html)
        return JS_NULL;

    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    if (!doc)
        return JS_NULL;

    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag)
        return JS_NULL;

    auto el = doc->create_element(tag, {});
    JS_FreeCString(ctx, tag);

    return el ? dom_bridge_wrap_element(ctx, wv, el) : JS_NULL;
}

static JSValue js_document_get_body(JSContext *ctx, JSValueConst /*this_val*/,
                                    int /*argc*/, JSValueConst * /*argv*/) {
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv || !wv->html)
        return JS_NULL;

    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    if (!doc || !doc->root())
        return JS_NULL;

    auto body = doc->root()->select_one("body");
    return body ? dom_bridge_wrap_element(ctx, wv, body) : JS_NULL;
}

static JSValue js_document_get_head(JSContext *ctx, JSValueConst /*this_val*/,
                                    int /*argc*/, JSValueConst * /*argv*/) {
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv || !wv->html)
        return JS_NULL;

    auto *doc = static_cast<litehtml::document *>(oops_html_get_document(wv->html));
    if (!doc || !doc->root())
        return JS_NULL;

    auto head = doc->root()->select_one("head");
    return head ? dom_bridge_wrap_element(ctx, wv, head) : JS_NULL;
}

static JSValue js_window_add_event_listener(JSContext *ctx, JSValueConst /*this_val*/,
                                            int argc, JSValueConst *argv) {
    if (argc < 2)
        return JS_UNDEFINED;
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv || !JS_IsFunction(ctx, argv[1]))
        return JS_UNDEFINED;

    const char *event_type = JS_ToCString(ctx, argv[0]);
    if (event_type) {
        wv->window_listeners.push_back(
            {JS_DupValue(ctx, argv[1]), std::string(event_type)});
        JS_FreeCString(ctx, event_type);
    }
    return JS_UNDEFINED;
}

int dom_bridge_init(JSContext *ctx, oops_webview_t *wv,
                    oops_container * /*container*/) {
    JS_NewClassID(&js_element_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_element_class_id, &js_element_class);

    JSValue element_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, element_proto, js_element_proto_funcs,
                               sizeof(js_element_proto_funcs) /
                                   sizeof(js_element_proto_funcs[0]));
    JS_SetClassProto(ctx, js_element_class_id, element_proto);

    JSValue global_obj = JS_GetGlobalObject(ctx);

    // document object
    JSValue doc_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(
        ctx, doc_obj, "getElementById",
        JS_NewCFunction(ctx, js_document_get_element_by_id, "getElementById", 1));
    JS_SetPropertyStr(
        ctx, doc_obj, "querySelector",
        JS_NewCFunction(ctx, js_document_query_selector, "querySelector", 1));
    JS_SetPropertyStr(
        ctx, doc_obj, "querySelectorAll",
        JS_NewCFunction(ctx, js_document_query_selector_all, "querySelectorAll", 1));
    JS_SetPropertyStr(
        ctx, doc_obj, "createElement",
        JS_NewCFunction(ctx, js_document_create_element, "createElement", 1));
    JS_SetPropertyStr(
        ctx, doc_obj, "addEventListener",
        JS_NewCFunction(ctx, js_window_add_event_listener, "addEventListener", 2));

    JSAtom body_atom = JS_NewAtom(ctx, "body");
    JS_DefinePropertyGetSet(ctx, doc_obj, body_atom,
                            JS_NewCFunction(ctx, js_document_get_body, "get body", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, body_atom);

    JSAtom head_atom = JS_NewAtom(ctx, "head");
    JS_DefinePropertyGetSet(ctx, doc_obj, head_atom,
                            JS_NewCFunction(ctx, js_document_get_head, "get head", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, head_atom);

    JS_SetPropertyStr(ctx, global_obj, "document", JS_DupValue(ctx, doc_obj));

    // window object
    JS_SetPropertyStr(ctx, global_obj, "window", JS_DupValue(ctx, global_obj));
    JS_SetPropertyStr(
        ctx, global_obj, "addEventListener",
        JS_NewCFunction(ctx, js_window_add_event_listener, "addEventListener", 2));

    JS_FreeValue(ctx, doc_obj);
    JS_FreeValue(ctx, global_obj);

    oops_log_debug(
        "WEBVIEW",
        "DOM bridge initialized with Document, Window, and Element prototypes");
    return 0;
}

void dom_bridge_dispatch_event(JSContext *ctx, JSValue elem_obj, const char *event_type,
                               JSValue event_obj) {
    if (!event_type)
        return;
    auto *data =
        static_cast<dom_element_data *>(JS_GetOpaque(elem_obj, js_element_class_id));
    if (!data || !data->wv || !data->el)
        return;
    const auto it = data->wv->element_listeners.find(data->el.get());
    if (it == data->wv->element_listeners.end())
        return;
    dom_bridge_call_listeners(ctx, it->second.list, event_type, elem_obj, event_obj);
}

void dom_bridge_call_listeners(JSContext *ctx,
                               const std::vector<dom_event_listener> &list,
                               const char *type, JSValueConst this_val,
                               JSValueConst event_obj) {
    // By index up to the count at entry: a listener that adds one grows, and may
    // reallocate, the vector being walked. The callback is held for the call.
    const size_t count = list.size();
    for (size_t i = 0; i < count; i++) {
        if (list[i].event_type != type)
            continue;
        JSValue fn = JS_DupValue(ctx, list[i].callback);
        JSValue argv[1] = {event_obj};
        JSValue ret = JS_Call(ctx, fn, this_val, 1, argv);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            const char *err = JS_ToCString(ctx, ex);
            oops_log_error("WEBVIEW", "Exception in %s listener: %s", type,
                           err ? err : "unknown");
            if (err)
                JS_FreeCString(ctx, err);
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
    }
}

void dom_bridge_free_listeners(JSContext *ctx, std::vector<dom_event_listener> &list) {
    for (auto &l : list)
        JS_FreeValue(ctx, l.callback);
    list.clear();
}
