#include "oops/thread.h"
#include "oops/system.h"

/* Platform thread symbols */
__attribute__((weak)) int scePthreadAttrInit(void *attr);
__attribute__((weak)) int scePthreadAttrSetstacksize(void *attr,
                                                     size_t stacksize);
__attribute__((weak)) int scePthreadAttrSetprio(void *attr, int prio);
__attribute__((weak)) int scePthreadAttrDestroy(void *attr);
__attribute__((weak)) int scePthreadCreate(void *thread, const void *attr,
                                           void *(*entry)(void *), void *arg,
                                           const char *name);
__attribute__((weak)) int scePthreadJoin(void *thread, void **val);
__attribute__((weak)) int scePthreadDetach(void *thread);
__attribute__((weak)) void scePthreadYield(void);
__attribute__((weak)) void *scePthreadSelf(void);
__attribute__((weak)) int scePthreadEqual(void *t1, void *t2);

/* Platform mutex symbols */
__attribute__((weak)) int scePthreadMutexInit(void *mutex, const void *attr,
                                              const char *name);
__attribute__((weak)) int scePthreadMutexLock(void *mutex);
__attribute__((weak)) int scePthreadMutexTrylock(void *mutex);
__attribute__((weak)) int scePthreadMutexUnlock(void *mutex);
__attribute__((weak)) int scePthreadMutexDestroy(void *mutex);
/* The attribute object a recursive mutex needs. `SCE_PTHREAD_MUTEX_RECURSIVE` is 2, the same
 * value POSIX gives `PTHREAD_MUTEX_RECURSIVE` on this platform's pthread lineage. */
__attribute__((weak)) int scePthreadMutexattrInit(void *attr);
__attribute__((weak)) int scePthreadMutexattrSettype(void *attr, int type);
__attribute__((weak)) int scePthreadMutexattrDestroy(void *attr);

/* Platform thread-local storage symbols */
__attribute__((weak)) int scePthreadKeyCreate(uint32_t *key, void (*dtor)(void *));
__attribute__((weak)) int scePthreadKeyDelete(uint32_t key);
__attribute__((weak)) void *scePthreadGetspecific(uint32_t key);
__attribute__((weak)) int scePthreadSetspecific(uint32_t key, const void *value);

/* Platform condvar symbols */
__attribute__((weak)) int scePthreadCondInit(void *cond, const void *attr,
                                             const char *name);
__attribute__((weak)) int scePthreadCondWait(void *cond, void *mutex);
__attribute__((weak)) int scePthreadCondTimedwait(void *cond, void *mutex,
                                                  uint64_t usec);
__attribute__((weak)) int scePthreadCondSignal(void *cond);
__attribute__((weak)) int scePthreadCondBroadcast(void *cond);
__attribute__((weak)) int scePthreadCondDestroy(void *cond);

/* Platform semaphore symbols */
__attribute__((weak)) int sceKernelCreateSema(int32_t *sema, const char *name,
                                              uint32_t attr, int init_count,
                                              int max_count, const void *opt);
__attribute__((weak)) int sceKernelWaitSema(int32_t sema, int need,
                                            const void *timeout);
__attribute__((weak)) int sceKernelPollSema(int32_t sema, int need);
__attribute__((weak)) int sceKernelSignalSema(int32_t sema, int signal);
__attribute__((weak)) int sceKernelDeleteSema(int32_t sema);

/* Exception handling (libkernel; addresses 0x800028660/0x8000287b0/0x8000288e0
 * in the payload leg, resolved here by name). The handler receives (signum,
 * arg1, arg2) as measured. */
__attribute__((weak)) int
sceKernelInstallExceptionHandler(int signo,
                                 void (*handler)(int, void *, void *));
__attribute__((weak)) int sceKernelRemoveExceptionHandler(int signo);
__attribute__((weak)) int sceKernelRaiseException(void *thread, int signo);

oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *),
                                 void *arg, int priority, size_t stack_size) {
  if (!scePthreadCreate) {
    oops_log_warn("THREAD", "create: scePthreadCreate unavailable");
    return NULL;
  }

  void *thread_handle = NULL;
  char attr_buf[128];
  void *attr_ptr = NULL;

  if (scePthreadAttrInit && (stack_size > 0 || priority != 0)) {
    for (size_t i = 0; i < sizeof(attr_buf); i++)
      attr_buf[i] = 0;
    if (scePthreadAttrInit(attr_buf) == 0) {
      attr_ptr = attr_buf;
      if (stack_size > 0 && scePthreadAttrSetstacksize) {
        scePthreadAttrSetstacksize(attr_ptr, stack_size);
      }
      if (priority != 0 && scePthreadAttrSetprio) {
        scePthreadAttrSetprio(attr_ptr, priority);
      }
    }
  }

  const char *thread_name = name ? name : "oops_worker";
  oops_log_debug("THREAD", "create '%s' entry=%p arg=%p prio=%d stack=%zu",
                 thread_name, (void *)entry, arg, priority, stack_size);
  int rc = scePthreadCreate(&thread_handle, attr_ptr, entry, arg, thread_name);

  if (attr_ptr && scePthreadAttrDestroy) {
    scePthreadAttrDestroy(attr_ptr);
  }

  if (rc != 0) {
    oops_log_warn("THREAD", "create '%s' failed rc=%d", thread_name, rc);
    return NULL;
  }
  return thread_handle;
}

int oops_thread_join(oops_thread_t thread, void **out_retval) {
  if (!scePthreadJoin || !thread)
    return -1;
  oops_log_trace("THREAD", "join thread=%p", thread);
  return scePthreadJoin(thread, out_retval);
}

int oops_thread_detach(oops_thread_t thread) {
  if (!scePthreadDetach || !thread)
    return -1;
  oops_log_trace("THREAD", "detach thread=%p", thread);
  return scePthreadDetach(thread);
}

void oops_thread_yield(void) {
  if (scePthreadYield) {
    scePthreadYield();
  } else {
    __asm__ __volatile__("pause");
  }
}

oops_thread_t oops_thread_self(void) {
  if (scePthreadSelf) {
    return scePthreadSelf();
  }
  return NULL;
}

int oops_thread_equal(oops_thread_t t1, oops_thread_t t2) {
  if (scePthreadEqual) {
    return scePthreadEqual(t1, t2);
  }
  return (t1 == t2);
}

/* Mutex implementation */

int oops_mutex_init(oops_mutex_t *mutex, const char *name) {
  if (!mutex || !scePthreadMutexInit)
    return -1;
  const char *mtx_name = name ? name : "oops_mtx";
  oops_log_trace("THREAD", "mutex_init '%s' mtx=%p", mtx_name, (void *)mutex);
  return scePthreadMutexInit(&mutex->handle, NULL, mtx_name);
}

int oops_mutex_lock(oops_mutex_t *mutex) {
  if (!mutex || !scePthreadMutexLock)
    return -1;
  return scePthreadMutexLock(&mutex->handle);
}

int oops_mutex_trylock(oops_mutex_t *mutex) {
  if (!mutex || !scePthreadMutexTrylock)
    return -1;
  return scePthreadMutexTrylock(&mutex->handle);
}

int oops_mutex_unlock(oops_mutex_t *mutex) {
  if (!mutex || !scePthreadMutexUnlock)
    return -1;
  return scePthreadMutexUnlock(&mutex->handle);
}

int oops_mutex_destroy(oops_mutex_t *mutex) {
  if (!mutex || !scePthreadMutexDestroy)
    return -1;
  oops_log_trace("THREAD", "mutex_destroy mtx=%p", (void *)mutex);
  return scePthreadMutexDestroy(&mutex->handle);
}

/*
 * **A recursive mutex, which is the same object with a different initialiser.**
 *
 * `oops_mutex_init` passes a null attribute, which is the default kind: locking it twice from
 * one thread deadlocks. This builds an attribute object, sets the recursive type on it, and
 * hands that to the same `scePthreadMutexInit`. Everything afterwards - lock, trylock, unlock,
 * destroy - is shared, because the difference lives in the mutex and not in the verbs.
 *
 * **The attribute is destroyed immediately.** pthread copies what it needs out of it at init,
 * so keeping it would be a leak per mutex for nothing. Destroyed even when the init failed,
 * which is the case a `goto`-free version of this gets wrong.
 *
 * The attribute object's size is not something this can ask for, so it is a local buffer with
 * room to spare. Sony's `ScePthreadMutexattr` is a pointer-sized handle on this platform, like
 * the mutex itself; 64 bytes is far more than it needs and costs one stack frame.
 */
int oops_mutex_init_recursive(oops_mutex_t *mutex, const char *name) {
  if (!mutex || !scePthreadMutexInit)
    return -1;
  /* Without the attribute symbols there is no way to ask for recursion, and returning the
   * *non*-recursive mutex the caller did not ask for is the kind of quiet substitution that
   * deadlocks somewhere else entirely. Refuse instead. */
  if (!scePthreadMutexattrInit || !scePthreadMutexattrSettype ||
      !scePthreadMutexattrDestroy)
    return -1;

  const char *mtx_name = name ? name : "oops_rmtx";
  unsigned char attr[64];
  void *attrp = (void *)attr;
  for (unsigned i = 0; i < sizeof(attr); i++)
    attr[i] = 0;

  if (scePthreadMutexattrInit(attrp) != 0)
    return -1;
  int rc = scePthreadMutexattrSettype(attrp, 2 /* SCE_PTHREAD_MUTEX_RECURSIVE */);
  if (rc == 0) {
    oops_log_trace("THREAD", "mutex_init_recursive '%s' mtx=%p", mtx_name,
                   (void *)mutex);
    rc = scePthreadMutexInit(&mutex->handle, attrp, mtx_name);
  }
  scePthreadMutexattrDestroy(attrp);
  return rc;
}

/*
 * Thread-local storage.
 *
 * Thin over the platform's pthread keys, with the one behaviour a caller depends on made
 * explicit: `oops_tls_get` on a thread that has never set the key answers NULL rather than
 * failing, so "first use on this thread" is detectable without a second flag. That is what
 * pthread already does; it is stated here because the rest of this file returns -1 for
 * "unavailable" and a pointer-returning function cannot.
 */
int oops_tls_create(oops_tls_key_t *key, void (*destructor)(void *)) {
  if (!key || !scePthreadKeyCreate)
    return -1;
  const int rc = scePthreadKeyCreate(key, destructor);
  oops_log_trace("THREAD", "tls_create key=%u rc=%d", (unsigned)*key, rc);
  return rc;
}

int oops_tls_delete(oops_tls_key_t key) {
  if (!scePthreadKeyDelete)
    return -1;
  return scePthreadKeyDelete(key);
}

void *oops_tls_get(oops_tls_key_t key) {
  if (!scePthreadGetspecific)
    return (void *)0;
  return scePthreadGetspecific(key);
}

int oops_tls_set(oops_tls_key_t key, const void *value) {
  if (!scePthreadSetspecific)
    return -1;
  return scePthreadSetspecific(key, value);
}

/* Condition variable implementation */

int oops_cond_init(oops_cond_t *cond, const char *name) {
  if (!cond || !scePthreadCondInit)
    return -1;
  const char *cond_name = name ? name : "oops_cond";
  oops_log_trace("THREAD", "cond_init '%s' cond=%p", cond_name, (void *)cond);
  return scePthreadCondInit(&cond->handle, NULL, cond_name);
}

int oops_cond_wait(oops_cond_t *cond, oops_mutex_t *mutex) {
  if (!cond || !mutex || !scePthreadCondWait)
    return -1;
  return scePthreadCondWait(&cond->handle, &mutex->handle);
}

int oops_cond_timedwait(oops_cond_t *cond, oops_mutex_t *mutex,
                        uint32_t timeout_us) {
  if (!cond || !mutex || !scePthreadCondTimedwait)
    return -1;
  /* No fallback to the untimed wait: a caller who asked for a timeout is
   * relying on coming back, and a wait that never returns is not a degraded
   * version of that. */
  return scePthreadCondTimedwait(&cond->handle, &mutex->handle,
                                 (uint64_t)timeout_us);
}

int oops_cond_signal(oops_cond_t *cond) {
  if (!cond || !scePthreadCondSignal)
    return -1;
  return scePthreadCondSignal(&cond->handle);
}

int oops_cond_broadcast(oops_cond_t *cond) {
  if (!cond || !scePthreadCondBroadcast)
    return -1;
  return scePthreadCondBroadcast(&cond->handle);
}

int oops_cond_destroy(oops_cond_t *cond) {
  if (!cond || !scePthreadCondDestroy)
    return -1;
  oops_log_trace("THREAD", "cond_destroy cond=%p", (void *)cond);
  return scePthreadCondDestroy(&cond->handle);
}

/* Semaphore implementation */

int oops_sem_init(oops_sem_t *sem, const char *name, int initial_count,
                  int max_count) {
  if (!sem || !sceKernelCreateSema)
    return -1;
  const char *sema_name = name ? name : "oops_sem";
  int32_t handle = 0;
  int rc = sceKernelCreateSema(&handle, sema_name, 0, initial_count, max_count,
                               NULL);
  oops_log_debug("THREAD", "sem_init '%s' init=%d max=%d -> handle=%d rc=%d",
                 sema_name, initial_count, max_count, handle, rc);
  if (rc == 0) {
    sem->handle = handle;
    sem->max_count = max_count;
    return 0;
  }
  return rc;
}

int oops_sem_wait(oops_sem_t *sem, int count) {
  if (!sem || !sceKernelWaitSema || sem->handle <= 0)
    return -1;
  int n = (count <= 0) ? 1 : count;
  return sceKernelWaitSema(sem->handle, n, NULL);
}

int oops_sem_poll(oops_sem_t *sem, int count) {
  if (!sem || !sceKernelPollSema || sem->handle <= 0)
    return -1;
  int n = (count <= 0) ? 1 : count;
  return sceKernelPollSema(sem->handle, n);
}

int oops_sem_signal(oops_sem_t *sem, int count) {
  if (!sem || !sceKernelSignalSema || sem->handle <= 0)
    return -1;
  int n = (count <= 0) ? 1 : count;
  return sceKernelSignalSema(sem->handle, n);
}

int oops_sem_destroy(oops_sem_t *sem) {
  if (!sem || !sceKernelDeleteSema || sem->handle <= 0)
    return -1;
  oops_log_debug("THREAD", "sem_destroy handle=%d", sem->handle);
  int rc = sceKernelDeleteSema(sem->handle);
  sem->handle = -1;
  return rc;
}

/*
 * Exception handling. Thin wrappers over the confirmed libkernel entry points;
 * where one does not resolve the call fails rather than pretending. A NULL
 * handler on install is refused locally (the platform reports 0x80020023 for
 * it, but there is nothing to install).
 */
int oops_thread_install_exception_handler(int signum,
                                          oops_exception_handler_t handler) {
  if (!sceKernelInstallExceptionHandler || !handler)
    return -1;
  oops_log_debug("THREAD", "install exception handler signo=%d handler=%p",
                 signum, (void *)handler);
  return sceKernelInstallExceptionHandler(signum, handler);
}

int oops_thread_remove_exception_handler(int signum) {
  if (!sceKernelRemoveExceptionHandler)
    return -1;
  oops_log_debug("THREAD", "remove exception handler signo=%d", signum);
  return sceKernelRemoveExceptionHandler(signum);
}

int oops_thread_raise_exception(oops_thread_t thread, int signum) {
  if (!sceKernelRaiseException)
    return -1;
  oops_log_warn("THREAD", "raise exception thread=%p signo=%d", thread, signum);
  return sceKernelRaiseException(thread, signum);
}
