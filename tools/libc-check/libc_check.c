/*
 * libc-check: every name the headers under `include/libc` declare, called once, so that the link
 * says whether it is there.
 *
 * A payload link passes `--unresolved-symbols=ignore-all`, because the platform's own modules
 * resolve `sce*` imports at load. That means a missing `sqrtf` does not fail the build: it links
 * to nothing and faults on the console, which is the most expensive place to find out. The only
 * way to know before then is to look at the symbol table, and the only way to know that a *name*
 * is missing is to call it.
 *
 * So this calls all of them and `build.sh` fails if the linked object leaves any of them
 * undefined. It is not a test of what they compute - `oops-sdk`'s own tests cover the functions
 * underneath - it is a test that a port calling them finds them.
 */
#include "libc/assert.h"
#include "libc/ctype.h"
#include "libc/math.h"
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/time.h"

/* Volatile so the compiler cannot fold the calls away and leave nothing to resolve, which would
 * make this pass by doing nothing. */
volatile float g_f;
volatile double g_d;
volatile int g_i;
volatile size_t g_z;
volatile void *g_p;
volatile char *g_s;

int libc_check_touch(void);

static int libc_check_cmp_int(const void *a, const void *b) {
    const int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int libc_check_touch(void) {
    static char buf[64];
    static const char text[] = "42.5e1 abc";
    volatile float x = 0.5f;
    volatile double y = 0.5;

    g_f = sqrtf(x); g_f = fabsf(x); g_f = floorf(x); g_f = ceilf(x);
    g_f = fmodf(x, x); g_f = sinf(x); g_f = cosf(x); g_f = tanf(x);
    g_f = asinf(x); g_f = acosf(x); g_f = atanf(x); g_f = atan2f(x, x);
    g_f = expf(x); g_f = logf(x); g_f = log2f(x); g_f = log10f(x);
    g_f = powf(x, x); g_f = hypotf(x, x); g_f = roundf(x); g_f = truncf(x);
    g_f = fminf(x, x); g_f = fmaxf(x, x);

    g_d = sqrt(y); g_d = fabs(y); g_d = floor(y); g_d = ceil(y);
    g_d = fmod(y, y); g_d = sin(y); g_d = cos(y); g_d = tan(y);
    g_d = asin(y); g_d = acos(y); g_d = atan(y); g_d = atan2(y, y);
    g_d = exp(y); g_d = log(y); g_d = log10(y); g_d = pow(y, y);
    g_d = hypot(y, y); g_d = round(y); g_d = trunc(y);
    g_d = fmin(y, y); g_d = fmax(y, y); g_d = log2(y);

    g_f = copysignf(x, -x); g_d = copysign(y, -y);
    g_f = ldexpf(x, 2); g_d = ldexp(y, 2);
    {
        float fi = 0.0f;
        double di = 0.0;
        int e = 0;
        g_f = modff(x, &fi); g_d = modf(y, &di);
        g_f = frexpf(x, &e); g_d = frexp(y, &e);
        g_i = e;
    }
    /* The classification macros are the compiler's builtins, so they resolve to no symbol at
     * all - they are here because a port writes them and they have to compile. */
    g_i = isnan(x) + isinf(x) + isfinite(x) + signbit(x) + isnormal(x);

    g_p = memset(buf, 0, sizeof(buf));
    g_p = memcpy(buf, text, 4);
    g_p = memmove(buf, text, 4);
    g_i = memcmp(buf, text, 4);
    g_p = memchr(buf, 'a', sizeof(buf));

    g_z = strlen(text);
    g_z = strnlen(text, 4);
    g_i = strcmp(text, buf);
    g_i = strncmp(text, buf, 3);
    g_s = strcpy(buf, text);
    g_s = strncpy(buf, text, 8);
    buf[0] = '\0';
    g_s = strcat(buf, text);
    buf[0] = '\0';
    g_s = strncat(buf, text, 3);
    g_s = strchr(text, 'a');
    g_s = strrchr(text, 'a');
    g_s = strstr(text, "abc");
    g_z = strspn(text, "0123456789");
    g_z = strcspn(text, "abc");
    g_s = strpbrk(text, "abc");
    g_s = strerror(1);
    {
        /* `strdup` allocates and `strtok` writes into its argument, so both work on a copy
         * rather than on the literal. */
        char *dup = strdup(text);
        g_s = dup;
        if (dup) {
            char *save = (char *)0;
            g_s = strtok_r(dup, " ", &save);
            free(dup);
        }
        (void)memcpy(buf, text, sizeof(text));
        g_s = strtok(buf, " ");
        g_s = strtok((char *)0, " ");
    }

    g_p = malloc(16);
    g_p = calloc(2, 8);
    g_p = realloc((void *)(size_t)g_p, 32);
    g_p = aligned_alloc(16, 32);
    g_z = MB_CUR_MAX;
    free((void *)(size_t)g_p);

    g_i = abs(-1);
    g_i = (int)labs(-1L);
    g_i = atoi(text);
    g_i = (int)atol(text);
    g_d = atof(text);
    g_i = (int)strtol(text, (char **)0, 10);
    g_d = strtod(text, (char **)0);
    g_i = (int)strtoul(text, (char **)0, 10);
    g_f = strtof(text, (char **)0);
    g_i = (int)llabs(-1LL);
    {
        const div_t d = div(7, 2);
        const ldiv_t l = ldiv(7L, 2L);
        g_i = d.quot + d.rem + (int)(l.quot + l.rem);
    }
    {
        /* qsort and bsearch, over a small array, with a comparison that is itself a call - so a
         * missing name in either is a missing symbol here. */
        static int nums[4] = {3, 1, 4, 1};
        static const int key = 3;
        qsort(nums, 4, sizeof(nums[0]), libc_check_cmp_int);
        g_p = bsearch(&key, nums, 4, sizeof(nums[0]), libc_check_cmp_int);
    }
    {
        /* sscanf and vsscanf, over a string that converts - the names are what is being
         * checked, but a conversion that runs is a better check than one that fails first. */
        int n = 0;
        float f = 0.0f;
        g_i = sscanf("7 1.5", "%d %f", &n, &f);
        g_i += n;
        g_f = f;
    }
    g_i = rand();
    srand(1u);

    /* stdio. The file half is called against a path that will not open, which is the point: a
     * name that is missing is missing whether the call succeeds or not. */
    FILE *f = fopen("/nonexistent/libc-check", "rb");
    g_z = fread(buf, 1u, sizeof(buf), f);
    g_z = fwrite(buf, 1u, 1u, f);
    g_i = fseek(f, 0L, SEEK_SET);
    g_i = (int)ftell(f);
    rewind(f);
    g_i = feof(f);
    g_i = ferror(f);
    g_i = fgetc(f);
    g_i = getc(f);
    g_s = fgets(buf, (int)sizeof(buf), f);
    g_i = fclose(f);
    g_i = remove("/nonexistent/libc-check");

    g_i = fputc('x', stdout);
    g_i = fputs("", stdout);
    g_i = puts("");
    g_i = fflush(stdout);
    g_i = printf("%s", "");
    g_i = fprintf(stderr, "%s", "");
    g_i = sprintf(buf, "%d", 1);
    g_i = snprintf(buf, sizeof(buf), "%d", 1);
    g_p = (void *)(size_t)&vprintf;
    g_p = (void *)(size_t)&vfprintf;
    g_p = (void *)(size_t)&vsnprintf;
    g_p = (void *)(size_t)&vsprintf;
    g_p = (void *)(size_t)&vsscanf;
    g_p = (void *)(size_t)stdin;

    /* ctype is inline, so it is compiled rather than linked - a mistake there is a compile
     * error, which is the failure this whole tool exists to turn the other one into. */
    g_i = isdigit('1') + isalpha('a') + isalnum('a') + isspace(' ') + isupper('A') +
          islower('a') + isxdigit('f') + isblank(' ') + isprint('a') + isgraph('a') +
          iscntrl('\n') + ispunct('.') + tolower('A') + toupper('a');

    {
        time_t t = 0;
        g_i = (int)time(&t);
        g_i = (int)clock();
    }

    /* assert's helper, which the macro calls and which therefore has to link. */
    g_p = (void *)(size_t)&oops_assert_failed;

    /* exit and abort do not return, so they are named rather than called - the linker resolves a
     * taken address exactly as it resolves a call, which is what this is checking. */
    g_p = (void *)(size_t)&exit;
    g_p = (void *)(size_t)&abort;
    return g_i;
}
