#include "nvinfo.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include "nvcache_ram.h"
#include "nvlog.h"

// Needed for traces
//#define NVINFO_TIMESTAMP
//#define NVINFO_ALL_STATS
//#define STATS_ONLY
// ------

//#define NVINFO_THREAD_ID

#ifdef NVINFO_ALL_STATS
extern ramcache_t ramcache;
extern atomic_size_t available_blocks, added_entries, flushed_entries;
#endif

struct timespec tp;
clockid_t clk_id = CLOCK_MONOTONIC;

static uint8_t nvinfo_mask_ = ~0, mtx_init = 0;
static pthread_rwlock_t nvinfo_mutex = PTHREAD_RWLOCK_INITIALIZER;

//-----------------------------------------------
static void init_mutex() {
    pthread_rwlock_init(&nvinfo_mutex, NULL);
    mtx_init = 1;
}

//-----------------------------------------------
void printinfo(uint8_t level, const char *format, ...) {
#ifdef NVCACHE_DEBUG
#ifdef NVINFO_TIMESTAMP
    clock_gettime(clk_id, &tp);
    double tmstp = tp.tv_sec + (tp.tv_nsec*1e-9);
    printf("%lf | ", tmstp);
#endif
#ifdef NVINFO_THREAD_ID
    printf("TID %X | ", pthread_self());
#endif
	
#ifndef STATS_ONLY
    uint8_t mask;

    if (mtx_init == 0) init_mutex();
    pthread_rwlock_rdlock(&nvinfo_mutex);
    mask = nvinfo_mask_;
    pthread_rwlock_unlock(&nvinfo_mutex);

    if (level & mask) {
        FILE *file = level & NVCRIT ? stderr : stdout;
        if (level & NVINFO) {
            fprintf(file, GRN "INFO: " RST);
        } else if (level & NVWARN) {
            fprintf(file, YEL "WARNING: " RST);
        } else if (level & NVDEBG) {
            fprintf(file, BLD "DEBUG: " RST);
        } else if (level & NVLOG) {
            fprintf(file, BLU "LOG: " RST);
        } else if (level & NVCRIT) {
            fprintf(file, RED "CRITICAL: " RST);
        } else if (level & NVTRACE) {
	    fprintf(file, CYN "TRACE: " RST);
        }

        va_list arglist;
        va_start(arglist, format);
        vfprintf(file, format, arglist);
        va_end(arglist);
	
        fprintf(file, "\n");
    }
#endif //STATS_ONLY
#ifdef NVINFO_ALL_STATS
    if (level & NVTRACE){
      printf("H %d M %d Dm %d A %ld F %ld W %ld\n", ramcache.hits, ramcache.misses, ramcache.dirty, atomic_load(&added_entries), atomic_load(&flushed_entries), LOG_SIZE-atomic_load(&available_blocks));
    }
#endif
    
#endif
}

//-----------------------------------------------
void info_logmask(uint8_t l) {
#ifdef NVCACHE_DEBUG
    if (mtx_init == 0) init_mutex();
    pthread_rwlock_wrlock(&nvinfo_mutex);
    nvinfo_mask_ = l;
    pthread_rwlock_unlock(&nvinfo_mutex);
#endif
}
