#include "oops/thread.h"

__attribute__((weak)) int scePthreadCreate(void *thread, const void *attr, void *(*entry)(void *), void *arg, const char *name);
__attribute__((weak)) int scePthreadJoin(void *thread, void **val);
__attribute__((weak)) int scePthreadMutexInit(void *mutex, const void *attr, const char *name);
__attribute__((weak)) int scePthreadMutexLock(void *mutex);
__attribute__((weak)) int scePthreadMutexUnlock(void *mutex);
__attribute__((weak)) int scePthreadMutexDestroy(void *mutex);

oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *), void *arg, int priority, size_t stack_size) {
    (void)priority;
    (void)stack_size;
    void *t = NULL;
    if (scePthreadCreate) {
        if (scePthreadCreate(&t, NULL, entry, arg, name) == 0) {
            return t;
        }
    }
    return NULL;
}

int oops_thread_join(oops_thread_t thread, void **out_retval) {
    if (!scePthreadJoin || !thread) return -1;
    return scePthreadJoin(thread, out_retval);
}

int oops_mutex_init(oops_mutex_t *mutex) {
    if (!mutex || !scePthreadMutexInit) return -1;
    return scePthreadMutexInit(&mutex->handle, NULL, "oops_mtx");
}

void oops_mutex_lock(oops_mutex_t *mutex) {
    if (mutex && scePthreadMutexLock) {
        (void)scePthreadMutexLock(&mutex->handle);
    }
}

void oops_mutex_unlock(oops_mutex_t *mutex) {
    if (mutex && scePthreadMutexUnlock) {
        (void)scePthreadMutexUnlock(&mutex->handle);
    }
}

void oops_mutex_destroy(oops_mutex_t *mutex) {
    if (mutex && scePthreadMutexDestroy) {
        (void)scePthreadMutexDestroy(&mutex->handle);
    }
}
