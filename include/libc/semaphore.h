/*
 * <semaphore.h> - POSIX unnamed semaphores, over the kernel semaphores in
 * <oops/thread.h>.
 *
 * These are real semaphores, not no-ops: a `sem_wait` that returned at once would let
 * the waiter run on data the poster has not finished writing (OpenAL Soft's mixer
 * thread sleeps on one until the device asks for more samples).
 *
 * Header-only, because every operation is one call. `sem_t` is `oops_sem_t`, so it can
 * live wherever the program puts it, a class member included.
 *
 * Unnamed semaphores only: `sem_open` and friends name a semaphore in a namespace
 * shared between processes, and a payload is one process. `pshared` is accepted and
 * ignored for the same reason.
 */
#ifndef OOPS_LIBC_SEMAPHORE_H
#define OOPS_LIBC_SEMAPHORE_H

#include <errno.h>
#include <oops/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef oops_sem_t sem_t;

/* The kernel takes a maximum count at creation where POSIX has none. INT32_MAX is the
 * largest the `int` parameter carries, and is what a caller that never names a ceiling
 * means. */
#define SEM_VALUE_MAX 0x7fffffff

static inline int sem_init(sem_t *sem, int pshared, unsigned int value) {
    (void)pshared;
    if (value > (unsigned int)SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (oops_sem_init(sem, "posix_sem", (int)value, SEM_VALUE_MAX) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static inline int sem_destroy(sem_t *sem) {
    if (oops_sem_destroy(sem) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static inline int sem_post(sem_t *sem) {
    if (oops_sem_signal(sem, 1) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static inline int sem_wait(sem_t *sem) {
    if (oops_sem_wait(sem, 1) != 0) {
        errno = EINTR;
        return -1;
    }
    return 0;
}

/* `EAGAIN` is the expected failure: it is how a caller learns the count was zero. The
 * kernel's poll does not distinguish zero from a bad handle, so both read as EAGAIN; a
 * bad handle is one that failed `sem_init` or was destroyed, which POSIX leaves
 * undefined. */
static inline int sem_trywait(sem_t *sem) {
    if (oops_sem_poll(sem, 1) != 0) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_SEMAPHORE_H */
