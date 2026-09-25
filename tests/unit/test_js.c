#include "oops/js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static oops_js_value_t native_add(oops_js_t *js, int argc, oops_js_value_t *argv, void *userdata) {
    intptr_t offset = (intptr_t)userdata;
    int sum = (int)offset;
    for (int i = 0; i < argc; i++) {
        if (argv[i].type == OOPS_JS_TYPE_INT) {
            sum += argv[i].u.integer;
        } else if (argv[i].type == OOPS_JS_TYPE_FLOAT) {
            sum += (int)argv[i].u.number;
        }
    }
    return oops_js_make_int(sum);
}

static oops_js_value_t native_concat(oops_js_t *js, int argc, oops_js_value_t *argv, void *userdata) {
    (void)userdata;
    char buf[256] = {0};
    for (int i = 0; i < argc; i++) {
        if (argv[i].type == OOPS_JS_TYPE_STRING && argv[i].u.string) {
            strncat(buf, argv[i].u.string, sizeof(buf) - strlen(buf) - 1);
        }
    }
    return oops_js_make_string(js, buf);
}

void test_js_lifecycle(void) {
    oops_js_t *js = oops_js_create();
    assert(js != NULL);
    oops_js_destroy(js);
    printf("PASS: test_js_lifecycle\n");
}

void test_js_eval_arithmetic(void) {
    oops_js_t *js = oops_js_create();
    assert(js != NULL);

    oops_js_value_t res;
    int rc = oops_js_eval(js, "40 + 2", "test.js", &res);
    assert(rc == 0);
    assert(res.type == OOPS_JS_TYPE_INT);
    assert(res.u.integer == 42);
    oops_js_free_value(js, &res);

    rc = oops_js_eval(js, "3.14 * 2.0", "test.js", &res);
    assert(rc == 0);
    assert(res.type == OOPS_JS_TYPE_FLOAT);
    assert(res.u.number > 6.27 && res.u.number < 6.29);
    oops_js_free_value(js, &res);

    oops_js_destroy(js);
    printf("PASS: test_js_eval_arithmetic\n");
}

void test_js_native_binding(void) {
    oops_js_t *js = oops_js_create();
    assert(js != NULL);

    int rc = oops_js_register_fn(js, "add_with_offset", native_add, (void *)(intptr_t)10);
    assert(rc == 0);

    oops_js_value_t res;
    rc = oops_js_eval(js, "add_with_offset(5, 7)", "test.js", &res);
    assert(rc == 0);
    assert(res.type == OOPS_JS_TYPE_INT);
    assert(res.u.integer == 22); /* 10 + 5 + 7 */
    oops_js_free_value(js, &res);

    rc = oops_js_register_fn(js, "concat_strings", native_concat, NULL);
    assert(rc == 0);

    rc = oops_js_eval(js, "concat_strings('Hello, ', 'OOPS ', 'Web!')", "test.js", &res);
    assert(rc == 0);
    assert(res.type == OOPS_JS_TYPE_STRING);
    assert(strcmp(res.u.string, "Hello, OOPS Web!") == 0);
    oops_js_free_value(js, &res);

    oops_js_destroy(js);
    printf("PASS: test_js_native_binding\n");
}

void test_js_globals_and_microtasks(void) {
    oops_js_t *js = oops_js_create();
    assert(js != NULL);

    oops_js_value_t val = oops_js_make_int(1337);
    int rc = oops_js_set_global(js, "myConfigValue", val);
    assert(rc == 0);

    oops_js_value_t out = oops_js_get_global(js, "myConfigValue");
    assert(out.type == OOPS_JS_TYPE_INT);
    assert(out.u.integer == 1337);
    oops_js_free_value(js, &out);

    /* Promise test */
    rc = oops_js_eval(js, "var resolved = 0; Promise.resolve(99).then(v => { resolved = v; });", "promise.js", NULL);
    assert(rc == 0);

    /* Microtask not run yet */
    out = oops_js_get_global(js, "resolved");
    assert(out.u.integer == 0);
    oops_js_free_value(js, &out);

    /* Drain jobs */
    int jobs = oops_js_execute_pending_jobs(js);
    assert(jobs > 0);

    out = oops_js_get_global(js, "resolved");
    assert(out.u.integer == 99);
    oops_js_free_value(js, &out);

    oops_js_destroy(js);
    printf("PASS: test_js_globals_and_microtasks\n");
}

void test_js_syntax_error(void) {
    oops_js_t *js = oops_js_create();
    assert(js != NULL);

    int rc = oops_js_eval(js, "this is definitely not valid JS {[[", "bad.js", NULL);
    assert(rc == -1);

    oops_js_destroy(js);
    printf("PASS: test_js_syntax_error\n");
}

int main(void) {
    printf("=== RUNNING JS UNIT TESTS ===\n");
    test_js_lifecycle();
    test_js_eval_arithmetic();
    test_js_native_binding();
    test_js_globals_and_microtasks();
    test_js_syntax_error();
    printf("ALL JS UNIT TESTS PASSED!\n");
    return 0;
}
