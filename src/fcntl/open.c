#include <fcntl.h>
#include <stdarg.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

int open(const char *filename, int flags, ...)
{
	mode_t mode = 0;

	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

#ifdef NVCACHE_BYPASS
    return musl_open(filename, flags, mode);
#else
    return nvcache_open(filename, flags, mode);
#endif
}

int musl_open(const char *filename, int flags, mode_t mode)
{
	int fd = __sys_open_cp(filename, flags, mode);
	if (fd>=0 && (flags & O_CLOEXEC))
		__syscall(SYS_fcntl, fd, F_SETFD, FD_CLOEXEC);

	return __syscall_ret(fd);
}

weak_alias(open, open64);
