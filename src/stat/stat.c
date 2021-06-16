#include <sys/stat.h>
#include <fcntl.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

int stat(const char *restrict path, struct stat *restrict buf)
{
    int ret;
#ifndef NVCACHE_BYPASS
    ret = nvcache_stat(path, buf);
#else
    ret = musl_stat(path, buf)
#endif
    return ret;
}


int musl_stat(const char *restrict path, struct stat *restrict buf)
{
#ifdef SYS_stat
	return syscall(SYS_stat, path, buf);
#else
	return syscall(SYS_fstatat, AT_FDCWD, path, buf, 0);
#endif
}

weak_alias(stat, stat64);
