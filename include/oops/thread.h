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

oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *), void *arg, int priority, size_t stack_size);
int oops_thread_join(oops_thread_t thread, void **out_retval);

int oops_mutex_init(oops_mutex_t *mutex);
void oops_mutex_lock(oops_mutex_t *mutex);
void oops_mutex_unlock(oops_mutex_t *mutex);
void oops_mutex_destroy(oops_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_THREAD_H */
