/*
 * <pthread.h> - POSIX threads over oops/thread.h.
 *
 * Covers what C11 <threads.h> implementations and libc++ ask for: threads, mutexes
 * including recursive, condition variables, thread-local keys. Cancellation, read-write
 * locks, barriers and spinlocks are absent because nothing here uses them.
 *
 * Every operation locks; none returns success without acting. include/libc precedes a
 * title's own include directories, so this header, not a title's shim, is what a port
 * gets.
 */
#ifndef OOPS_LIBC_PTHREAD_H
#define OOPS_LIBC_PTHREAD_H

#include "oops/system.h" /* oops_klog, for the pthread_exit message below */
#include "oops/thread.h"
#include "oops/time.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> /* abort, likewise */

#ifdef __cplusplus
extern "C" {
#endif

typedef oops_thread_t pthread_t;
typedef oops_mutex_t pthread_mutex_t;
typedef oops_cond_t pthread_cond_t;
typedef oops_tls_key_t pthread_key_t;

/* Thread and condition attributes carry nothing: every caller here passes NULL. */
typedef struct {
    int unused;
} pthread_attr_t;
typedef struct {
    int unused;
} pthread_condattr_t;

/* Ignoring the attribute type deadlocks a recursive mutex on its second lock. 2 is
 * FreeBSD's value for RECURSIVE; glibc uses 1, so code naming the number rather than
 * the macro is wrong. */
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

typedef struct {
    int type;
} pthread_mutexattr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

/* PTHREAD_MUTEX_INITIALIZER is undefined on purpose: oops_mutex_t is a handle
 * oops_mutex_init fills in, so a zeroed mutex locks nothing. Undefined makes that a
 * build error. */

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------- */

/* oops-sdk wants a name, a priority and a stack size, which pthreads cannot express.
 * The priority is the platform's middle and the stack is 256 KiB - enough for a worker
 * that recurses nowhere. */
#define OOPS_PTHREAD_PRIORITY 700
#define OOPS_PTHREAD_STACK (256u * 1024u)

/* `attr` is accepted and ignored; every caller here passes NULL. A non-NULL one would
 * not be honoured, so it is named and documented rather than quietly dropped. */
static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg) {
    oops_thread_t t;
    (void)attr;
    t = oops_thread_create("pthread", start_routine, arg, OOPS_PTHREAD_PRIORITY,
                           OOPS_PTHREAD_STACK);
    if (!t) {
        return -1;
    }
    if (thread) {
        *thread = t;
    }
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    return oops_thread_join(thread, retval);
}
static inline int pthread_detach(pthread_t thread) {
    return oops_thread_detach(thread);
}
static inline pthread_t pthread_self(void) {
    return oops_thread_self();
}
static inline int pthread_equal(pthread_t a, pthread_t b) {
    return oops_thread_equal(a, b);
}
static inline void pthread_yield(void) {
    oops_thread_yield();
}

/*
 * Ends the process, not the thread: a thread here ends by returning from its entry and
 * cannot be unwound from the middle. Returning normally would resume the caller's code,
 * and an undefined symbol would link silently and jump to zero, so this aborts where
 * the mistake is. */
static inline void pthread_exit(void *retval) {
    (void)retval;
    oops_klog("PTHREAD", "pthread_exit: this target cannot end one thread; aborting");
    abort();
}

/* ---------------------------------------------------------------------------
 * Thread-local storage
 * ------------------------------------------------------------------------- */

static inline int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    return oops_tls_create(key, destructor);
}
static inline int pthread_key_delete(pthread_key_t key) {
    return oops_tls_delete(key);
}
static inline void *pthread_getspecific(pthread_key_t key) {
    return oops_tls_get(key);
}
static inline int pthread_setspecific(pthread_key_t key, const void *value) {
    return oops_tls_set(key, value);
}

/* ---------------------------------------------------------------------------
 * Mutexes
 * ------------------------------------------------------------------------- */

static inline int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (attr) {
        attr->type = PTHREAD_MUTEX_DEFAULT;
    }
    return 0;
}
static inline int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) {
        return -1;
    }
    attr->type = type;
    return 0;
}
static inline int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr; /* it owns nothing */
    return 0;
}

/* Honours `PTHREAD_MUTEX_RECURSIVE`; a NULL attribute is the default kind, as POSIX
 * says. */
static inline int pthread_mutex_init(pthread_mutex_t *m,
                                     const pthread_mutexattr_t *attr) {
    if (attr && attr->type == PTHREAD_MUTEX_RECURSIVE) {
        return oops_mutex_init_recursive(m, "pthread");
    }
    return oops_mutex_init(m, "pthread");
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    return oops_mutex_destroy(m);
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    return oops_mutex_lock(m);
}
static inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    return oops_mutex_trylock(m);
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    return oops_mutex_unlock(m);
}

/* ---------------------------------------------------------------------------
 * Condition variables
 * ------------------------------------------------------------------------- */

static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
    (void)attr;
    return oops_cond_init(c, "pthread");
}
static inline int pthread_cond_destroy(pthread_cond_t *c) {
    return oops_cond_destroy(c);
}
static inline int pthread_cond_signal(pthread_cond_t *c) {
    return oops_cond_signal(c);
}
static inline int pthread_cond_broadcast(pthread_cond_t *c) {
    return oops_cond_broadcast(c);
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return oops_cond_wait(c, m);
}

/*
 * POSIX gives a wall-clock instant; oops_cond_timedwait takes microseconds from now.
 * Now comes from oops_time_get_ns, a monotonic clock, so the comparison mixes clocks -
 * callers use this only to bound a wait a signal normally ends. A past deadline clamps
 * to zero. */
static inline int pthread_cond_timedwait_us(pthread_cond_t *c, pthread_mutex_t *m,
                                            long long abs_sec, long long abs_nsec) {
    const unsigned long long now_ns = oops_time_get_ns();
    const long long want_ns = abs_sec * 1000000000LL + abs_nsec;
    const long long delta_ns = want_ns - (long long)now_ns;
    const long long us = delta_ns > 0 ? delta_ns / 1000LL : 0LL;
    return oops_cond_timedwait(c, m, us > 0xffffffffLL ? 0xffffffffu : (uint32_t)us);
}

#define pthread_cond_timedwait(c, m, abstime)                                          \
    pthread_cond_timedwait_us((c), (m), (long long)(abstime)->tv_sec,                  \
                              (long long)(abstime)->tv_nsec)

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_PTHREAD_H */
