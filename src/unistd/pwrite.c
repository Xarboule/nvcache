#include <unistd.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

ssize_t pwrite(int fd, const void *buf, size_t size, off_t ofs)
{
#ifdef NVCACHE_BYPASS
    return musl_pwrite(fd, buf, size, ofs);
#else
    return nvcache_pwrite(fd, buf, size, ofs);
#endif
}

ssize_t musl_pwrite(int fd, const void *buf, size_t size, off_t ofs)
{
	return syscall_cp(SYS_pwrite, fd, buf, size, __SYSCALL_LL_PRW(ofs));
}

weak_alias(pwrite, pwrite64);
