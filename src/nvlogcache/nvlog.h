#pragma once
#include <pthread.h>
#include <unistd.h>
#include "nvcache_types.h"
#include "nvcache_ram.h"


extern pthread_mutex_t nvcache_flush_mutex;

#ifndef NVCACHE_STATIC_CONF
// From dynamic config
extern int __enable_recover;
extern int __flush_thread;
extern long __max_batch_size;
extern long __min_batch_size;

#endif

//Static config

#ifdef NVCACHE_STATIC_CONF

#if defined BIG_LOG
#define LOG_SIZE 5000000
#elif defined MEDIUM_LOG
#define LOG_SIZE 50000
#elif defined SMALL_LOG
#define LOG_SIZE 500
#else
#error \
    "You must define either BIG or SMALL_LOG to choose the size of the NVlog."
#endif
#endif //Static conf



#define FLUSH_ALIGN ((uintptr_t)64)

// Value of nvlog.nvlog_tail meaning the log has been recovered but still needs
// to be reset. This state cannot be reached in a normal execution.
#define INVALID_STATE LOG_SIZE

#define PMEM_PATH "/dev/dax1.0"
//#define PMEM_PATH "/mnt/pmem1/pool"
//#define PMEM_PATH "/dev/pmem1" // NUMA node 1
//#define PMEM_PATH "/home/ubuntu/nvcache-ng.cache"

#ifdef __cplusplus
extern "C" {
#endif



atomic_size_t added_entries, flushed_entries, available_blocks;
  
void nvlog_init(void);
void nvlog_add_entry(int fd, size_t offset, const char *content, size_t count);
int nvlog_play_log_on_page(int fd, page *p);
void nvlog_final_flush(void);
void nvlog_flush_file(int fd);
void nvlog_set_file_table(int fd, const char *path, int flags, mode_t mode);
void nvlog_reset_file_table(int fd);
int nvlog_find_fd_by_path(const char *path);
  
#ifdef __cplusplus
}
#endif

//-----------------------------
