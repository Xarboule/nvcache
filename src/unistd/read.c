#include <unistd.h>
#include "syscall.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"

ssize_t read(int fd, void *buf, size_t count)
{ 
#ifdef NVCACHE_BYPASS
    return musl_read(fd, buf, count); 
#else 
    return nvcache_read(fd, buf, count);
#endif 
} 

ssize_t musl_read(int fd, void *buf, size_t count)
{
	return syscall_cp(SYS_read, fd, buf, count);
}
