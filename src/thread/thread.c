#include "oops/thread.h"

/* Platform thread symbols */
__attribute__((weak)) int scePthreadAttrInit(void *attr);
__attribute__((weak)) int scePthreadAttrSetstacksize(void *attr, size_t stacksize);
__attribute__((weak)) int scePthreadAttrSetprio(void *attr, int prio);
__attribute__((weak)) int scePthreadAttrDestroy(void *attr);
__attribute__((weak)) int scePthreadCreate(void *thread, const void *attr, void *(*entry)(void *), void *arg, const char *name);
__attribute__((weak)) int scePthreadJoin(void *thread, void **val);
__attribute__((weak)) int scePthreadDetach(void *thread);
__attribute__((weak)) void scePthreadYield(void);
__attribute__((weak)) void *scePthreadSelf(void);
__attribute__((weak)) int scePthreadEqual(void *t1, void *t2);

/* Platform mutex symbols */
__attribute__((weak)) int scePthreadMutexInit(void *mutex, const void *attr, const char *name);
__attribute__((weak)) int scePthreadMutexLock(void *mutex);
__attribute__((weak)) int scePthreadMutexTrylock(void *mutex);
__attribute__((weak)) int scePthreadMutexUnlock(void *mutex);
__attribute__((weak)) int scePthreadMutexDestroy(void *mutex);

/* Platform condvar symbols */
__attribute__((weak)) int scePthreadCondInit(void *cond, const void *attr, const char *name);
__attribute__((weak)) int scePthreadCondWait(void *cond, void *mutex);
__attribute__((weak)) int scePthreadCondTimedwait(void *cond, void *mutex, uint64_t usec);
__attribute__((weak)) int scePthreadCondSignal(void *cond);
__attribute__((weak)) int scePthreadCondBroadcast(void *cond);
__attribute__((weak)) int scePthreadCondDestroy(void *cond);

/* Platform semaphore symbols */
__attribute__((weak)) int sceKernelCreateSema(int32_t *sema, const char *name, uint32_t attr, int init_count, int max_count, const void *opt);
__attribute__((weak)) int sceKernelWaitSema(int32_t sema, int need, const void *timeout);
__attribute__((weak)) int sceKernelPollSema(int32_t sema, int need);
__attribute__((weak)) int sceKernelSignalSema(int32_t sema, int signal);
__attribute__((weak)) int sceKernelDeleteSema(int32_t sema);

oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *), void *arg, int priority, size_t stack_size) {
    if (!scePthreadCreate) return NULL;

    void *thread_handle = NULL;
    char attr_buf[128];
    void *attr_ptr = NULL;

    if (scePthreadAttrInit && (stack_size > 0 || priority != 0)) {
        for (size_t i = 0; i < sizeof(attr_buf); i++) attr_buf[i] = 0;
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
    int rc = scePthreadCreate(&thread_handle, attr_ptr, entry, arg, thread_name);

    if (attr_ptr && scePthreadAttrDestroy) {
        scePthreadAttrDestroy(attr_ptr);
    }

    return (rc == 0) ? thread_handle : NULL;
}

int oops_thread_join(oops_thread_t thread, void **out_retval) {
    if (!scePthreadJoin || !thread) return -1;
    return scePthreadJoin(thread, out_retval);
}

int oops_thread_detach(oops_thread_t thread) {
    if (!scePthreadDetach || !thread) return -1;
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
    if (!mutex || !scePthreadMutexInit) return -1;
    const char *mtx_name = name ? name : "oops_mtx";
    return scePthreadMutexInit(&mutex->handle, NULL, mtx_name);
}

int oops_mutex_lock(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexLock) return -1;
    return scePthreadMutexLock(&mutex->handle);
}

int oops_mutex_trylock(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexTrylock) return -1;
    return scePthreadMutexTrylock(&mutex->handle);
}

int oops_mutex_unlock(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexUnlock) return -1;
    return scePthreadMutexUnlock(&mutex->handle);
}

int oops_mutex_destroy(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexDestroy) return -1;
    return scePthreadMutexDestroy(&mutex->handle);
}

/* Condition variable implementation */

int oops_cond_init(oops_cond_t *cond, const char *name) {
    if (!cond || !scePthreadCondInit) return -1;
    const char *cond_name = name ? name : "oops_cond";
    return scePthreadCondInit(&cond->handle, NULL, cond_name);
}

int oops_cond_wait(oops_cond_t *cond, oops_mutex_t *mutex) {
    if (!cond || !mutex || !scePthreadCondWait) return -1;
    return scePthreadCondWait(&cond->handle, &mutex->handle);
}

int oops_cond_timedwait(oops_cond_t *cond, oops_mutex_t *mutex, uint32_t timeout_us) {
    if (!cond || !mutex || !scePthreadCondTimedwait) return -1;
    /* No fallback to the untimed wait: a caller who asked for a timeout is relying on coming
     * back, and a wait that never returns is not a degraded version of that. */
    return scePthreadCondTimedwait(&cond->handle, &mutex->handle, (uint64_t)timeout_us);
}

int oops_cond_signal(oops_cond_t *cond) {
    if (!cond || !scePthreadCondSignal) return -1;
    return scePthreadCondSignal(&cond->handle);
}

int oops_cond_broadcast(oops_cond_t *cond) {
    if (!cond || !scePthreadCondBroadcast) return -1;
    return scePthreadCondBroadcast(&cond->handle);
}

int oops_cond_destroy(oops_cond_t *cond) {
    if (!cond || !scePthreadCondDestroy) return -1;
    return scePthreadCondDestroy(&cond->handle);
}

/* Semaphore implementation */

int oops_sem_init(oops_sem_t *sem, const char *name, int initial_count, int max_count) {
    if (!sem || !sceKernelCreateSema) return -1;
    const char *sema_name = name ? name : "oops_sem";
    int32_t handle = 0;
    int rc = sceKernelCreateSema(&handle, sema_name, 0, initial_count, max_count, NULL);
    if (rc == 0) {
        sem->handle = handle;
        sem->max_count = max_count;
        return 0;
    }
    return rc;
}

int oops_sem_wait(oops_sem_t *sem, int count) {
    if (!sem || !sceKernelWaitSema || sem->handle <= 0) return -1;
    int n = (count <= 0) ? 1 : count;
    return sceKernelWaitSema(sem->handle, n, NULL);
}

int oops_sem_poll(oops_sem_t *sem, int count) {
    if (!sem || !sceKernelPollSema || sem->handle <= 0) return -1;
    int n = (count <= 0) ? 1 : count;
    return sceKernelPollSema(sem->handle, n);
}

int oops_sem_signal(oops_sem_t *sem, int count) {
    if (!sem || !sceKernelSignalSema || sem->handle <= 0) return -1;
    int n = (count <= 0) ? 1 : count;
    return sceKernelSignalSema(sem->handle, n);
}

int oops_sem_destroy(oops_sem_t *sem) {
    if (!sem || !sceKernelDeleteSema || sem->handle <= 0) return -1;
    int rc = sceKernelDeleteSema(sem->handle);
    sem->handle = -1;
    return rc;
}
