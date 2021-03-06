#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

int fstat(int fd, struct stat *st)
{
    int ret;
#ifndef NVCACHE_BYPASS
    ret = nvcache_fstat(fd, st);
#else
    ret = musl_fstat(fd, st);
#endif
    return ret;
}

int musl_fstat(int fd, struct stat *st)
{
	int ret = __syscall(SYS_fstat, fd, st);
	if (ret != -EBADF || __syscall(SYS_fcntl, fd, F_GETFD) < 0)
		return __syscall_ret(ret);

	char buf[15+3*sizeof(int)];
	__procfdname(buf, fd);
#ifdef SYS_stat
	return syscall(SYS_stat, buf, st);
#else
	return syscall(SYS_fstatat, AT_FDCWD, buf, st, 0);
#endif
}

weak_alias(fstat, fstat64);
