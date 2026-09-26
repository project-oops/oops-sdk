/*
 * JavaScript through QuickJS: contexts, evaluation, native functions, globals, and a
 * plain C value type.
 */
#ifndef OOPS_JS_H
#define OOPS_JS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_js oops_js_t;

typedef enum oops_js_type {
    OOPS_JS_TYPE_UNDEFINED = 0,
    OOPS_JS_TYPE_NULL,
    OOPS_JS_TYPE_BOOL,
    OOPS_JS_TYPE_INT,
    OOPS_JS_TYPE_FLOAT,
    OOPS_JS_TYPE_STRING,
    OOPS_JS_TYPE_OBJECT,
    OOPS_JS_TYPE_ARRAY,
    OOPS_JS_TYPE_FUNCTION,
    OOPS_JS_TYPE_EXCEPTION
} oops_js_type_t;

typedef struct oops_js_value {
    oops_js_type_t type;
    union {
        int boolean;
        int integer;
        double number;
        char *string; /* Dynamically allocated, freed by oops_js_free_value */
        void *ptr;
    } u;
    uint64_t _raw; /* Internal JSValue tag & payload */
} oops_js_value_t;

typedef oops_js_value_t (*oops_js_native_fn)(oops_js_t *js, int argc,
                                             oops_js_value_t *argv, void *userdata);

/*
 * Create an isolated JavaScript execution context backed by QuickJS.
 * Returns NULL on allocation failure.
 */
oops_js_t *oops_js_create(void);

/*
 * Destroy a JavaScript context, freeing all associated memory, objects,
 * and pending jobs.
 */
void oops_js_destroy(oops_js_t *js);

/*
 * Evaluate a JavaScript script string.
 * `name` is the filename displayed in stack traces and logs (e.g. "main.js" or
 * "<eval>"). If `out` is non-NULL, it receives the evaluated value. Caller must free
 * with oops_js_free_value. Returns 0 on success, or -1 if an exception was thrown.
 */
int oops_js_eval(oops_js_t *js, const char *src, const char *name,
                 oops_js_value_t *out);

/*
 * Register a native C function into the global scope.
 */
int oops_js_register_fn(oops_js_t *js, const char *name, oops_js_native_fn fn,
                        void *userdata);

/*
 * Set a named property on the global object.
 */
int oops_js_set_global(oops_js_t *js, const char *name, oops_js_value_t val);

/*
 * Get a named property from the global object.
 * Caller must free returned value with oops_js_free_value.
 */
oops_js_value_t oops_js_get_global(oops_js_t *js, const char *name);

/*
 * Execute pending asynchronous microtasks and Promise jobs.
 * Returns the number of executed jobs, or negative on error.
 */
int oops_js_execute_pending_jobs(oops_js_t *js);

/* Value constructors */
oops_js_value_t oops_js_make_undefined(void);
oops_js_value_t oops_js_make_null(void);
oops_js_value_t oops_js_make_bool(int val);
oops_js_value_t oops_js_make_int(int val);
oops_js_value_t oops_js_make_number(double val);
oops_js_value_t oops_js_make_string(oops_js_t *js, const char *str);

/* Free any resources associated with a value */
void oops_js_free_value(oops_js_t *js, oops_js_value_t *val);

/* Advanced: retrieve underlying JSContext* and JSRuntime* pointers */
void *oops_js_get_context(oops_js_t *js);
void *oops_js_get_runtime(oops_js_t *js);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_JS_H */
