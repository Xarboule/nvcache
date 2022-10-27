#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "../nvlogcache/nvcache_musl_wrapp.h"
#include "stdio_impl.h"

static inline FILE *fopen_common(const char *restrict filename,
                                 const char *restrict mode, int *fd,
                                 int *flags) {
    /* Check for valid initial mode character */
    if (!strchr("rwa", *mode)) {
        errno = EINVAL;
        return 0;
    }

    /* Compute the flags to pass to open() */
    *flags = __fmodeflags(mode);

    *fd = sys_open(filename, *flags, 0666);
    if (*fd < 0) return 0;
    if (*flags & O_CLOEXEC) __syscall(SYS_fcntl, *fd, F_SETFD, FD_CLOEXEC);

    return __fdopen(*fd, mode);
}

FILE *fopen(const char *restrict filename, const char *restrict mode) {
    int fd = -1, flags;
    FILE *f = fopen_common(filename, mode, &fd, &flags);
    if (fd < 0) return 0;
    if (f) {
#ifdef NVCACHE_BYPASS
        return f;
#else
        return nvcache_fopen(filename, f, flags);
#endif
    }
    __syscall(SYS_close, fd);
    return 0;
}

FILE *musl_fopen(const char *restrict filename, const char *restrict mode) {
    int fd = -1, flags;
    FILE *f = fopen_common(filename, mode, &fd, &flags);
    if (fd < 0) return 0;
    if (f) {
        return f;
    }
    __syscall(SYS_close, fd);
    return 0;
}
