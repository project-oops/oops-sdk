/*
 * <malloc_np.h> - non-portable heap queries.
 */
#ifndef OOPS_LIBC_MALLOC_NP_H
#define OOPS_LIBC_MALLOC_NP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

size_t malloc_usable_size(const void *p);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_MALLOC_NP_H */
