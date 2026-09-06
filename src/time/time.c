#include "oops/time.h"

__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);
__attribute__((weak)) uint64_t sceKernelGetProcessTime(void);
__attribute__((weak)) uint64_t sceKernelGetProcessTimeCounter(void);
__attribute__((weak)) uint64_t sceKernelGetProcessTimeCounterFrequency(void);
__attribute__((weak)) uint64_t sceKernelGetTscFrequency(void);

static uint64_t s_tsc_frequency = 0;
static uint64_t s_counter_frequency = 0;
static int s_time_initialized = 0;

uint64_t oops_time_get_ticks(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t oops_time_get_counter(void) {
    if (sceKernelGetProcessTimeCounter) {
        return sceKernelGetProcessTimeCounter();
    }
    return oops_time_get_ticks();
}

uint64_t oops_time_get_counter_frequency(void) {
    if (s_counter_frequency != 0) return s_counter_frequency;
    if (sceKernelGetProcessTimeCounterFrequency) {
        s_counter_frequency = sceKernelGetProcessTimeCounterFrequency();
    }
    if (s_counter_frequency == 0) {
        s_counter_frequency = 1596283790ULL; /* Standard hardware frequency */
    }
    return s_counter_frequency;
}

void oops_time_init(void) {
    if (s_time_initialized) return;

    if (sceKernelGetProcessTimeCounterFrequency) {
        s_counter_frequency = sceKernelGetProcessTimeCounterFrequency();
    }

    if (sceKernelGetTscFrequency) {
        s_tsc_frequency = sceKernelGetTscFrequency();
    }

    /* If TSC frequency is unknown, calibrate against sceKernelGetProcessTime() */
    if (s_tsc_frequency == 0 && sceKernelGetProcessTime) {
        uint64_t t0 = sceKernelGetProcessTime();
        uint64_t c0 = oops_time_get_ticks();
        /* Spin until at least 5000 microseconds (5 ms) elapsed */
        uint64_t t1 = t0;
        while ((t1 = sceKernelGetProcessTime()) - t0 < 5000) {
            __asm__ __volatile__("pause");
        }
        uint64_t c1 = oops_time_get_ticks();
        uint64_t dt_us = t1 - t0;
        uint64_t dt_cycles = c1 - c0;
        if (dt_us > 0) {
            s_tsc_frequency = (dt_cycles * 1000000ULL) / dt_us;
        }
    }

    /* Fallback default to 3.2 GHz (Zen 2 base) */
    if (s_tsc_frequency == 0) {
        s_tsc_frequency = 3200000000ULL;
    }

    s_time_initialized = 1;
}

uint64_t oops_time_get_frequency(void) {
    if (!s_time_initialized) oops_time_init();
    return s_tsc_frequency;
}

uint64_t oops_time_get_us(void) {
    if (sceKernelGetProcessTime) {
        return sceKernelGetProcessTime();
    }
    uint64_t ticks = oops_time_get_ticks();
    uint64_t freq = oops_time_get_frequency();
    if (freq == 0) return 0;
    return (uint64_t)(((unsigned __int128)ticks * 1000000ULL) / freq);
}

uint64_t oops_time_get_ns(void) {
    uint64_t ticks = oops_time_get_ticks();
    uint64_t freq = oops_time_get_frequency();
    if (freq == 0) return 0;
    return (uint64_t)(((unsigned __int128)ticks * 1000000000ULL) / freq);
}

uint64_t oops_time_get_ms(void) {
    return oops_time_get_us() / 1000ULL;
}

double oops_time_get_seconds(void) {
    return (double)oops_time_get_us() / 1000000.0;
}

void oops_time_sleep_us(uint32_t microseconds) {
    if (sceKernelUsleep) {
        (void)sceKernelUsleep(microseconds);
    } else {
        uint64_t start = oops_time_get_us();
        while (oops_time_get_us() - start < microseconds) {
            __asm__ __volatile__("pause");
        }
    }
}

void oops_time_sleep_ms(uint32_t milliseconds) {
    oops_time_sleep_us(milliseconds * 1000u);
}
