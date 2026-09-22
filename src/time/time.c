#include "oops/time.h"
#include "oops/syscall.h"

__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);
__attribute__((weak)) uint64_t sceKernelGetProcessTime(void);
__attribute__((weak)) uint64_t sceKernelGetProcessTimeCounter(void);
__attribute__((weak)) uint64_t sceKernelGetProcessTimeCounterFrequency(void);
__attribute__((weak)) uint64_t sceKernelGetTscFrequency(void);

/*
 * One origin for every reading. On hardware the microsecond and nanosecond
 * clocks both derive from the process clock - the microsecond call and its
 * high-resolution counter share a base - so a nanosecond reading divided by
 * 1000 agrees with a microsecond one. Off hardware, with no platform call
 * resolved, both fall back to the TSC and a calibrated or assumed frequency,
 * which keeps that agreement. Mixing the two must never produce a delta from
 * two origins.
 */
static uint64_t s_tsc_frequency = 0;
static uint64_t s_counter_frequency = 0;
static int s_time_initialized = 0;

/* ticks * per_second / freq without overflowing the intermediate. */
static uint64_t scale(uint64_t ticks, uint64_t per_second, uint64_t freq) {
  if (freq == 0)
    return 0;
#if defined(__x86_64__) || defined(_M_X64)
  uint64_t quot, rem;
  __asm__("mulq %[per_sec]\n\t"
          "divq %[divisor]"
          : "=a"(quot), "=&d"(rem)
          : "a"(ticks), [per_sec] "r"(per_second), [divisor] "r"(freq)
          : "cc");
  return quot;
#else
  return (uint64_t)(((unsigned __int128)ticks * per_second) / freq);
#endif
}

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
  if (s_counter_frequency != 0)
    return s_counter_frequency;
  if (sceKernelGetProcessTimeCounterFrequency) {
    s_counter_frequency = sceKernelGetProcessTimeCounterFrequency();
  }
  if (s_counter_frequency == 0) {
    s_counter_frequency = 1596283790ULL; /* Standard hardware frequency */
  }
  return s_counter_frequency;
}

void oops_time_init(void) {
  if (s_time_initialized)
    return;

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
  if (!s_time_initialized)
    oops_time_init();
  return s_tsc_frequency;
}

uint64_t oops_time_get_us(void) {
  if (sceKernelGetProcessTime) {
    return sceKernelGetProcessTime();
  }
  if (sceKernelGetProcessTimeCounter) {
    return scale(oops_time_get_counter(), 1000000ULL,
                 oops_time_get_counter_frequency());
  }
  return scale(oops_time_get_ticks(), 1000000ULL, oops_time_get_frequency());
}

/*
 * The wall clock, which every other call in this file is not.
 *
 * `clock_gettime(CLOCK_REALTIME, &ts)` through the FreeBSD syscall table, for the same reason
 * `oops_fs_mkdir` reaches `SYS_mkdir` directly: the kernel here is FreeBSD-derived and its
 * numbers are the ones `<oops/syscall.h>` already carries.
 *
 * `struct timespec` is declared here rather than pulled from a header because this SDK has no
 * `<time.h>` of the POSIX kind and should not grow one for a two-field struct only this function
 * writes. The layout is `time_t` then `long`, both 64-bit on this target.
 *
 * A failure returns 0, which `<oops/time.h>` defines as "unknown" - not as 1970. There is no
 * value a real date cannot take, so a caller has to be told to check, and it is.
 */
uint64_t oops_time_get_epoch_seconds(void) {
#ifndef OOPS_HOST_BUILD
  struct {
    int64_t tv_sec;
    int64_t tv_nsec;
  } ts = {0, 0};

  /* CLOCK_REALTIME is 0 on FreeBSD. */
  if (sys_call(SYS_clock_gettime, 0, (long)&ts, 0, 0, 0, 0) != 0) {
    return 0;
  }
  if (ts.tv_sec <= 0) {
    return 0; /* a clock that has not been set says so, rather than reporting the epoch */
  }
  return (uint64_t)ts.tv_sec;
#else
  return 0;
#endif
}

uint64_t oops_time_get_ns(void) {
  if (sceKernelGetProcessTimeCounter) {
    return scale(oops_time_get_counter(), 1000000000ULL,
                 oops_time_get_counter_frequency());
  }
  if (sceKernelGetProcessTime) {
    return sceKernelGetProcessTime() * 1000ULL;
  }
  return scale(oops_time_get_ticks(), 1000000000ULL, oops_time_get_frequency());
}

uint64_t oops_time_get_ms(void) { return oops_time_get_us() / 1000ULL; }

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
  /* In chunks: milliseconds * 1000 wraps past 71 minutes. */
  while (milliseconds > 4000000u) {
    oops_time_sleep_us(4000000000u);
    milliseconds -= 4000000u;
  }
  oops_time_sleep_us(milliseconds * 1000u);
}
