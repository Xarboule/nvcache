#include <unistd.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

ssize_t pread(int fd, void *buf, size_t size, off_t ofs)
{
#ifdef NVCACHE_BYPASS
    return musl_pread(fd, buf, size, ofs);
#else
    return nvcache_pread(fd, buf, size, ofs);
#endif
}

ssize_t musl_pread(int fd, void *buf, size_t size, off_t ofs)
{
	return syscall_cp(SYS_pread, fd, buf, size, __SYSCALL_LL_PRW(ofs));
}

weak_alias(pread, pread64);
