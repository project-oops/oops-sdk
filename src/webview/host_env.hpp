#ifndef OOPS_HOST_ENV_HPP
#define OOPS_HOST_ENV_HPP

// The browser globals (console, timers, fetch) a webview installs in its JS context.

#include "oops/webview.h"
#include "quickjs/quickjs.h"
#include <string>
#include <vector>

struct webview_timer {
    int id;
    JSValue callback;
    int64_t delay_ms;
    int64_t next_fire_ms;
    bool is_interval;
};

struct webview_fetch_task {
    std::string url;
    JSValue resolve_func;
    JSValue reject_func;
    bool completed;
    int status;
    std::string body;
};

// Initializes console.*, setTimeout/clearTimeout, setInterval/clearInterval, and
// fetch()
int host_env_init(JSContext *ctx, oops_webview_t *wv);

// Pumps active timers and dispatches callbacks
void host_env_pump_timers(JSContext *ctx, oops_webview_t *wv, int64_t now_ms);

// Pumps active HTTP fetch tasks and resolves/rejects promises
void host_env_pump_fetches(JSContext *ctx, oops_webview_t *wv);

// Cleans up all pending timers and fetches
void host_env_cleanup(JSContext *ctx, oops_webview_t *wv);

#endif // OOPS_HOST_ENV_HPP
