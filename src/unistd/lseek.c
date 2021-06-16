#include <unistd.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

off_t lseek(int fd, off_t offset, int whence)
{
#ifdef NVCACHE_BYPASS
    return musl_lseek(fd, offset, whence);
#else
    return nvcache_lseek(fd, offset, whence);
#endif
}

off_t musl_lseek(int fd, off_t offset, int whence)
{
#ifdef SYS__llseek
	off_t result;
	return syscall(SYS__llseek, fd, offset>>32, offset, &result, whence) ? -1 : result;
#else
	return syscall(SYS_lseek, fd, offset, whence);
#endif
}

weak_alias(lseek, lseek64);
