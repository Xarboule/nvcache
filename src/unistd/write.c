#include <unistd.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

ssize_t write(int fd, const void *buf, size_t count)
{
#ifdef NVCACHE_BYPASS
    return musl_write(fd, buf, count);
#else
    return nvcache_write(fd, buf, count);
#endif
}

ssize_t musl_write(int fd, const void *buf, size_t count)
{
	return syscall_cp(SYS_write, fd, buf, count);
}
