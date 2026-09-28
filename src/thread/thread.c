/*
 * Threads, mutexes, condition variables, semaphores, thread-local storage and
 * exception handlers over the platform's scePthread and sceKernel entry points, all
 * weakly bound.
 */
#include "oops/thread.h"
#include "oops/heap.h" /* the per-thread destructor list below */
#include "oops/system.h"

/* Platform thread symbols */
__attribute__((weak)) int scePthreadAttrInit(void *attr);
__attribute__((weak)) int scePthreadAttrSetstacksize(void *attr, size_t stacksize);
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
/* The attribute object a recursive mutex needs. `SCE_PTHREAD_MUTEX_RECURSIVE` is 2, the
 * same value POSIX gives `PTHREAD_MUTEX_RECURSIVE` on this platform's pthread lineage.
 */
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

/*
 * Platform semaphore symbols. The handle is pointer-sized: `sceKernelCreateSema` writes
 * eight bytes through its out-parameter, measured on hardware as a guard word after an
 * `int` coming back zeroed (obscene `018-relational/handle-fits-its-out-parameter`).
 * Declared `int32_t *`, the upper half landed on whatever the compiler placed next to
 * the local - in `oops_sem_init` that was the caller's saved `rbx`, so SDL's
 * `SDL_CreateSemaphore` returned its allocation with the low half zeroed and Neverball
 * faulted freeing it.
 */
__attribute__((weak)) int sceKernelCreateSema(int64_t *sema, const char *name,
                                              uint32_t attr, int init_count,
                                              int max_count, const void *opt);
__attribute__((weak)) int sceKernelWaitSema(int64_t sema, int need,
                                            const void *timeout);
__attribute__((weak)) int sceKernelPollSema(int64_t sema, int need);
__attribute__((weak)) int sceKernelSignalSema(int64_t sema, int signal);
__attribute__((weak)) int sceKernelDeleteSema(int64_t sema);

/* Exception handling (libkernel, resolved by name). The handler receives (signum,
 * arg1, arg2), as measured on hardware. */
__attribute__((weak)) int
sceKernelInstallExceptionHandler(int signo, void (*handler)(int, void *, void *));
__attribute__((weak)) int sceKernelRemoveExceptionHandler(int signo);
__attribute__((weak)) int sceKernelRaiseException(void *thread, int signo);

/* Defined below, beside the other diagnostics; called from the first thread creation.
 */
void oops_thread_dump_kernel_imports(void);

oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *), void *arg,
                                 int priority, size_t stack_size) {
    oops_thread_dump_kernel_imports();
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

/*
 * A mutex whose handle is still zero has never been through `scePthreadMutexInit`.
 *
 * Handing one to libkernel is not an error it reports: it dereferences the handle and
 * takes a page fault at address 0x8, on whatever thread made the call, with a register
 * dump naming libkernel rather than the caller. Ship of Harkinian and Spaghetti Kart
 * both died exactly there - same faulting instruction, same `rax` of 0, both on a
 * thread named `libcxx` - and the log said nothing about which lock it was.
 *
 * So the check belongs here, at the boundary that makes the call, rather than in any
 * one caller: every consumer of `oops/thread.h` gets an honest -1 and a line naming the
 * operation instead of a fault in somebody else's library.
 */
/*
 * Where libkernel's threading entry points landed, once, at the first thread creation.
 *
 * A fatal signal reports `rip` and nothing about who owns it: a fault inside libkernel
 * names libkernel, not the call that got there. The module is loaded at a fixed base,
 * so the entry point with the largest address at or below a faulting `rip` is the
 * function containing it, and that turns a register dump into a name. Printed at INFO
 * once per process, which is the cost of one line against a run that otherwise ends in
 * a guess.
 */
void oops_thread_dump_kernel_imports(void) {
    static int done = 0;
    if (done)
        return;
    done = 1;

    struct {
        const char *name;
        const void *fn;
    } const entries[] = {
        {"scePthreadCreate", (const void *)scePthreadCreate},
        {"scePthreadJoin", (const void *)scePthreadJoin},
        {"scePthreadDetach", (const void *)scePthreadDetach},
        {"scePthreadYield", (const void *)scePthreadYield},
        {"scePthreadSelf", (const void *)scePthreadSelf},
        {"scePthreadEqual", (const void *)scePthreadEqual},
        {"scePthreadAttrInit", (const void *)scePthreadAttrInit},
        {"scePthreadAttrDestroy", (const void *)scePthreadAttrDestroy},
        {"scePthreadMutexInit", (const void *)scePthreadMutexInit},
        {"scePthreadMutexLock", (const void *)scePthreadMutexLock},
        {"scePthreadMutexTrylock", (const void *)scePthreadMutexTrylock},
        {"scePthreadMutexUnlock", (const void *)scePthreadMutexUnlock},
        {"scePthreadMutexDestroy", (const void *)scePthreadMutexDestroy},
        {"scePthreadMutexattrInit", (const void *)scePthreadMutexattrInit},
        {"scePthreadMutexattrSettype", (const void *)scePthreadMutexattrSettype},
        {"scePthreadKeyCreate", (const void *)scePthreadKeyCreate},
        {"scePthreadGetspecific", (const void *)scePthreadGetspecific},
        {"scePthreadSetspecific", (const void *)scePthreadSetspecific},
        {"scePthreadCondInit", (const void *)scePthreadCondInit},
        {"scePthreadCondWait", (const void *)scePthreadCondWait},
        {"scePthreadCondTimedwait", (const void *)scePthreadCondTimedwait},
        {"scePthreadCondSignal", (const void *)scePthreadCondSignal},
        {"scePthreadCondBroadcast", (const void *)scePthreadCondBroadcast},
        {"scePthreadCondDestroy", (const void *)scePthreadCondDestroy},
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        oops_log_info("THREAD", "import %-26s %p", entries[i].name, entries[i].fn);
    }
}

static int oops_mutex_absent(const oops_mutex_t *mutex, const char *op) {
    if (mutex && mutex->handle)
        return 0;
    oops_log_warn("THREAD", "mutex_%s on a mutex that was never initialised (mtx=%p)",
                  op, (const void *)mutex);
    return 1;
}

static int oops_cond_absent(const oops_cond_t *cond, const char *op) {
    if (cond && cond->handle)
        return 0;
    oops_log_warn("THREAD",
                  "cond_%s on a condition variable that was never "
                  "initialised (cond=%p)",
                  op, (const void *)cond);
    return 1;
}

int oops_mutex_init(oops_mutex_t *mutex, const char *name) {
    if (!mutex || !scePthreadMutexInit)
        return -1;
    const char *mtx_name = name ? name : "oops_mtx";
    oops_log_trace("THREAD", "mutex_init '%s' mtx=%p", mtx_name, (void *)mutex);
    return scePthreadMutexInit(&mutex->handle, NULL, mtx_name);
}

int oops_mutex_lock(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexLock || oops_mutex_absent(mutex, "lock"))
        return -1;
    return scePthreadMutexLock(&mutex->handle);
}

int oops_mutex_trylock(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexTrylock || oops_mutex_absent(mutex, "trylock"))
        return -1;
    return scePthreadMutexTrylock(&mutex->handle);
}

int oops_mutex_unlock(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexUnlock || oops_mutex_absent(mutex, "unlock"))
        return -1;
    return scePthreadMutexUnlock(&mutex->handle);
}

int oops_mutex_destroy(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexDestroy || oops_mutex_absent(mutex, "destroy"))
        return -1;
    oops_log_trace("THREAD", "mutex_destroy mtx=%p", (void *)mutex);
    return scePthreadMutexDestroy(&mutex->handle);
}

/*
 * A recursive mutex: the same object as `oops_mutex_init` makes, initialised with an
 * attribute that sets the recursive type, so lock, trylock, unlock and destroy are
 * shared. The attribute is destroyed straight after init, whether or not init
 * succeeded, since pthread copies what it needs. `ScePthreadMutexattr` is a
 * pointer-sized handle; the 64-byte buffer leaves room to spare.
 */
int oops_mutex_init_recursive(oops_mutex_t *mutex, const char *name) {
    if (!mutex || !scePthreadMutexInit)
        return -1;
    /* Without the attribute symbols recursion cannot be asked for, and a non-recursive
     * mutex in its place would deadlock elsewhere, so this refuses. */
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
 * Thread-local storage over the platform's pthread keys. `oops_tls_get` on a thread
 * that has never set the key answers NULL, as pthread does, so first use on a thread
 * is detectable; it also answers NULL when the entry point is unavailable.
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

/*
 * `__cxa_thread_atexit`: the C++ ABI's hook for a `thread_local` object with a
 * destructor, run when its thread ends. It is here, beside the keys it is built on,
 * rather than in a C++ runtime: oops-apps' libc++abi is built without threads, which
 * empties its own `cxa_thread_atexit.cpp`, and a title that has thread-locals has this
 * module.
 *
 * The technique is libc++abi's fallback for a C library without
 * `__cxa_thread_atexit_impl`: a per-thread list, newest first, held in a key whose
 * destructor runs it. The platform's keys run their destructors at thread exit and
 * clear the value first, so a destructor that registers another starts a fresh list the
 * platform runs on its next pass. Nothing runs for the main thread, which leaves
 * through `exit` - the same as `__cxa_atexit`.
 */
struct oops_thread_dtor {
    void (*fn)(void *);
    void *obj;
    struct oops_thread_dtor *next;
};

static oops_tls_key_t s_thread_dtors;
static int s_thread_dtors_state; /* 0 unmade, 1 being made, 2 ready, -1 unavailable */

static void oops_run_thread_dtors(void *head) {
    struct oops_thread_dtor *e = (struct oops_thread_dtor *)head;
    while (e) {
        struct oops_thread_dtor *next = e->next;
        e->fn(e->obj);
        oops_free(e);
        e = next;
    }
}

static int oops_thread_dtors_ready(void) {
    for (;;) {
        const int state = __atomic_load_n(&s_thread_dtors_state, __ATOMIC_ACQUIRE);
        int unmade = 0;
        if (state == 2 || state == -1)
            return state == 2;
        if (state == 0 &&
            __atomic_compare_exchange_n(&s_thread_dtors_state, &unmade, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            const int made =
                oops_tls_create(&s_thread_dtors, oops_run_thread_dtors) == 0;
            __atomic_store_n(&s_thread_dtors_state, made ? 2 : -1, __ATOMIC_RELEASE);
            return made;
        }
    }
}

int __cxa_thread_atexit(void (*destructor)(void *), void *obj, void *dso);
int __cxa_thread_atexit(void (*destructor)(void *), void *obj, void *dso) {
    struct oops_thread_dtor *e;

    (void)dso;
    if (!oops_thread_dtors_ready())
        return -1;
    e = (struct oops_thread_dtor *)oops_malloc(sizeof(*e));
    if (!e)
        return -1;
    e->fn = destructor;
    e->obj = obj;
    e->next = (struct oops_thread_dtor *)oops_tls_get(s_thread_dtors);
    if (oops_tls_set(s_thread_dtors, e) != 0) {
        oops_free(e);
        return -1;
    }
    return 0;
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
    if (!cond || !mutex || !scePthreadCondWait || oops_cond_absent(cond, "wait") ||
        oops_mutex_absent(mutex, "wait"))
        return -1;
    return scePthreadCondWait(&cond->handle, &mutex->handle);
}

int oops_cond_timedwait(oops_cond_t *cond, oops_mutex_t *mutex, uint32_t timeout_us) {
    if (!cond || !mutex || !scePthreadCondTimedwait ||
        oops_cond_absent(cond, "timedwait") || oops_mutex_absent(mutex, "timedwait"))
        return -1;
    /* No fallback to the untimed wait: a caller who asked for a timeout relies on
     * coming back. */
    return scePthreadCondTimedwait(&cond->handle, &mutex->handle, (uint64_t)timeout_us);
}

int oops_cond_signal(oops_cond_t *cond) {
    if (!cond || !scePthreadCondSignal || oops_cond_absent(cond, "signal"))
        return -1;
    return scePthreadCondSignal(&cond->handle);
}

int oops_cond_broadcast(oops_cond_t *cond) {
    if (!cond || !scePthreadCondBroadcast || oops_cond_absent(cond, "broadcast"))
        return -1;
    return scePthreadCondBroadcast(&cond->handle);
}

int oops_cond_destroy(oops_cond_t *cond) {
    if (!cond || !scePthreadCondDestroy || oops_cond_absent(cond, "destroy"))
        return -1;
    oops_log_trace("THREAD", "cond_destroy cond=%p", (void *)cond);
    return scePthreadCondDestroy(&cond->handle);
}

/* Semaphore implementation */

int oops_sem_init(oops_sem_t *sem, const char *name, int initial_count, int max_count) {
    if (!sem || !sceKernelCreateSema)
        return -1;
    const char *sema_name = name ? name : "oops_sem";
    int64_t handle = 0;
    int rc = sceKernelCreateSema(&handle, sema_name, 0, initial_count, max_count, NULL);
    oops_log_debug("THREAD", "sem_init '%s' init=%d max=%d -> handle=%lld rc=%d",
                   sema_name, initial_count, max_count, (long long)handle, rc);
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
    oops_log_debug("THREAD", "sem_destroy handle=%lld", (long long)sem->handle);
    int rc = sceKernelDeleteSema(sem->handle);
    sem->handle = -1;
    return rc;
}

/*
 * Exception handling over the libkernel entry points; a call whose entry point does
 * not resolve fails. A NULL handler on install is refused locally (the platform
 * reports 0x80020023 for it).
 */
int oops_thread_install_exception_handler(int signum,
                                          oops_exception_handler_t handler) {
    if (!sceKernelInstallExceptionHandler || !handler)
        return -1;
    oops_log_debug("THREAD", "install exception handler signo=%d handler=%p", signum,
                   (void *)handler);
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
