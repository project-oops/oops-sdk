#ifndef OOPS_THREAD_H
#define OOPS_THREAD_H

#include <stdint.h>
#include <stddef.h>

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
oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *), void *arg, int priority, size_t stack_size);
int           oops_thread_join(oops_thread_t thread, void **out_retval);
int           oops_thread_detach(oops_thread_t thread);
void          oops_thread_yield(void);
oops_thread_t oops_thread_self(void);
int           oops_thread_equal(oops_thread_t t1, oops_thread_t t2);

/* Mutexes */
int  oops_mutex_init(oops_mutex_t *mutex, const char *name);
int  oops_mutex_lock(oops_mutex_t *mutex);
int  oops_mutex_trylock(oops_mutex_t *mutex);
int  oops_mutex_unlock(oops_mutex_t *mutex);
int  oops_mutex_destroy(oops_mutex_t *mutex);

/* Condition variables */
int  oops_cond_init(oops_cond_t *cond, const char *name);
int  oops_cond_wait(oops_cond_t *cond, oops_mutex_t *mutex);
int  oops_cond_timedwait(oops_cond_t *cond, oops_mutex_t *mutex, uint32_t timeout_us);
int  oops_cond_signal(oops_cond_t *cond);
int  oops_cond_broadcast(oops_cond_t *cond);
int  oops_cond_destroy(oops_cond_t *cond);

/* Semaphores */
int  oops_sem_init(oops_sem_t *sem, const char *name, int initial_count, int max_count);
int  oops_sem_wait(oops_sem_t *sem, int count);
int  oops_sem_poll(oops_sem_t *sem, int count);
int  oops_sem_signal(oops_sem_t *sem, int count);
int  oops_sem_destroy(oops_sem_t *sem);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_THREAD_H */
