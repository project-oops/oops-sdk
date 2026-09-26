/*
 * JavaScript (oops/js.h) over QuickJS: one runtime and one context per `oops_js_t`.
 *
 * Values cross the boundary as `oops_js_value_t` copies; strings are duplicated and
 * freed with `oops_js_free_value`. A native function is registered in a fixed table
 * and reached through one thunk, which finds its binding by the QuickJS `magic` index.
 */

#include "oops/js.h"
#include "oops/system.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wcast-function-type-mismatch"
#endif
#include "quickjs/quickjs.h"
#pragma GCC diagnostic pop

#include <stdlib.h>
#include <string.h>

#define MAX_BINDINGS 256

typedef struct oops_js_fn_binding {
    oops_js_native_fn fn;
    void *userdata;
} oops_js_fn_binding_t;

struct oops_js {
    JSRuntime *rt;
    JSContext *ctx;
    oops_js_fn_binding_t bindings[MAX_BINDINGS];
    int binding_count;
};

static oops_js_value_t js_val_to_oops(JSContext *ctx, JSValueConst v) {
    oops_js_value_t out;
    memset(&out, 0, sizeof(out));

    int tag = JS_VALUE_GET_NORM_TAG(v);
    if (tag == JS_TAG_INT) {
        out.type = OOPS_JS_TYPE_INT;
        out.u.integer = JS_VALUE_GET_INT(v);
    } else if (tag == JS_TAG_BOOL) {
        out.type = OOPS_JS_TYPE_BOOL;
        out.u.boolean = JS_VALUE_GET_BOOL(v);
    } else if (tag == JS_TAG_FLOAT64) {
        out.type = OOPS_JS_TYPE_FLOAT;
        out.u.number = JS_VALUE_GET_FLOAT64(v);
    } else if (tag == JS_TAG_NULL) {
        out.type = OOPS_JS_TYPE_NULL;
    } else if (tag == JS_TAG_UNDEFINED) {
        out.type = OOPS_JS_TYPE_UNDEFINED;
    } else if (tag == JS_TAG_EXCEPTION) {
        out.type = OOPS_JS_TYPE_EXCEPTION;
    } else if (tag == JS_TAG_STRING) {
        out.type = OOPS_JS_TYPE_STRING;
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            out.u.string = strdup(s);
            JS_FreeCString(ctx, s);
        }
    } else if (tag == JS_TAG_OBJECT) {
        if (JS_IsFunction(ctx, v)) {
            out.type = OOPS_JS_TYPE_FUNCTION;
        } else if (JS_IsArray(ctx, v)) {
            out.type = OOPS_JS_TYPE_ARRAY;
        } else {
            out.type = OOPS_JS_TYPE_OBJECT;
        }
        out.u.ptr = JS_VALUE_GET_PTR(v);
    } else {
        out.type = OOPS_JS_TYPE_UNDEFINED;
    }

    return out;
}

static JSValue oops_val_to_js(JSContext *ctx, oops_js_value_t val) {
    switch (val.type) {
    case OOPS_JS_TYPE_INT:
        return JS_NewInt32(ctx, val.u.integer);
    case OOPS_JS_TYPE_BOOL:
        return JS_NewBool(ctx, val.u.boolean);
    case OOPS_JS_TYPE_FLOAT:
        return JS_NewFloat64(ctx, val.u.number);
    case OOPS_JS_TYPE_STRING:
        return val.u.string ? JS_NewString(ctx, val.u.string) : JS_NULL;
    case OOPS_JS_TYPE_NULL:
        return JS_NULL;
    case OOPS_JS_TYPE_UNDEFINED:
    default:
        return JS_UNDEFINED;
    }
}

static JSValue native_thunk(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int magic) {
    (void)this_val;
    /* The oops_js_t is found through the runtime opaque, not the context opaque: an
     * embedder may use the context opaque for its own back-pointer (the webview stores
     * its oops_webview_t there). Each oops_js_t owns its runtime, so that slot is
     * private to it. */
    oops_js_t *js = (oops_js_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    oops_kprintf_level(OOPS_LOG_INFO, "JS", "OOPSYDBG thunk entered magic=%d js=%lx",
                       magic, (unsigned long)(uintptr_t)js);
    if (!js || magic < 0 || magic >= js->binding_count) {
        return JS_UNDEFINED;
    }

    oops_js_fn_binding_t *b = &js->bindings[magic];
    oops_kprintf_level(OOPS_LOG_INFO, "JS", "OOPSYDBG thunk count=%d fn=%lx ud=%lx",
                       js->binding_count, (unsigned long)(uintptr_t)b->fn,
                       (unsigned long)(uintptr_t)b->userdata);
    oops_js_value_t *arg_vals = NULL;
    if (argc > 0) {
        arg_vals = (oops_js_value_t *)malloc(sizeof(oops_js_value_t) * (size_t)argc);
        for (int i = 0; i < argc; i++) {
            arg_vals[i] = js_val_to_oops(ctx, argv[i]);
        }
    }

    oops_js_value_t ret = b->fn(js, argc, arg_vals, b->userdata);

    if (arg_vals) {
        for (int i = 0; i < argc; i++) {
            oops_js_free_value(js, &arg_vals[i]);
        }
        free(arg_vals);
    }

    JSValue out = oops_val_to_js(ctx, ret);
    oops_js_free_value(js, &ret);
    return out;
}

oops_js_t *oops_js_create(void) {
    oops_js_t *js = (oops_js_t *)calloc(1, sizeof(*js));
    if (!js) {
        oops_log_error("JS", "Failed to allocate oops_js_t instance");
        return NULL;
    }

    js->rt = JS_NewRuntime();
    if (!js->rt) {
        oops_log_error("JS", "Failed to create QuickJS runtime");
        free(js);
        return NULL;
    }

    js->ctx = JS_NewContext(js->rt);
    if (!js->ctx) {
        oops_log_error("JS", "Failed to create QuickJS context");
        JS_FreeRuntime(js->rt);
        free(js);
        return NULL;
    }

    /* The runtime opaque is the back-pointer native_thunk reads. The context opaque is
     * set too, for callers that expect it, but an embedder may replace it, so nothing
     * internal relies on it. */
    JS_SetRuntimeOpaque(js->rt, js);
    JS_SetContextOpaque(js->ctx, js);
    oops_log_debug("JS", "QuickJS runtime and context initialized");
    return js;
}

void oops_js_destroy(oops_js_t *js) {
    if (!js)
        return;

    if (js->ctx) {
        JS_FreeContext(js->ctx);
        js->ctx = NULL;
    }
    if (js->rt) {
        JS_FreeRuntime(js->rt);
        js->rt = NULL;
    }
    free(js);
    oops_log_debug("JS", "QuickJS context destroyed");
}

int oops_js_eval(oops_js_t *js, const char *src, const char *name,
                 oops_js_value_t *out) {
    if (!js || !js->ctx || !src) {
        oops_log_error("JS", "eval called with invalid parameters");
        return -1;
    }

    if (!name)
        name = "<eval>";

    oops_log_debug("JS", "Evaluating script: %s", name);
    JSValue result = JS_Eval(js->ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        JSValue exception_val = JS_GetException(js->ctx);
        const char *err = JS_ToCString(js->ctx, exception_val);
        oops_log_error("JS", "Eval exception in '%s': %s", name,
                       err ? err : "unknown error");
        if (err)
            JS_FreeCString(js->ctx, err);
        JSValue stk = JS_GetPropertyStr(js->ctx, exception_val, "stack");
        if (!JS_IsUndefined(stk)) {
            const char *s = JS_ToCString(js->ctx, stk);
            if (s) {
                oops_log_error("JS", "  stack: %s", s);
                JS_FreeCString(js->ctx, s);
            }
        }
        JS_FreeValue(js->ctx, stk);
        JS_FreeValue(js->ctx, exception_val);
        JS_FreeValue(js->ctx, result);
        return -1;
    }

    if (out) {
        *out = js_val_to_oops(js->ctx, result);
    }
    JS_FreeValue(js->ctx, result);
    return 0;
}

int oops_js_register_fn(oops_js_t *js, const char *name, oops_js_native_fn fn,
                        void *userdata) {
    if (!js || !js->ctx || !name || !fn)
        return -1;
    if (js->binding_count >= MAX_BINDINGS) {
        oops_log_error("JS", "Cannot register function '%s': max bindings reached",
                       name);
        return -1;
    }

    int id = js->binding_count++;
    js->bindings[id].fn = fn;
    js->bindings[id].userdata = userdata;

    JSValue global_obj = JS_GetGlobalObject(js->ctx);
    JSValue func_obj = JS_NewCFunctionMagic(js->ctx, (JSCFunctionMagic *)native_thunk,
                                            name, 0, JS_CFUNC_generic_magic, id);
    oops_kprintf_level(OOPS_LOG_INFO, "JS", "OOPSYDBG reg '%s' id=%d thunk=%lx obj=%lx",
                       name, id, (unsigned long)(uintptr_t)&native_thunk,
                       (unsigned long)(uintptr_t)JS_VALUE_GET_PTR(func_obj));
    JS_SetPropertyStr(js->ctx, global_obj, name, func_obj);
    JS_FreeValue(js->ctx, global_obj);

    oops_log_debug("JS", "Registered native function '%s' (binding id %d)", name, id);
    return 0;
}

int oops_js_set_global(oops_js_t *js, const char *name, oops_js_value_t val) {
    if (!js || !js->ctx || !name)
        return -1;

    JSValue global_obj = JS_GetGlobalObject(js->ctx);
    JSValue jv = oops_val_to_js(js->ctx, val);
    int ret = JS_SetPropertyStr(js->ctx, global_obj, name, jv);
    JS_FreeValue(js->ctx, global_obj);
    return ret >= 0 ? 0 : -1;
}

oops_js_value_t oops_js_get_global(oops_js_t *js, const char *name) {
    if (!js || !js->ctx || !name)
        return oops_js_make_undefined();

    JSValue global_obj = JS_GetGlobalObject(js->ctx);
    JSValue jv = JS_GetPropertyStr(js->ctx, global_obj, name);
    JS_FreeValue(js->ctx, global_obj);

    oops_js_value_t out = js_val_to_oops(js->ctx, jv);
    JS_FreeValue(js->ctx, jv);
    return out;
}

int oops_js_execute_pending_jobs(oops_js_t *js) {
    if (!js || !js->rt)
        return 0;

    int count = 0;
    JSContext *pctx;
    while (JS_IsJobPending(js->rt)) {
        int err = JS_ExecutePendingJob(js->rt, &pctx);
        if (err < 0) {
            oops_log_error("JS", "Error executing pending microtask job");
            return -1;
        }
        count++;
    }
    return count;
}

oops_js_value_t oops_js_make_undefined(void) {
    oops_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = OOPS_JS_TYPE_UNDEFINED;
    return v;
}

oops_js_value_t oops_js_make_null(void) {
    oops_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = OOPS_JS_TYPE_NULL;
    return v;
}

oops_js_value_t oops_js_make_bool(int val) {
    oops_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = OOPS_JS_TYPE_BOOL;
    v.u.boolean = val ? 1 : 0;
    return v;
}

oops_js_value_t oops_js_make_int(int val) {
    oops_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = OOPS_JS_TYPE_INT;
    v.u.integer = val;
    return v;
}

oops_js_value_t oops_js_make_number(double val) {
    oops_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = OOPS_JS_TYPE_FLOAT;
    v.u.number = val;
    return v;
}

oops_js_value_t oops_js_make_string(oops_js_t *js, const char *str) {
    (void)js;
    oops_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = OOPS_JS_TYPE_STRING;
    v.u.string = str ? strdup(str) : NULL;
    return v;
}

void oops_js_free_value(oops_js_t *js, oops_js_value_t *val) {
    (void)js;
    if (!val)
        return;
    if (val->type == OOPS_JS_TYPE_STRING && val->u.string) {
        free(val->u.string);
        val->u.string = NULL;
    }
}

void *oops_js_get_context(oops_js_t *js) {
    return js ? (void *)js->ctx : NULL;
}

void *oops_js_get_runtime(oops_js_t *js) {
    return js ? (void *)js->rt : NULL;
}
