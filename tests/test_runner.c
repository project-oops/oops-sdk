/* The host test runner: runs every unit and integration suite and prints a summary of
 * all failures. */
#include "tests/test_common.h"

#include <stdarg.h>

int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;

jmp_buf g_test_abort;
int g_test_abort_ready = 0;

/*
 * Every failure is printed where it happens and also kept here, so the summary names
 * all of them in one place (see `tests/test_common.h`). The store is fixed and lives in
 * `.bss`, so a failing run does not depend on the allocator. Past its end failures are
 * counted rather than kept, and the summary says so.
 */
#define OOPS_TEST_MAX_FAILURES 256

typedef struct {
    char suite[64];
    char test[64];
    char file[128];
    int line;
    char msg[192];
} oops_test_failure_t;

static oops_test_failure_t s_failures[OOPS_TEST_MAX_FAILURES];
static int s_failure_count = 0;
static int s_failures_dropped = 0;
static char s_suite[64] = "(no suite)";
static char s_test[64] = "(no test)";

static void oops_test_copy(char *dst, size_t cap, const char *src) {
    if (src == NULL) {
        src = "(null)";
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void oops_test_suite_begin(const char *suite) {
    oops_test_copy(s_suite, sizeof(s_suite), suite);
    printf("\n=== [SUITE: %s] ===\n", suite);
}

void oops_test_begin(const char *test) {
    oops_test_copy(s_test, sizeof(s_test), test);
}

void oops_test_fail(const char *file, int line, const char *fmt, ...) {
    char msg[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    g_tests_failed++;
    printf("\033[31mFAIL\033[0m (%s:%d: %s)\n", file, line, msg);
    fflush(stdout);

    if (s_failure_count < OOPS_TEST_MAX_FAILURES) {
        oops_test_failure_t *f = &s_failures[s_failure_count++];
        oops_test_copy(f->suite, sizeof(f->suite), s_suite);
        oops_test_copy(f->test, sizeof(f->test), s_test);
        oops_test_copy(f->file, sizeof(f->file), file);
        f->line = line;
        oops_test_copy(f->msg, sizeof(f->msg), msg);
    } else {
        s_failures_dropped++;
    }

    if (g_test_abort_ready) {
        g_test_abort_ready = 0;
        longjmp(g_test_abort, 1);
    }

    /* An assertion outside any RUN_TEST - fixture setup, or a helper called from main -
     * has no test to abandon and nowhere to unwind to, so it ends the run. */
    printf("(that assertion was outside a test: stopping)\n");
    fflush(stdout);
    exit(1);
}

int oops_test_report(void) {
    if (s_failure_count > 0) {
        printf("\n\033[31m=== FAILURES ===\033[0m\n");
        for (int i = 0; i < s_failure_count; i++) {
            const oops_test_failure_t *f = &s_failures[i];
            printf("  %2d. [%s] %s\n", i + 1, f->suite, f->test);
            printf("      %s:%d: %s\n", f->file, f->line, f->msg);
        }
        if (s_failures_dropped > 0) {
            printf("  ... and %d more, past the %d this runner keeps\n",
                   s_failures_dropped, OOPS_TEST_MAX_FAILURES);
        }
    }

    printf("\n=======================================================\n");
    printf(" TEST SUMMARY: %d Ran | \033[32m%d Passed\033[0m | \033[%sm%d "
           "Failed\033[0m\n",
           g_tests_run, g_tests_passed, g_tests_failed > 0 ? "31" : "32",
           g_tests_failed);
    printf("=======================================================\n\n");

    return (g_tests_failed == 0) ? 0 : 1;
}

#include "oops/display.h"

static uint32_t s_host_fb[1920 * 1080];
static int s_host_disp_dummy = 1;
static unsigned int s_host_w = 1920;
static unsigned int s_host_h = 1080;

__attribute__((weak)) oops_display_t *oops_display_open(oops_display_backend_t backend,
                                                        unsigned int width,
                                                        unsigned int height) {
    (void)backend;
    s_host_w = width ? width : 1920;
    s_host_h = height ? height : 1080;
    return (oops_display_t *)&s_host_disp_dummy;
}

__attribute__((weak)) void oops_display_close(oops_display_t *disp) {
    (void)disp;
}

__attribute__((weak)) int oops_display_flip(oops_display_t *disp) {
    (void)disp;
    return 0;
}

__attribute__((weak)) uint32_t *oops_display_get_framebuffer(oops_display_t *disp) {
    (void)disp;
    return s_host_fb;
}
__attribute__((weak)) unsigned int oops_display_get_width(const oops_display_t *disp) {
    (void)disp;
    return s_host_w;
}
__attribute__((weak)) unsigned int oops_display_get_height(const oops_display_t *disp) {
    (void)disp;
    return s_host_h;
}
__attribute__((weak)) int oops_display_is_gpu_accelerated(const oops_display_t *disp) {
    (void)disp;
    return 0;
}
#include "agc/display.h"
__attribute__((weak)) int agc_display_is_gpu_accelerated(const agc_display_t *disp) {
    (void)disp;
    return 0;
}

/* Declarations of unit test suites */
void run_unit_tests_agc_tiler(void);
void run_unit_tests_draw(void);
void run_unit_tests_input(void);
void run_unit_tests_audio(void);
void run_unit_tests_videodec(void);
void run_unit_tests_audiodec(void);
void run_unit_tests_memory(void);
void run_unit_tests_system(void);
void run_unit_tests_offsets(void);
void run_unit_tests_sysmodule(void);
void run_unit_tests_dialog(void);
void run_unit_tests_netctl(void);
void run_unit_tests_savedata(void);
void run_unit_tests_escalate(void);
void run_unit_tests_pkg(void);
void run_unit_tests_time(void);
void run_unit_tests_thread(void);
void run_unit_tests_net(void);
void run_unit_tests_freestd(void);
void run_unit_tests_krw(void);
void run_unit_tests_inject(void);
void run_unit_tests_gpu(void);
void run_unit_tests_pm4(void);
void run_unit_tests_gl(void);
void run_unit_tests_gl2(void);
void run_unit_tests_jit(void);
void run_unit_tests_fs(void);
void run_unit_tests_heap(void);
void run_unit_tests_math(void);
void run_unit_tests_dns(void);
void run_unit_tests_zip(void);
void run_unit_tests_http(void);

/* Declarations of integration test suites */
void run_integration_tests_pipeline(void);
void run_integration_tests_memory(void);
void run_integration_tests_thread_pool(void);
void run_integration_tests_net_loopback(void);

int main(int argc, char **argv) {
    bool run_unit = true;
    bool run_int = true;

    if (argc > 1) {
        if (strcmp(argv[1], "unit") == 0) {
            run_int = false;
        } else if (strcmp(argv[1], "int") == 0) {
            run_unit = false;
        }
    }

    printf("\n=======================================================\n");
    printf("         OOPS-SDK COMPREHENSIVE TEST SUITE             \n");
    printf("=======================================================\n");

    if (run_unit) {
        printf("\n>>> RUNNING UNIT TESTS <<<\n");
        run_unit_tests_agc_tiler();
        run_unit_tests_draw();
        run_unit_tests_input();
        run_unit_tests_audio();
        run_unit_tests_videodec();
        run_unit_tests_audiodec();
        run_unit_tests_memory();
        run_unit_tests_system();
        run_unit_tests_offsets();
        run_unit_tests_sysmodule();
        run_unit_tests_dialog();
        run_unit_tests_netctl();
        run_unit_tests_savedata();
        run_unit_tests_escalate();
        run_unit_tests_pkg();
        run_unit_tests_time();
        run_unit_tests_thread();
        run_unit_tests_net();
        run_unit_tests_freestd();
        run_unit_tests_krw();
        run_unit_tests_inject();
        run_unit_tests_gpu();
        run_unit_tests_pm4();
        run_unit_tests_gl();
        run_unit_tests_gl2();
        run_unit_tests_jit();
        run_unit_tests_fs();
        run_unit_tests_heap();
        run_unit_tests_math();
        run_unit_tests_dns();
        run_unit_tests_zip();
        run_unit_tests_http();
    }

    if (run_int) {
        printf("\n>>> RUNNING INTEGRATION TESTS <<<\n");
        run_integration_tests_pipeline();
        run_integration_tests_memory();
        run_integration_tests_thread_pool();
        run_integration_tests_net_loopback();
    }

    return oops_test_report();
}
