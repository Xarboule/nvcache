#include "stdio_impl.h"
#include "aio_impl.h"
#include "../nvlogcache/nvcache_musl_wrapp.h"


static int dummy(int fd)
{
	return fd;
}

weak_alias(dummy, __aio_close);

int __stdio_close(FILE *f)
{
#ifdef NVCACHE_BYPASS
	return syscall(SYS_close, __aio_close(f->fd));
#else
    return nvcache_close(f->fd);
#endif
}
