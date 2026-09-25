#include "host_env.hpp"
#include "webview_impl.hpp"
#include "oops/http.h"
#include "oops/time.h"
#include "oops/system.h"

#include <cstring>
#include <algorithm>
#include <string>

static JSValue js_console_log_impl(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv, int level) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            msg += s;
            JS_FreeCString(ctx, s);
        }
    }
    switch (level) {
        case 0: oops_log_debug("WEBVIEW", "%s", msg.c_str()); break;
        case 1: oops_log_info("WEBVIEW", "%s", msg.c_str()); break;
        case 2: oops_log_warn("WEBVIEW", "%s", msg.c_str()); break;
        case 3: oops_log_error("WEBVIEW", "%s", msg.c_str()); break;
        default: oops_log_info("WEBVIEW", "%s", msg.c_str()); break;
    }
    return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, 1);
}

static JSValue js_console_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, 1);
}

static JSValue js_console_warn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, 2);
}

static JSValue js_console_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, 3);
}

static JSValue js_console_debug(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, 0);
}

static JSValue js_set_timeout(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv) return JS_UNDEFINED;

    int64_t delay = 0;
    if (argc >= 2) {
        JS_ToInt64(ctx, &delay, argv[1]);
        if (delay < 0) delay = 0;
    }

    int id = ++wv->next_timer_id;
    int64_t now = (int64_t)oops_time_get_ms();
    wv->timers.push_back({ id, JS_DupValue(ctx, argv[0]), delay, now + delay, false });
    return JS_NewInt32(ctx, id);
}

static JSValue js_clear_timeout(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv) return JS_UNDEFINED;

    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);

    for (auto it = wv->timers.begin(); it != wv->timers.end(); ++it) {
        if (it->id == id) {
            JS_FreeValue(ctx, it->callback);
            wv->timers.erase(it);
            break;
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_set_interval(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv) return JS_UNDEFINED;

    int64_t delay = 0;
    if (argc >= 2) {
        JS_ToInt64(ctx, &delay, argv[1]);
        if (delay < 0) delay = 0;
    }

    int id = ++wv->next_timer_id;
    int64_t now = (int64_t)oops_time_get_ms();
    wv->timers.push_back({ id, JS_DupValue(ctx, argv[0]), delay, now + delay, true });
    return JS_NewInt32(ctx, id);
}

static JSValue js_clear_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_clear_timeout(ctx, this_val, argc, argv);
}

static JSValue js_response_text(JSContext *ctx, JSValueConst this_val, int /*argc*/, JSValueConst * /*argv*/) {
    JSValue body_val = JS_GetPropertyStr(ctx, this_val, "_body");
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    JSValue args[1] = { body_val };
    JSValue ret = JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    JS_FreeValue(ctx, body_val);
    return promise;
}

static JSValue js_response_json(JSContext *ctx, JSValueConst this_val, int /*argc*/, JSValueConst * /*argv*/) {
    JSValue body_val = JS_GetPropertyStr(ctx, this_val, "_body");
    const char *str = JS_ToCString(ctx, body_val);
    JSValue parsed = JS_NULL;
    if (str) {
        parsed = JS_ParseJSON(ctx, str, strlen(str), "<fetch>");
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, body_val);

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(parsed)) {
        JSValue ex = JS_GetException(ctx);
        JSValue args[1] = { ex };
        JSValue ret = JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, ex);
    } else {
        JSValue args[1] = { parsed };
        JSValue ret = JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, parsed);
    }
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch requires at least 1 argument");
    auto *wv = static_cast<oops_webview_t *>(JS_GetContextOpaque(ctx));
    if (!wv) return JS_ThrowInternalError(ctx, "No webview context");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_ThrowTypeError(ctx, "Invalid URL");

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        JS_FreeCString(ctx, url);
        return promise;
    }

    webview_fetch_task task;
    task.url = url;
    JS_FreeCString(ctx, url);
    task.resolve_func = resolving_funcs[0];
    task.reject_func = resolving_funcs[1];
    task.completed = false;
    task.status = 0;

    wv->pending_fetches.push_back(task);
    return promise;
}

int host_env_init(JSContext *ctx, oops_webview_t *wv) {
    (void)wv;
    JSValue global_obj = JS_GetGlobalObject(ctx);

    // console object
    JSValue console_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console_obj, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console_obj, "info", JS_NewCFunction(ctx, js_console_info, "info", 1));
    JS_SetPropertyStr(ctx, console_obj, "warn", JS_NewCFunction(ctx, js_console_warn, "warn", 1));
    JS_SetPropertyStr(ctx, console_obj, "error", JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, console_obj, "debug", JS_NewCFunction(ctx, js_console_debug, "debug", 1));
    JS_SetPropertyStr(ctx, global_obj, "console", console_obj);

    // timers
    JS_SetPropertyStr(ctx, global_obj, "setTimeout", JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global_obj, "clearTimeout", JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global_obj, "setInterval", JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global_obj, "clearInterval", JS_NewCFunction(ctx, js_clear_interval, "clearInterval", 1));

    // fetch
    JS_SetPropertyStr(ctx, global_obj, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));

    JS_FreeValue(ctx, global_obj);
    oops_log_debug("WEBVIEW", "Host environment initialized: console, timers, fetch");
    return 0;
}

void host_env_pump_timers(JSContext *ctx, oops_webview_t *wv, int64_t now_ms) {
    if (!wv || wv->timers.empty()) return;

    std::vector<webview_timer> active = wv->timers;
    for (size_t i = 0; i < active.size(); ++i) {
        if (now_ms >= active[i].next_fire_ms) {
            auto it = std::find_if(wv->timers.begin(), wv->timers.end(),
                                   [&](const webview_timer& t) { return t.id == active[i].id; });
            if (it != wv->timers.end()) {
                JSValue ret = JS_Call(ctx, it->callback, JS_UNDEFINED, 0, NULL);
                if (JS_IsException(ret)) {
                    JSValue ex = JS_GetException(ctx);
                    const char *err = JS_ToCString(ctx, ex);
                    oops_log_error("WEBVIEW", "Timer callback exception: %s", err ? err : "unknown");
                    if (err) JS_FreeCString(ctx, err);
                    JS_FreeValue(ctx, ex);
                }
                JS_FreeValue(ctx, ret);

                it = std::find_if(wv->timers.begin(), wv->timers.end(),
                                  [&](const webview_timer& t) { return t.id == active[i].id; });
                if (it != wv->timers.end()) {
                    if (it->is_interval) {
                        it->next_fire_ms = now_ms + it->delay_ms;
                    } else {
                        JS_FreeValue(ctx, it->callback);
                        wv->timers.erase(it);
                    }
                }
            }
        }
    }
}

void host_env_pump_fetches(JSContext *ctx, oops_webview_t *wv) {
    if (!wv || wv->pending_fetches.empty()) return;

    std::vector<webview_fetch_task> current_fetches = std::move(wv->pending_fetches);
    wv->pending_fetches.clear();

    for (auto& task : current_fetches) {
        oops_http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        int err = oops_http_get(task.url.c_str(), &resp);
        if (err != OOPS_HTTP_OK) {
            JSValue err_val = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, err_val, "message", JS_NewString(ctx, "Network request failed"));
            JSValue args[1] = { err_val };
            JSValue ret = JS_Call(ctx, task.reject_func, JS_UNDEFINED, 1, args);
            JS_FreeValue(ctx, err_val);
            JS_FreeValue(ctx, ret);
        } else {
            JSValue res_obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, res_obj, "status", JS_NewInt32(ctx, resp.status_code));
            JS_SetPropertyStr(ctx, res_obj, "ok", JS_NewBool(ctx, resp.status_code >= 200 && resp.status_code < 300));
            JS_SetPropertyStr(ctx, res_obj, "statusText", JS_NewString(ctx, resp.status_code == 200 ? "OK" : ""));
            JS_SetPropertyStr(ctx, res_obj, "_body", JS_NewString(ctx, resp.body ? resp.body : ""));
            JS_SetPropertyStr(ctx, res_obj, "text", JS_NewCFunction(ctx, js_response_text, "text", 0));
            JS_SetPropertyStr(ctx, res_obj, "json", JS_NewCFunction(ctx, js_response_json, "json", 0));

            JSValue args[1] = { res_obj };
            JSValue ret = JS_Call(ctx, task.resolve_func, JS_UNDEFINED, 1, args);
            JS_FreeValue(ctx, res_obj);
            JS_FreeValue(ctx, ret);

            oops_http_response_free(&resp);
        }

        JS_FreeValue(ctx, task.resolve_func);
        JS_FreeValue(ctx, task.reject_func);
    }
}

void host_env_cleanup(JSContext *ctx, oops_webview_t *wv) {
    if (!wv) return;

    for (auto& t : wv->timers) {
        JS_FreeValue(ctx, t.callback);
    }
    wv->timers.clear();

    for (auto& task : wv->pending_fetches) {
        JS_FreeValue(ctx, task.resolve_func);
        JS_FreeValue(ctx, task.reject_func);
    }
    wv->pending_fetches.clear();
}
