#include <unistd.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

int fsync(int fd)
{
#ifdef NVCACHE_BYPASS
    return musl_fsync(fd);
#else
    return nvcache_fsync(fd);
#endif
}

int musl_fsync(int fd) 
{
  return syscall_cp(SYS_fsync, fd);
}

