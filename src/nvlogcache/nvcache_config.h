#pragma once
#include <stdlib.h>


//#define NVCACHE_STATIC_CONF



#ifndef NVCACHE_STATIC_CONF
//=================DYNAMIC CONFIG=========================
extern long __ram_cache_size;

extern long __log_size;

extern int __enable_recover;
extern int __flush_thread;

extern long __max_batch_size;
extern long __min_batch_size;

//------------------------------
//        RAM CACHE
//------------------------------

#define RAM_CACHE_SIZE __ram_cache_size  // pages

#ifndef NVCACHE_RAM_PAGE_SIZE_K
#define RAM_PAGE_SIZE 4096 // Bytes
#else
#define RAM_PAGE_SIZE (NVCACHE_RAM_PAGE_SIZE_K*1024L)
#endif

#define MAX_FILES 1024 // Must be set by a define.
//------------------------------
//          NVLOG
//------------------------------

#define LOG_SIZE __log_size

#ifndef NVCACHE_ENTRY_SIZE_K
#define LOGENTRY_SIZE 4096 // Must be set by a define.
#else
#define LOGENTRY_SIZE (NVCACHE_ENTRY_SIZE_K*1024L)
#endif

// PWB must be defined at compile time.
#define PWB_IS_CLWB
//#define PWB_IS_CLFLUSHOPT
//#define PWB_IS_NOP

#define ENABLE_RECOVER __enable_recover

#define FLUSH_THREAD __flush_thread

#define MAX_BATCH_SIZE __max_batch_size
#define MIN_BATCH_SIZE __min_batch_size



//=================STATIC CONFIG=========================
#else



//------------------------------
//        RAM CACHE
//------------------------------

#define RAM_CACHE_SIZE 50000  // pages
#define RAM_PAGE_SIZE 4096     // bytes
#define MAX_FILES 1024


//------------------------------
//          NVLOG
//------------------------------

#define BIG_LOG
//#define MEDIUM_LOG
//#define SMALL_LOG

#define PWB_IS_CLWB
//#define PWB_IS_CLFLUSHOPT
//#define PWB_IS_NOP

//#define ENABLE_RECOVER

#define FLUSH_THREAD

#define MAX_BATCH_SIZE 120000
#define MIN_BATCH_SIZE 1

#define LOGENTRY_SIZE 8192  // One complete page at maximum
#define MAX_FD 50           // Max number of fd used simultaneously

#endif //NVCACHE_STATIC_CONF


// Thank you Pedro for this part :-) (from OneFile)
//-------------------------------
#if defined(PWB_IS_CLFLUSH)

#define PWB(addr)                               \
    __asm__ volatile("clflush (%0)" ::"r"(addr) \
                     : "memory")  // Broadwell only works with this.
#define PFENCE() \
    {}  // No ordering fences needed for CLFLUSH (section 7.4.6 of Intel manual)
#define PSYNC() \
    {}  // For durability it's not obvious, but CLFLUSH seems to be enough, and
        // PMDK uses the same approach

#elif defined(PWB_IS_CLWB)
/* Use this for CPUs that support clwb, such as the SkyLake SP series (c5
 * compute intensive instances in AWS are an example of it) */
#define PWB(addr)                 \
    __asm__ volatile(             \
        ".byte 0x66; xsaveopt %0" \
        : "+m"(*(volatile char *)(addr)))  // clwb() only for Ice Lake onwards
#define PFENCE() __asm__ volatile("sfence" : : : "memory")
#define PSYNC() __asm__ volatile("sfence" : : : "memory")

#elif defined(PWB_IS_NOP)
/* pwbs are not needed for shared memory persistency (i.e. persistency across
 * process failure) */
#define PWB(addr) \
    {}
#define PFENCE() __asm__ volatile("sfence" : : : "memory")
#define PSYNC() __asm__ volatile("sfence" : : : "memory")

#elif defined(PWB_IS_CLFLUSHOPT)
/* Use this for CPUs that support clflushopt, which is most recent x86 */
#define PWB(addr)                \
    __asm__ volatile(            \
        ".byte 0x66; clflush %0" \
        : "+m"(*(volatile char *)(addr)))  // clflushopt (Kaby Lake)
#define PFENCE() __asm__ volatile("sfence" : : : "memory")
#define PSYNC() __asm__ volatile("sfence" : : : "memory")
#else
#error \
    "You must define what PWB is. Choose PWB_IS_CLFLUSHOPT if you don't know what your CPU is capable of"

#endif

/*#define ntstore(_dst, _src)		    \
  unsigned long dst = (unsigned long) _dst; \
  unsigned long src = (unsigned long) _src; \
  asm("movq    (%0), %%r8\n" \
      "movnti  %%r8,   (%1)\n" \
      :: "r" (src), "r" (dst) \
      : "memory", "r8");*/



void nvcache_config_init(void);  


