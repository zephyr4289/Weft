#ifndef WEFT_WIN_COMPAT_H
#define WEFT_WIN_COMPAT_H

#if defined(_WIN32) || defined(_WIN64)
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>

static inline int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!memptr || (alignment % sizeof(void*) != 0) || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void *ptr = _aligned_malloc(size, alignment);
    if (!ptr) return ENOMEM;
    *memptr = ptr;
    return 0;
}

#define free(p) _aligned_free(p)
#define calloc(n, s) _aligned_recalloc(NULL, (n), (s), 64)

#endif
#endif
