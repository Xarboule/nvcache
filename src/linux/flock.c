#include <sys/file.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"


int flock(int fd, int op)
{
#ifdef NVCACHE_BYPASS
  return syscall(SYS_flock, fd, op);
#else
  return nvcache_flock(fd, op);
#endif
}


int musl_flock(int fd, int op)
{
	return syscall(SYS_flock, fd, op);
}
