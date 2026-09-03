#include "oops/time.h"

__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);
__attribute__((weak)) int sceKernelGetProcessTimeCounter(void);
__attribute__((weak)) uint64_t sceKernelGetTscFrequency(void);

uint64_t oops_time_get_ticks(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t oops_time_get_frequency(void) {
    if (sceKernelGetTscFrequency) {
        return sceKernelGetTscFrequency();
    }
    return 3200000000ULL; /* Typical 3.2 GHz PS5 AMD Zen 2 CPU clock */
}

uint64_t oops_time_get_us(void) {
    uint64_t ticks = oops_time_get_ticks();
    uint64_t freq = oops_time_get_frequency();
    if (freq == 0) return 0;
    return (ticks * 1000000ULL) / freq;
}

uint64_t oops_time_get_ms(void) {
    return oops_time_get_us() / 1000ULL;
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
