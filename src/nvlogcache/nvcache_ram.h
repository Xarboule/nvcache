#pragma once
#include "nvcache_types.h"
#include "radix-tree.h"
#include "nvcache_config.h"

#ifdef __cplusplus
extern "C" {
#endif
  


//-----------------------------

extern long __ram_cache_size;
  
ramcache_t ramcache;
  
void ramcache_init();
void ramcache_flush();
void ramcache_print();
void ramcache_lower_dirty_level(key k, int size, int fd);
void ramcache_greater_dirty_level(key k, int size, int fd);
int ramcache_lock_radix_page(int fd, off_t offset);
int ramcache_trylock_radix_pages(int fd, off_t offset, int size);
int ramcache_unlock_radix_page(int fd, off_t offset);
int ramcache_unlock_radix_pages(int fd, off_t offset, int size);
int ramcache_get_dirty_level(key k, int fd);
radixcache *ramcache_newradix(int fd);
int ramcache_exists(int fd);
ssize_t ramcache_pread(int fd, off_t offset, char *buf, size_t size);
ssize_t ramcache_pwrite(int fd, off_t offset, const char *buf, size_t size);
void ramcache_file_clean(int fd);

#define max(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a > _b ? _a : _b;      \
    })

#define min(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a > _b ? _b : _a;      \
    })

#ifdef __cplusplus
}
#endif
