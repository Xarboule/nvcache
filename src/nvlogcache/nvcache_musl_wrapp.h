#pragma once

#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>

// #define USE_LINUXCACHE Do not use this
// If you want Linux cache, do instead
// $ make LINUXCACHE=1

#ifdef __cplusplus
extern "C" {
#endif

//#define DIRECT_IO

//#define IOSTATS
  
  
  
int nvcache_open(const char *filename, int flags, mode_t mode);
int nvcache_close(int fd);
off_t nvcache_lseek(int fd, off_t offset, int whence);
ssize_t nvcache_read(int fd, void *buf, size_t count);
ssize_t nvcache_pread(int fd, void *buf, size_t size, off_t ofs);
ssize_t nvcache_write(int fd, const void *buf, size_t count);
ssize_t nvcache_pwrite(int fd, const void *buf, size_t size, off_t ofs);
int nvcache_stat(const char *pathname, struct stat *statbuf);

int nvcache_fsync(int fd);

FILE *nvcache_fopen(const char *restrict filename, FILE *f, int flags);
size_t nvcache_fread(FILE *f, unsigned char *buf, size_t len);
size_t nvcache_fwrite(FILE *f, const unsigned char *buf, size_t len);
size_t nvcache_stdio_write(FILE *f, const unsigned char *buf, size_t len);
int nvcache_fseeko(FILE *f, off_t off, int whence);
int nvcache_fstat(int fd, struct stat *st);
int nvcache_flock(int fd, int op);
  
#ifdef __cplusplus
}
#endif
