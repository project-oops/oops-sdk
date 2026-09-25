#ifndef OOPS_THREAD_H
#define OOPS_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *oops_thread_t;

typedef struct oops_mutex {
  void *handle;
  uint32_t reserved[8];
} oops_mutex_t;

typedef struct oops_cond {
  void *handle;
  uint32_t reserved[8];
} oops_cond_t;

typedef struct oops_sem {
  int32_t handle;
  int32_t max_count;
  uint32_t reserved[6];
} oops_sem_t;

/* Thread management */
oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *),
                                 void *arg, int priority, size_t stack_size);
int oops_thread_join(oops_thread_t thread, void **out_retval);
int oops_thread_detach(oops_thread_t thread);
void oops_thread_yield(void);
oops_thread_t oops_thread_self(void);
int oops_thread_equal(oops_thread_t t1, oops_thread_t t2);

/* Mutexes */
int oops_mutex_init(oops_mutex_t *mutex, const char *name);
/*
 * **A mutex the owning thread may lock again.** `oops_mutex_init` creates the default kind,
 * which deadlocks on a second lock from the same thread; this one counts.
 *
 * Added for C++'s `std::recursive_mutex`, which libc++ requires as a *distinct* type from
 * `std::mutex` - its external threading contract has `__libcpp_recursive_mutex_t` alongside
 * `__libcpp_mutex_t` and will not accept one standing in for both. Everything else about the
 * two is the same, including `oops_mutex_lock` and the rest of the operations: only the
 * initialiser differs, so there is one type and one set of verbs.
 */
int oops_mutex_init_recursive(oops_mutex_t *mutex, const char *name);
int oops_mutex_lock(oops_mutex_t *mutex);
int oops_mutex_trylock(oops_mutex_t *mutex);
int oops_mutex_unlock(oops_mutex_t *mutex);
int oops_mutex_destroy(oops_mutex_t *mutex);

/*
 * Thread-local storage, by key.
 *
 * A key is created once and then carries a different value per thread. Added for C++'s
 * `thread_local` and for libc++'s `__libcpp_tls_*`, which every threaded C++ program reaches
 * through `std::thread`'s own bookkeeping even when it declares no thread-local of its own.
 *
 * `oops_tls_get` on a thread that never set the key answers NULL rather than failing: that is
 * what every caller of this shape expects, and it is what makes "first use on this thread"
 * detectable without a second flag.
 */
typedef uint32_t oops_tls_key_t;
int oops_tls_create(oops_tls_key_t *key, void (*destructor)(void *));
int oops_tls_delete(oops_tls_key_t key);
void *oops_tls_get(oops_tls_key_t key);
int oops_tls_set(oops_tls_key_t key, const void *value);

/* Condition variables */
int oops_cond_init(oops_cond_t *cond, const char *name);
int oops_cond_wait(oops_cond_t *cond, oops_mutex_t *mutex);
int oops_cond_timedwait(oops_cond_t *cond, oops_mutex_t *mutex,
                        uint32_t timeout_us);
int oops_cond_signal(oops_cond_t *cond);
int oops_cond_broadcast(oops_cond_t *cond);
int oops_cond_destroy(oops_cond_t *cond);

/* Semaphores */
int oops_sem_init(oops_sem_t *sem, const char *name, int initial_count,
                  int max_count);
int oops_sem_wait(oops_sem_t *sem, int count);
int oops_sem_poll(oops_sem_t *sem, int count);
int oops_sem_signal(oops_sem_t *sem, int count);
int oops_sem_destroy(oops_sem_t *sem);

/*
 * Exception handling (Prospero, hardware-confirmed - obSCEne sweep
 * 20260909-151910, payload leg; libkernel
 * sceKernelInstallExceptionHandler/RemoveExceptionHandler/RaiseException).
 *
 * Delivery is synchronous: the handler runs to completion on the raising thread
 * before oops_thread_raise_exception returns. A handler is registered per
 * signal; installing a second for the same signal without removing the first is
 * refused. The handler receives the signal number and two platform pointer
 * arguments.
 */
#define OOPS_EXCEPTION_SIGNAL                                                  \
  30 /* 0x1e: the primary exception signal the platform accepts */

/* Raw platform codes the exception calls return (0x8002xxxx is the kernel errno
 * encoding). */
#define OOPS_EXC_EAGAIN                                                        \
  0x80020023 /* 35: a handler is already installed, or removing with none */
#define OOPS_EXC_EINVAL                                                        \
  0x80020016 /* 22: invalid or inverted arguments, or an unhandled signal */
#define OOPS_EXC_ESRCH 0x80020003 /* 3: no such thread for the target handle   \
                                   */

typedef void (*oops_exception_handler_t)(int signum, void *arg1, void *arg2);

/* Install/remove a per-signal handler. Return 0, a negative code when the entry
 * point is absent, or the platform's raw code above. */
int oops_thread_install_exception_handler(int signum,
                                          oops_exception_handler_t handler);
int oops_thread_remove_exception_handler(int signum);

/* Raise `signum` on `thread`; the handler runs before this returns. 0 on
 * success, a negative code when the entry point is absent, or the platform's
 * raw code (ESRCH/EINVAL). */
int oops_thread_raise_exception(oops_thread_t thread, int signum);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_THREAD_H */
