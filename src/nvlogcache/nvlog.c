#define _GNU_SOURCE
#include "nvlog.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "nvcache_ram.h"
#include "nvinfo.h"

#define TRACE_ADD 0x1
#define TRACE_DISK_WRITE 0x2
#define TRACE_PLAY_LOG 0x4
#define TRACE_FLUSH 0x8

static int tracemask = 0; //TRACE_FLUSH;//TRACE_DISK_WRITE | TRACE_PLAY_LOG | TRACE_FLUSH;

pthread_mutex_t nvcache_flush_mutex = PTHREAD_MUTEX_INITIALIZER;

#define INCR_IN_LOG(index) (index) = (index + 1) % LOG_SIZE
//-----------------------------------------------
//             NOT EXPORTED
//-----------------------------------------------
#ifdef NVCACHE_DEBUG
static int trace(int bit);
#else
#define trace(x) 0
#endif
static int mypmem_fd;
static nvlog_t *nvlog;
static pthread_t write_thread;
static struct timespec time_sleep;
static atomic_int wthread = 1;
static atomic_size_t nvlog_head = 0;
static int files_to_fsync[1024] = {0};

//-----------------------------------------------
static int recover_entry(log_entry_t *entry);
static void recover_nvlog(void);
static void *disk_write_loop();
static int nvlog_empty();
static void reset_nvram();
static void *memcpy_ntstore(void *_dest, void *_src, size_t n);
static void *memcpy_ntstore32(void *_dst, void *_src, size_t n);
static void memcpy_ntstore_nova(void *to, void *from);
static void flush_with_clwb(volatile char *content, size_t count);
static size_t flush_to_disk(log_entry_t *log_entry);
static void unsafe_log_flush(log_entry_t *log_entry);
static int __flush_batch();
static void flush_batch();
static int page_concerned(log_entry_t *log, page *ram, size_t *orig,
                          size_t *dest, size_t *size);
static int __nvlog_play_log_on_page(int fd, page *rampage);
static int log_to_socache(log_entry_t *log_entry);
static void mark_written(log_entry_t *log_entry);
static void free_log_entry(log_entry_t *log_entry);
//-----------------------------------------------
extern int is_writeonly(int fd);
extern int is_ramcached(int fd);
//-----------------------------------------------
#define clwb(val) flush_with_clwb((volatile char *)&val, sizeof(val))
//-----------------------------------------------

int recover_entry(log_entry_t *entry){
  if(!entry->fd>0) return 0;
  if(entry->already_written) return 0;
  if(!entry->committed) return 0;
  if(entry->waiting == INVALID_STATE) return 0;
  if(!nvlog->entries[entry->waiting].committed) return 0;
  flush_to_disk(entry);  // do not fsync after pwrite
  return 1;
  
}

//-----------------------------------------------
void recover_nvlog(){
  printinfo(NVINFO, RED "PMEM IS NOT EMPTY" RST);
  printinfo(NVINFO, RED "Starting recovery procedure...\n" RST);

  // Index is old fd. Value is new fd.
  int new_fd[MAX_FILES] = {0};
  long int recovered = 0;
  long int ignored = 0;
  size_t current_tail = nvlog->nvlog_tail;
  
  for(int i=0; i<MAX_FILES; i++){
    file_t file = nvlog->file_table[i];
    if(file.opened){
      new_fd[i]=musl_open(file.path, (file.flags&~O_CREAT), file.mode);
      if (new_fd[i]>0){
	printinfo(NVINFO, "-- Open %s        [" GRN "OK" RST "]", file.path);
      }
      else {
	printinfo(NVINFO, "-- Open %s        [" RED "FAILED" RST "]", file.path);
      }
    }
  }

  printinfo(NVINFO, "");
  printinfo(NVINFO, BLU"Flushing PMEM to disk..."RST);

  
  for(long int i=0; i<LOG_SIZE; i++){
    log_entry_t entry = nvlog->entries[current_tail];
    entry.fd = new_fd[entry.fd];
    int rec = recover_entry(&entry);
    if(rec==1){
      ++recovered;
    } else {
      ++ignored;
    } 
    INCR_IN_LOG(current_tail);
  }

  nvlog->nvlog_tail = INVALID_STATE;
  clwb(nvlog->nvlog_tail);
  PFENCE();
  
  printinfo(NVINFO, BLU"-- PMEM flushed. --"RST);
  printinfo(NVINFO, "Statistics on %d entries :");
  printinfo(NVINFO, "-----> Flushed : %ld entries", recovered);
  printinfo(NVINFO, "-----> Ignored : %ld entries", ignored);
  printinfo(NVINFO, "");
  printinfo(NVINFO, "Continuing with a clean log.");

  for(int i=0; i<MAX_FILES; i++){
    int fd = new_fd[i];
    if(fd>0){
      musl_fsync(fd);
      musl_close(fd);
    }
    nvlog->file_table[i].opened=0;
    clwb(nvlog->file_table[i]);

  }
  PFENCE();
}

//-----------------------------------------------
void nvlog_init() {
    available_blocks = LOG_SIZE;

    added_entries = 0;
    flushed_entries = 0;
    
    mypmem_fd = musl_open(PMEM_PATH, O_RDWR, 0);
    if (mypmem_fd == -1) {
        perror("Pmem");
    }

    nvlog = mmap((void *)0x7f625ef55000, sizeof(nvlog_t)+LOG_SIZE*sizeof(log_entry_t),
                 PROT_READ | PROT_WRITE, (MAP_SHARED_VALIDATE | MAP_SYNC), mypmem_fd, 0);
    if (nvlog == MAP_FAILED) {
        perror("Nvlog mmap");
    }

    // Trying to recover the data from NVRAM, if needed

    if(ENABLE_RECOVER){
      if (nvlog->nvlog_tail != INVALID_STATE) {
	recover_nvlog();
      }
    }
    
    reset_nvram();
    PFENCE();
    // NVRAM ready to be used
    nvlog->nvlog_tail = 0;
    clwb(nvlog->nvlog_tail);

    time_sleep.tv_sec = 1;
    time_sleep.tv_nsec = 0;

    if(FLUSH_THREAD){
    printinfo(NVINFO, MAG " -- Starting flushing thread --\n" RST);
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus); //10 for NUMA node 1 on castor
    pthread_attr_t tattr;
    struct sched_param param;
    pthread_attr_init(&tattr);
    pthread_attr_getschedparam(&tattr,&param);
    param.sched_priority = 31;
    pthread_attr_setschedparam(&tattr,&param);
    pthread_create(&write_thread, &tattr, disk_write_loop, NULL);
    pthread_setaffinity_np(write_thread, sizeof(cpus), &cpus);
    }
    else {
    printinfo(NVINFO, MAG
              "\n\t-- WARNING: FLUSHING THREAD DISABLED --\n"
              "\t(See nvlog.h)\n)" RST);
    }
}

//-----------------------------------------------
int nvlog_reserve_block(size_t *index) {
    size_t available_local = available_blocks;
    if (available_local <= 1 ||
        atomic_compare_exchange_strong(&available_blocks, &available_local,
                                       available_local - 1) == 0) {
      return 0;
    }
    
    *index = atomic_fetch_add(&nvlog_head, 1) % LOG_SIZE;
    ++added_entries;
    return 1;
}

//-----------------------------------------------
void nvlog_copy_to_log(int fd, log_entry_t *log_entry, size_t to_offset,
                       const char *content, size_t n) {
  assert(fd!=0);
    log_entry->fd = fd;  // TODO: use inodes ?
    log_entry->offset = to_offset;
    log_entry->size = n;
    log_entry->already_written = 0;
    if(n>256){
      memcpy_ntstore(log_entry->content, content, n);
    }
    else{
      memcpy(log_entry->content, content, n);
      flush_with_clwb(log_entry->content, n);
    }
      
    
    clwb(log_entry->fd);
    clwb(log_entry->offset);
    clwb(log_entry->size);
    clwb(log_entry->already_written);
    PFENCE();
    assert(log_entry->fd!=0);
    //flush_with_clwb(log_entry->content, n);
}

//-----------------------------------------------
void nvlog_add_entry(int fd, size_t offset, const char *content, size_t count) {
    if (!count) return;

    size_t my_index;
    size_t start_off = 0;
    log_entry_t *first_log = NULL;  // First log in case of multiple-log writes

    if (trace(TRACE_ADD)) {
        printinfo(NVTRACE,
                  MAG "NVlog : ADD ENTRY (fd=%d, offset=%ld, size=%ld)" RST, fd,
                  offset, count);
    }

    // Waiting to reserve a free block
    do {
#ifndef FLUSH_THREAD
        if (available_blocks == 0) {
            perror("Impasse: log is full, there is no thread to empty it");
            exit(-666);
        }
#endif

        if (!nvlog_reserve_block(&my_index)) {
            continue;
        }

        log_entry_t *log_entry = &nvlog->entries[my_index];
        size_t n = count < LOGENTRY_SIZE ? count : LOGENTRY_SIZE;
        nvlog_copy_to_log(fd, log_entry, offset + start_off,
                          content + start_off, n);

        if (first_log) {
            // Pre-committing the logs that are not the first
            //(Will not be persisted on disk while the first is not committed
            log_entry->waiting = first_log->waiting;
            clwb(log_entry->waiting);
            atomic_store_explicit(&log_entry->committed, 1,
                                  memory_order_release);
            clwb(log_entry->committed);
        } else {
            first_log = log_entry;
            log_entry->waiting = my_index;
            clwb(log_entry->waiting);
        }

#ifndef USE_LINUXCACHE
        // Write only files are not cached in RAM
        if (!is_writeonly(fd)) {
            ramcache_greater_dirty_level(offset + start_off, n, fd);
        }
#endif

        start_off += n;
        count -= n;
    } while (count);  // For >4096 logs

    PFENCE();
    // Commit the first log
    atomic_store_explicit(&first_log->committed, 1, memory_order_release);
    clwb(first_log->committed);
    PFENCE();
}

//----------------------------------------------
//        Set/Reset file in file_table
//----------------------------------------------

void nvlog_set_file_table(int fd, const char *path, int flags, mode_t mode){
  if(fd<0) return;
  strcpy(nvlog->file_table[fd].path, path);
  nvlog->file_table[fd].flags = flags;
  nvlog->file_table[fd].mode = mode;
  nvlog->file_table[fd].opened = 1;
  clwb(nvlog->file_table[fd]);
  PFENCE();
}

//----------------------------------------------

void nvlog_reset_file_table(int fd){

  nvlog->file_table[fd].opened=0;
  clwb(nvlog->file_table[fd].opened);
  PFENCE();
 
}

//----------------------------------------------
//          Find fd by path
//----------------------------------------------

int nvlog_find_fd_by_path(const char *path){
  for (int i=0; i<MAX_FILES; i++){
    if (!strcmp(path, nvlog->file_table[i].path)){
      return i;
    }
  }
  return -1;
}


//-----------------------------------------------
// page_concerned is called by nvlog_play_log_on_page to check whether
// *log should be applied on *ram. Being the case, the offsets *orig, *dest
// and *size are properly set.
//-----------------------------------------------
int page_concerned(log_entry_t *log, page *ram, size_t *orig, size_t *dest,
                   size_t *size) {
    if ((log->fd != ram->fd) || log->already_written || !log->committed) {
        return 0;
    }

    size_t ram_beg = ram->offset, ram_end = ram->offset + RAM_PAGE_SIZE,
           log_beg = log->offset, log_end = log->offset + log->size;

    if (ram_end <= log_beg || ram_beg >= log_end) {
        return 0;
    }

    *orig = ram_beg > log_beg ? ram_beg - log_beg : 0;
    *dest = ram_beg > log_beg ? 0 : log_beg - ram_beg;
    *size =
        ram_beg > log_beg ? log_end - ram_beg : min(ram_end, log_end) - log_beg;
    return 1;
}

//-----------------------------------------------
// nvcache_flush_mutex is used to serialize the following procedures:
// void flush_batch() @nvlog.c
// int nvlog_play_log_on_page(int fd, page *rampage) @nvlog.c
// void nvlog_flush_file(int fd) @nvlog.c
// page *cache_miss(int fd, off_t offset) @nvcache_ram.c
//-----------------------------------------------
int nvlog_play_log_on_page(int fd, page *rampage) {
    // ramcache_lock_page(fd, rampage->offset);
    int ret = __nvlog_play_log_on_page(fd, rampage);
    // ramcache_unlock_page(fd, rampage->offset);
    return ret;
}

//-----------------------------------------------
int __nvlog_play_log_on_page(int fd, page *rampage) {
    int dirty_level = ramcache_get_dirty_level(rampage->offset, fd);
    if (nvlog_empty()) {
        return 0;
    }

    if (trace(TRACE_PLAY_LOG)) {
        printinfo(NVTRACE,
                  MAG
                  "NVlog : Playing log on Page (fd=%d, off=%ld, size=%ld)" RST,
                  rampage->fd, rampage->offset, rampage->size);
        printinfo(NVTRACE, MAG "NVlog : == Dirty level = %d ==" RST,
                  dirty_level);
    }

    size_t c_disk_tail = nvlog->nvlog_tail;
    size_t c_whead = nvlog_head % LOG_SIZE;
    size_t ret = 0;

    while (c_disk_tail != c_whead) {
        log_entry_t *logentry = nvlog->entries + c_disk_tail;
        size_t origin, destination, size;
        if (page_concerned(logentry, rampage, &origin, &destination, &size)) {
            if (trace(TRACE_PLAY_LOG)) {
                printinfo(NVTRACE,
                          MAG
                          "NVlog : ---> Playing logentry LOG(off=%ld, "
                          "size=%ld) (logoff= %ld, ramoff= %ld, size= %ld)" RST,
                          logentry->offset, logentry->size, origin, destination,
                          size);
                printinfo(
                    NVTRACE,
                    RED
                    "NVlog play => c_disk_tail = %8ld | c_whead = %8ld\n" RST,
                    c_disk_tail, c_whead);
            }
            memcpy(rampage->content + destination, logentry->content + origin,
                   size);
            rampage->size = max(rampage->size, destination + size);
            ++ret;
        }
        INCR_IN_LOG(c_disk_tail);
    }
    if (trace(TRACE_PLAY_LOG)) {
        printinfo(NVTRACE,
                  MAG "=== Logs played : %d ===\n=== Dirty Level : %d ===", ret,
                  dirty_level);
    }
    return ret;
}

//-----------------------------------------------
void nvlog_final_flush(void) {
    wthread = 0;  // Stops the next iteration of write thread
    tracemask = 0;
#ifdef FLUSH_THREAD
    pthread_join(write_thread, NULL);
#endif

    printinfo(NVINFO, BLU "\t -- Final flush --" RST);
    printinfo(NVINFO, BLD "\tAdded: %lu\n\tFlushed: %lu" RST, added_entries,
              flushed_entries);

#ifndef FAST_FLUSH
    size_t final_flush = 0;
    while (!nvlog_empty()) {
        final_flush += __flush_batch();
    }
    printinfo(NVINFO, BLD "\tFinal flush: %lu" RST, final_flush);
    sync();
#else
    printinfo(NVINFO, RED "\tFAST FLUSH ENABLED: NOT FLUSHING THE CACHE\n" RST);
#endif

    //Set clean exit flag
    nvlog->nvlog_tail = INVALID_STATE;
    clwb(nvlog->nvlog_tail);
    PFENCE();
}

//-----------------------------------------------
void reset_nvram() {
    for (int i = LOG_SIZE-1; i >= 0; --i) {
        nvlog->entries[i].fd = -1;
        //nvlog->entries[i].offset = 0;
        //nvlog->entries[i].size = 0;
        nvlog->entries[i].waiting = INVALID_STATE;
        nvlog->entries[i].committed = 0;
        nvlog->entries[i].already_written = 0;
    }
}

//-----------------------------------------------
int nvlog_empty() { return (available_blocks >= LOG_SIZE); }

//-----------------------------------------------
//          THREAD
//-----------------------------------------------
void *disk_write_loop() {
    while (wthread) {
      if (LOG_SIZE - available_blocks > MIN_BATCH_SIZE) {
	flush_batch();
      } else {
	//nanosleep(&time_sleep, NULL);
      }
    }
#ifdef FLUSH_THREAD
    printinfo(NVINFO, MAG "\t -- Flushing thread ended --" RST);
#else
    printinfo(NVINFO, MAG "\tNo flush thread to end." RST);
#endif
    return NULL;
}

//-----------------------------------------------
// nvcache_flush_mutex is used to serialize the following procedures:
// void flush_batch() @nvlog.c
// int nvlog_play_log_on_page(int fd, page *rampage) @nvlog.c
// void nvlog_flush_file(int fd) @nvlog.c
// page *cache_miss(int fd, off_t offset) @nvcache_ram.c
//-----------------------------------------------
void nvlog_flush_file(int fd) {
    pthread_mutex_lock(&nvcache_flush_mutex);
    size_t c_tail = (nvlog->nvlog_tail % LOG_SIZE), entries = 0;
    size_t c_head = (nvlog_head % LOG_SIZE);
    if (trace(TRACE_FLUSH)) {
        printinfo(NVTRACE, "\t--- Flushing file %d ---", fd);
    }

    for (c_tail = nvlog->nvlog_tail % LOG_SIZE; c_tail != c_head;
         c_tail = (c_tail + 1) % LOG_SIZE) {
        // printf("Flushing a log : nvlog_tail = %ld   nvlog_head = %ld \n",
        // c_tail, c_head);
        log_entry_t *l = &nvlog->entries[c_tail];
        if (l->fd == fd) {
            // ramcache_lock_page(fd, l->offset);
            if (flush_to_disk(l)) {  // do not fsync after pwrite
                ++entries;
            }
        }
    }

    musl_fsync(fd);  // only one fsync

    // mark as written
    for (c_tail = nvlog->nvlog_tail % LOG_SIZE; c_tail != c_head;
         c_tail = (c_tail + 1) % LOG_SIZE) {
        log_entry_t *l = &nvlog->entries[c_tail];
        if (l->fd == fd) {
            l->already_written = 1;
            clwb(l->already_written);
            // ramcache_unlock_page(fd, l->offset);
        }
    }
    PFENCE();
    pthread_mutex_unlock(&nvcache_flush_mutex);

#ifndef FLUSH_THREAD
    printinfo(NVINFO, "\t--- Finished flushing file %d --- Total entries: %lu",
              fd, entries);
#endif
}

//-----------------------------------------------
void mark_written(log_entry_t *log_entry) {
#ifndef USE_LINUXCACHE
    // Write only files are not cached in RAM
    if (is_ramcached(log_entry->fd) && !is_writeonly(log_entry->fd)) {
        ramcache_lower_dirty_level(log_entry->offset, log_entry->size,
                                   log_entry->fd);
    }
#endif

    log_entry->already_written = 1;
    clwb(log_entry->already_written);
    PFENCE();
}

//-----------------------------------------------
void free_log_entry(log_entry_t *log_entry) {
    atomic_store_explicit(&log_entry->committed, 0, memory_order_release);
    clwb(log_entry->committed);

    log_entry->waiting = INVALID_STATE;
    clwb(log_entry->waiting);

    nvlog->nvlog_tail = (nvlog->nvlog_tail + 1) % LOG_SIZE;
    clwb(nvlog->nvlog_tail);
    PFENCE();

    atomic_fetch_add(&available_blocks, 1);
}

//-----------------------------------------------
int log_to_socache(log_entry_t *log_entry) {
    int ret = musl_pwrite(log_entry->fd, log_entry->content, log_entry->size,
                          log_entry->offset);
    if (ret != log_entry->size) {
        printinfo(NVCRIT, "Run to the hills size=%lu written=%d",
                  log_entry->size, ret);
	perror("Flushing to disk");
        return 0;
    } else {
        ++flushed_entries;
        return ret;
    }
}

//-----------------------------------------------
size_t flush_to_disk(log_entry_t *log_entry) {
    if (log_entry->already_written) {
        return 0;
    }

    if (trace(TRACE_DISK_WRITE)) {
        printinfo(NVTRACE,
                  MAG
                  "NVlog : Write to disk : Entry (fd=%d, off=%ld, size=%ld)",
                  log_entry->fd, log_entry->offset, log_entry->size);
    }

    return log_to_socache(log_entry);
}

//-----------------------------------------------
// Batch code
//-----------------------------------------------

//-----------------------------------------------
// nvcache_flush_mutex is used to serialize the following procedures:
// void flush_batch() @nvlog.c
// int nvlog_play_log_on_page(int fd, page *rampage) @nvlog.c
// void nvlog_flush_file(int fd) @nvlog.c
// page *cache_miss(int fd, off_t offset) @nvcache_ram.c
//-----------------------------------------------
void flush_batch() {
    static int failcount = 0, times = 0;
    pthread_mutex_lock(&nvcache_flush_mutex);
    if (__flush_batch()) {
        failcount = 0;
    } else {
        ++failcount;
    }
    pthread_mutex_unlock(&nvcache_flush_mutex);
    if (failcount >= 1e7) {
        failcount /= 1e6;
        printinfo(
            NVCRIT,
            "Too many (%dM) unsuccessful consecutive attempts to flush batch",
            failcount * ++times);
        failcount = 0;
    }
}

//-----------------------------------------------
// committed == 1 guarantees that a write (which potentially touches multiple
// pages) is finished.
//-----------------------------------------------
int is_log_batchable(log_entry_t *log_entry) {
    if (log_entry->committed) {
        // Return 1 when trylock succeed
        return 1;
    }
    return 0;
}

//-----------------------------------------------
int __flush_batch() {
    int batch_size = 0;
    if (nvlog_empty()) {  // Log empty
        return batch_size;
    }

    log_entry_t *log_entry = &nvlog->entries[(nvlog->nvlog_tail)];

    while ((batch_size < MAX_BATCH_SIZE) && is_log_batchable(log_entry)) {
        if (!log_entry->already_written) {
            int ret = ramcache_trylock_radix_pages(log_entry->fd, log_entry->offset,
                                             log_entry->size);
            if (ret) {
                break;
            }
            flush_to_disk(log_entry);  // do not fsync after pwrite
            files_to_fsync[log_entry->fd] = 1;
        }

        ++batch_size;

        log_entry =
            &nvlog->entries[(nvlog->nvlog_tail + batch_size) % LOG_SIZE];
    }

    if (batch_size == 0) {  // i.e. first is not commited or page locked
        return batch_size;
    }

    if (trace(TRACE_FLUSH)) {
        printinfo(NVTRACE, RED " FLUSH BATCH : WRITING %d LOG ENTRIES" RST,
                  batch_size);
    }

    for (int i = 0; i < 1024; i++) {
        if (files_to_fsync[i]) {
            musl_fsync(i);  // One fsync to rule them all !
            files_to_fsync[i] = 0;
        }
    }

    for (int i = 0; i < batch_size; i++) {
        log_entry_t *l = &nvlog->entries[nvlog->nvlog_tail];
        int fd = l->fd;
        off_t off = l->offset;
        int size = l->size;
        int already_written = l->already_written;
        mark_written(l);
        free_log_entry(l);
        if (!already_written) {
            int lockret = ramcache_unlock_radix_pages(fd, off, size);
        }
    }
    return batch_size;
}

//-----------------------------------------------
// End batch code
//-----------------------------------------------

//-----------------------------------------------
// From PMDK (flush.h)
void flush_with_clwb(volatile char *content, size_t count) {
    uintptr_t uptr;

    for (uptr = (uintptr_t)content & ~(FLUSH_ALIGN - 1);
         uptr < (uintptr_t)content + count; uptr += FLUSH_ALIGN) {
        PWB((char *)uptr);
    }
}

//-----------------------------------------------
inline void ntstore(void* _dst, void* _src) {
  unsigned long dst = (unsigned long) _dst;
  unsigned long src = (unsigned long) _src;
  __asm__ __volatile__(
      "movq    (%0), %%r8\n"
      "movnti  %%r8,   (%1)\n"
      :: "r" (src), "r" (dst)
      : "memory", "r8");
}



//-----------------------------------------------
inline void *memcpy_ntstore(void *_dest, void *_src, size_t n){

  unsigned long dst = (unsigned long) _dest;
  unsigned long src = (unsigned long) _src;
  
  for(int i=0; i<=(n/8); i++){

    __asm__(
      "movq    (%0), %%r8\n"
      "movnti  %%r8,   (%1)\n"
      :: "r" (src), "r" (dst)
      : "memory", "r8");
    dst += 8;
    src += 8;
  }

  return _dest;
}

//-----------------------------------------------
inline void *memcpy_ntstore32(void *_dst, void *_src, size_t n){
    unsigned long dst = (unsigned long) _dst;
    unsigned long src = (unsigned long) _src;

    for(int i=0; i<=(n/32)+1; i++){
      __asm__(
	   "   prefetchnta (%0)\n"
	   "   prefetchnta 64(%0)\n"
	   "   prefetchnta 128(%0)\n"
	   "   prefetchnta 192(%0)\n"
	   "   prefetchnta 256(%0)\n"
	   "   prefetchnta 320(%0)\n"
	   : : "r" (src) );
      __asm__(
	  "movq  (%0), %%mm0\n"
	  "movq 8(%0), %%mm1\n"
	  "movq 16(%0), %%mm2\n"
	  "movq 24(%0), %%mm3\n"
	  "movq 32(%0), %%mm4\n"
	  "movq 40(%0), %%mm5\n"
	  "movq 48(%0), %%mm6\n"
	  "movq 56(%0), %%mm7\n"
	  "movntq %%mm0, (%1)\n"
	  "movntq %%mm1, 8(%1)\n"
	  "movntq %%mm2, 16(%1)\n"
	  "movntq %%mm3, 24(%1)\n"
	  "movntq %%mm4, 32(%1)\n"
	  "movntq %%mm5, 40(%1)\n"
	  "movntq %%mm6, 48(%1)\n"
	  "movntq %%mm7, 56(%1)\n"
	  :: "r" (src), "r" (dst)
	  : "memory");

      dst += 32;
      src += 32;
    }

    return _dst;
}

//-----------------------------------------------
inline void memcpy_ntstore_nova(void *_to, void *_from){

  unsigned long to = (unsigned long) _to;
  unsigned long from = (unsigned long) _from;

  int i;
  
  __asm__ __volatile__(
		       "1: prefetch (%0)\n"
		       "   prefetch 64(%0)\n"
		       "   prefetch 128(%0)\n"
		       "   prefetch 192(%0)\n"
		       "   prefetch 256(%0)\n"
		       "2:  \n"
		       ".section .fixup, \"ax\"\n"
		       "3: movw $0x1AEB, 1b\n"	/* jmp on 26 bytes */
		       "   jmp 2b\n"
		       ".previous\n"
		       :: "r" (from));

  for (i = 0; i < (4096-320)/64; i++) {
    __asm__ __volatile__ (
			  "1: prefetch 320(%0)\n"
			  "2: movq (%0), %%mm0\n"
			  "   movntq %%mm0, (%1)\n"
			  "   movq 8(%0), %%mm1\n"
			  "   movntq %%mm1, 8(%1)\n"
			  "   movq 16(%0), %%mm2\n"
			  "   movntq %%mm2, 16(%1)\n"
			  "   movq 24(%0), %%mm3\n"
			  "   movntq %%mm3, 24(%1)\n"
			  "   movq 32(%0), %%mm4\n"
			  "   movntq %%mm4, 32(%1)\n"
			  "   movq 40(%0), %%mm5\n"
			  "   movntq %%mm5, 40(%1)\n"
			  "   movq 48(%0), %%mm6\n"
			  "   movntq %%mm6, 48(%1)\n"
			  "   movq 56(%0), %%mm7\n"
			  "   movntq %%mm7, 56(%1)\n"
			  ".section .fixup, \"ax\"\n"
			  "3: movw $0x05EB, 1b\n"	/* jmp on 5 bytes */
			  "   jmp 2b\n"
			  ".previous\n"
			  :: "r" (from), "r" (to)
			  : "memory");

    from += 64;
    to += 64;
  }

  for (i = (4096-320)/64; i < 4096/64; i++) {
    __asm__ __volatile__ (
			  "2: movq (%0), %%mm0\n"
			  "   movntq %%mm0, (%1)\n"
			  "   movq 8(%0), %%mm1\n"
			  "   movntq %%mm1, 8(%1)\n"
			  "   movq 16(%0), %%mm2\n"
			  "   movntq %%mm2, 16(%1)\n"
			  "   movq 24(%0), %%mm3\n"
			  "   movntq %%mm3, 24(%1)\n"
			  "   movq 32(%0), %%mm4\n"
			  "   movntq %%mm4, 32(%1)\n"
			  "   movq 40(%0), %%mm5\n"
			  "   movntq %%mm5, 40(%1)\n"
			  "   movq 48(%0), %%mm6\n"
			  "   movntq %%mm6, 48(%1)\n"
			  "   movq 56(%0), %%mm7\n"
			  "   movntq %%mm7, 56(%1)\n"
			  : : "r" (from), "r" (to) : "memory");
    from += 64;
    to += 64;
  }

}


    
//-----------------------------------------------
// For debug only
void gdb_print_log() {
    printinfo(NVTRACE,
              "+---------+----+----------+--------+--------+---------+---------"
              "--------+");
    size_t c_disk_tail = nvlog->nvlog_tail;
    printinfo(NVTRACE,
              "|  Index  | Fd |  Offset  |  Size  | Commit | Waiting | Already "
              "written |");
    printinfo(NVTRACE,
              "+---------+----+----------+--------+--------+---------+---------"
              "--------+");
    while (!nvlog_empty() && (c_disk_tail != nvlog_head % LOG_SIZE)) {
        log_entry_t *entry = &nvlog->entries[c_disk_tail];

        printinfo(NVTRACE, "|%9ld|%4d|%10ld|%8d|%8d|%9d|%17d|", c_disk_tail,
                  entry->fd, entry->offset, entry->size, entry->committed,
                  entry->waiting, entry->already_written);
        c_disk_tail = (c_disk_tail + 1) % LOG_SIZE;
    }

    printinfo(NVTRACE,
              "+---------+----+----------+--------+--------+---------+---------"
              "--------+");
}

void gdb_print_concerned_logs(int fd, off_t offset) {
    size_t vorig, vdest, vsize;
    size_t *orig = &vorig;
    size_t *dest = &vdest;
    size_t *size = &vsize;
    page vram;
    page *ram = &vram;
    ram->fd = fd;
    ram->offset = offset;
    printinfo(NVTRACE,
              "+---------+----+----------+--------+--------+---------+---------"
              "--------+");
    size_t c_disk_tail = nvlog->nvlog_tail;
    printinfo(NVTRACE,
              "|  Index  | Fd |  Offset  |  Size  | Commit | Waiting | Already "
              "written |");
    printinfo(NVTRACE,
              "+---------+----+----------+--------+--------+---------+---------"
              "--------+");
    while (!nvlog_empty() && (c_disk_tail != nvlog_head % LOG_SIZE)) {
        log_entry_t *entry = &nvlog->entries[c_disk_tail];

        if (page_concerned(entry, ram, orig, dest, size)) {
            printinfo(NVTRACE, "|%9ld|%4d|%10ld|%8d|%8d|%9d|%17d|", c_disk_tail,
                      entry->fd, entry->offset, entry->size, entry->committed,
                      entry->waiting, entry->already_written);
        }
        c_disk_tail = (c_disk_tail + 1) % LOG_SIZE;
    }

    printinfo(NVTRACE,
              "+---------+----+----------+--------+--------+---------+---------"
              "--------+");
}

//-----------------------------------------------
//             AUXILIARY
//-----------------------------------------------
#ifdef NVCACHE_DEBUG
int trace(int bit) { return tracemask & bit; }
#endif
