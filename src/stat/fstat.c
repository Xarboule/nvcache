#define _BSD_SOURCE
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

int __fstat(int fd, struct stat *st)
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
	if (fd<0) return __syscall_ret(-EBADF);
	return __fstatat(fd, "", st, AT_EMPTY_PATH);
}

weak_alias(__fstat, fstat);
