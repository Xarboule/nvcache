#include "nvcache_ram.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal_profile.h"
#include "nvinfo.h"
#include "nvlog.h"
#include "radix-tree.h"


#define TRACE_ADD 0x1
#define TRACE_EVICT 0x2
#define TRACE_MISS 0x4
#define TRACE_DIRTY_MISS 0x8
#define TRACE_CLEAN 0x10
#define TRACE_LOCK 0x20
#define TRACE_UNLOCK 0x30
#define TRACE_TRYLOCK 0x40

static int tracemask = 0;
  //TRACE_LOCK | TRACE_UNLOCK | TRACE_TRYLOCK | TRACE_ADD | TRACE_EVICT | TRACE_MISS | TRACE_DIRTY_MISS;

//-----------------------------------------------
//           NOT EXPORTED
//-----------------------------------------------
static int trace(int bit);
static pthread_mutex_t lru_lock;
static void pagetable_init();
static void page_init(page *p, page *prev, page *next);
static page *get_page(int fd, off_t offset);
static page *__get_page(int fd, off_t offset);
static page *dirty_miss(page *p, int fd, size_t offset);
static page *cache_miss(int fd, off_t offset);
static page *__cache_miss(int fd, off_t offset);
static page *add_page(off_t offset, ssize_t size, char *buf, radixcache *cache);
static page *__add_page(off_t offset, ssize_t size, char *buf, radixcache *cache);
static page *rm_last_page();
static size_t page_read(int fd, off_t offset, char *buf, size_t nbyte);
static size_t page_write(int fd, off_t offset, const char *buf, size_t nbyte);
static off_t page_busy(off_t offset);
static off_t page_free(off_t offset);
static off_t page_base(off_t offset);

//-----------------------------------------------
//               INIT
//-----------------------------------------------
void ramcache_init() {
    ramcache.page_table=malloc(RAM_CACHE_SIZE*sizeof(page));
    radix_init_nodes(RAM_CACHE_SIZE);
    pagetable_init();

    pthread_mutex_init(&lru_lock, NULL);


    ramcache.first = &ramcache.page_table[0];
    ramcache.last = &ramcache.page_table[RAM_CACHE_SIZE - 1];
    ramcache.hits = 0;
    ramcache.misses = 0;
    ramcache.overlaps = 0;
    printinfo(NVINFO,
              GRN
              "\t--- Radix Cache ---\n"
              "\tCache size : %d MB\n"
              "\t--- --- --- --- ---" RST,
              RAM_CACHE_SIZE * RAM_PAGE_SIZE / 1024 / 1024);
    for (int i = 0; i < 1024; i++) {
      ramcache.cache_table[i] = NULL;
    }
    printinfo(NVINFO, GRN
              "\tCache table initiated\n"
              "\t-------------------------------------" RST);
}

//-----------------------------------------------
void pagetable_init() {
    page_init(&ramcache.page_table[0], NULL, &ramcache.page_table[1]);

    for (int i = 1; i < RAM_CACHE_SIZE - 1; i++) {
        page_init(&ramcache.page_table[i], &ramcache.page_table[i - 1],
                  &ramcache.page_table[i + 1]);
    }

    page_init(&ramcache.page_table[RAM_CACHE_SIZE - 1],
              &ramcache.page_table[RAM_CACHE_SIZE - 2], NULL);
}

//-----------------------------------------------
void page_init(page *p, page *prev, page *next) {
    p->next = next;
    p->previous = prev;
    p->fd = -1;
    p->offset = 0;
    p->state = CLEAN;
    p->size = 0;
    p->touched = 0;
    p->radix_parent = NULL;
}

//-----------------------------------------------
//                AUXILIARY
//-----------------------------------------------
off_t page_busy(off_t offset) { return offset % RAM_PAGE_SIZE; }

//-----------------------------------------------
off_t page_free(off_t offset) { return RAM_PAGE_SIZE - page_busy(offset); }

//-----------------------------------------------
off_t page_base(off_t offset) { return offset - page_busy(offset); }

//-----------------------------------------------
//             SYNCHRONISATION
//-----------------------------------------------
int ramcache_lock_radix_page(int fd, off_t offset) {
    if (ramcache.cache_table[fd] != NULL) {
        radix_tree *tree = ramcache.cache_table[fd]->tree;
        if (tree != NULL) {
            int lockret = radix_lock_page(offset, tree);
            if (lockret) {
                printf("ERROR : Lock failed !\n");
                perror("Lock");
                // exit(EXIT_FAILURE);
            }
            if (trace(TRACE_LOCK)) {
                printinfo(NVTRACE,
                          BLU "|    Locked    | fd=%2d | off=%8ld |" RST, fd,
                          page_base(offset));
            }
            return lockret;
        }
    }

    printf("Failed to lock, fd=%d off=%ld\n", fd, offset);
    return 666;
}

//-----------------------------------------------
int ramcache_trylock_radix_pages(int fd, off_t offset, int size) {
    if (ramcache.cache_table[fd] != NULL) {
        radix_tree *tree = ramcache.cache_table[fd]->tree;
        if (tree != NULL) {
            int nbpages = 1 + ((size - 1 + page_busy(offset)) / RAM_PAGE_SIZE);

            offset = page_base(offset);
            if (trace(TRACE_TRYLOCK)) {
                printinfo(NVTRACE, "");
                printinfo(NVTRACE,
                          WHT
                          "| START TRYLOCK| fd=%2d | off=%8ld | size=%6d | "
                          "nbpages=%d" RST,
                          fd, offset, size, nbpages);
            }

            int locked = 0;
            for (int i = 0; i < nbpages; i++) {
                int ret = radix_trylock_page(offset + (i * RAM_PAGE_SIZE),
                                             ramcache.cache_table[fd]->tree);
                if (!ret) {
                    ++locked;
                    if (trace(TRACE_TRYLOCK)) {
                        printinfo(NVTRACE,
                                  WHT
                                  "|  Trylock OK  | fd=%2d | off=%8ld |" RST,
                                  fd, offset + (i * RAM_PAGE_SIZE));
                    }
                } else {
                    if (trace(TRACE_TRYLOCK)) {
                        printinfo(NVTRACE,
                                  RED
                                  "| Trylock Fail | fd=%2d | off=%8ld |" RST,
                                  fd, offset + (i * RAM_PAGE_SIZE));
                    }
                    for (int j = 0; j < locked; j++) {
                        radix_unlock_page(offset + (j * RAM_PAGE_SIZE),
                                          ramcache.cache_table[fd]->tree);
                    }
                    break;
                }
            }
            if (locked == nbpages) {
                if (trace(TRACE_TRYLOCK)) {
                    printinfo(NVTRACE,
                              WHT
                              "|END TRYLOCK OK| fd=%2d | off=%8ld | size=%6d | "
                              "nbpages=%d" RST,
                              fd, offset, size, nbpages);
                }
                return 0;  // Success
            } else {
                return 1;
            }
        }
    }
    if (trace(TRACE_TRYLOCK)) {
        printinfo(NVTRACE,
                  WHT "|END TRY FAILED| fd=%2d | off=%8ld | size=%6d |" RST, fd,
                  offset, size);
    }
    return 0;
}

//-----------------------------------------------
int ramcache_unlock_radix_page(int fd, off_t offset) {
    if (ramcache.cache_table[fd] != NULL) {
        radix_tree *tree = ramcache.cache_table[fd]->tree;
        if (tree != NULL) {
            int lockret = radix_unlock_page(offset, tree);
            if (lockret) {
                printf("ERROR : Failed unlock fd=%d off=%ld error=%d\n", fd,
                       offset, lockret);
                perror("Unlock");
                // exit(EXIT_FAILURE);
            }
            if (trace(TRACE_UNLOCK)) {
                printinfo(NVTRACE,
                          GRN "|   Unlocked   | fd=%2d | off=%8ld |" RST, fd,
                          page_base(offset));
            }
            return lockret;
        }
    }
    return 0;
}

//-----------------------------------------------
int ramcache_unlock_radix_pages(int fd, off_t offset, int size) {
    int nbpages = 1 + ((size - 1 + page_busy(offset)) / RAM_PAGE_SIZE);

    offset = page_base(offset);
    int ret = 0;
    for (int i = 0; i < nbpages; i++) {
        ret += ramcache_unlock_radix_page(fd, offset + (i * RAM_PAGE_SIZE));
    }
    return ret;
}

//-----------------------------------------------
//                READ
//-----------------------------------------------
ssize_t ramcache_pread(int fd, off_t offset, char *buf, size_t size) {
    off_t page_size = RAM_PAGE_SIZE, base = page_base(offset),
          free = page_free(offset);

    // first page
    ssize_t ret = page_read(fd, offset, buf, min(free, size));
    if (ret != free || free == size) {
        return ret;
    }

    // stats
    ramcache.overlaps++;

        
    size_t buf_offset = ret; // first is already written
    while(size-buf_offset > RAM_PAGE_SIZE){
      ret = page_read(fd, offset+buf_offset, buf+buf_offset, RAM_PAGE_SIZE);
      buf_offset += ret;
    }

    ret = page_read(fd, offset+buf_offset, buf+buf_offset, size-buf_offset);
    buf_offset += ret;
    
    return buf_offset;


    /*
    printinfo(NVTRACE, "Overlap : size = %ld  offset = %ld", size, offset);
    // # of additional pages
    unsigned int numpages = 1 + (size - 1 + page_busy(offset)) / page_size;
    if(page_busy(free+size)==0){
      ++numpages;
    }
    printinfo(NVTRACE, "Numpages = %d", numpages);
    for (unsigned int i = 1; i < numpages; ++i) {
      
      size_t to_read =
	    i == (numpages-1) ? page_busy(size - free) : page_size;
      printinfo(NVTRACE, "   |   page %d : to_read = %ld", i, to_read);
      size_t read = page_read(fd, base + i * page_size,
                                buf + free + (i - 1) * page_size, to_read);
	ret += read;
        if (read != to_read) {
            return ret;
        }
    }
    return ret;
    */
}

//-----------------------------------------------
size_t page_read(int fd, off_t offset, char *buf, size_t size) {
  
    page *p = get_page(fd, offset);  // To get the lock
    while(pthread_mutex_trylock(&p->lock)){
      p = get_page(fd, offset);  // Handles cache miss
    } //spinlock

    size_t read = 0;  
    off_t base = page_busy(offset), left = p->size - base;
    if (buf != NULL && left > 0) {
        read = min(size, left);
        memcpy(buf, p->content + base, read);
    }
    pthread_mutex_unlock(&p->lock);
    return read;
}

//-----------------------------------------------
void move_to_first_page(page *p){
  page *next = p->next;
  page *previous = p->previous;

  if(p!=ramcache.last){
    next->previous = previous;
  }
  if(p!=ramcache.first){
    previous->next = next;
  }
  if(p==ramcache.last){
    ramcache.last = previous;
    ramcache.last->next = NULL;
  }
  
  p->next = ramcache.first;
  p->previous = NULL;

  ramcache.first->previous = p;
  
  ramcache.first = p;
  p->touched = 0;  
}


//-----------------------------------------------
page *get_page(int fd, off_t offset) {  // Unaligned offset
    page *p = __get_page(fd, offset);
    return p;
}

//-----------------------------------------------
page *__get_page(int fd, off_t offset) {  // Unaligned offset
    if (offset < 0) {
        return NULL;
    }

    CHRONO_START(PERF_CACHEHIT);
    int dirty;
    page *p =
        radix_find(page_base(offset), ramcache.cache_table[fd]->tree, &dirty);

    if (p == NULL) {
        CHRONO_TRANSFER(PERF_CACHEHIT, PERF_CACHEMISS);
        ramcache_lock_radix_page(fd, offset);
        p = cache_miss(fd, offset);
        if (dirty > 0) {
            CHRONO_TRANSFER(PERF_CACHEHIT, PERF_DIRTYMISS);
            ramcache.dirty++;
            p = dirty_miss(p, fd, offset);
        }
        ramcache_unlock_radix_page(fd, offset);
    } else {  // If it is a hit, the page is up to date
        ramcache.hits++;
    }
    CHRONO_STOP(PERF_CACHEHIT);

    p->touched=1; // Will be sent to the beginning on attempt to evict
    return p;
}

//-----------------------------------------------
page *dirty_miss(page *p, int fd, size_t offset) {
    if (trace(TRACE_DIRTY_MISS)) {
        printinfo(NVTRACE, GRN "DIRTY MISS : Page (fd=%d, offset=%ld)" RST, fd,
                  offset);
    }
    // pthread_mutex_lock(&nvcache_flush_mutex);
    int played = nvlog_play_log_on_page(fd, p);
    if (played ==
        radix_get_dirty_level(offset, ramcache.cache_table[fd]->tree)) {
        // pthread_mutex_unlock(&nvcache_flush_mutex);
        return p;  // Give the updated page
    } else {
        // The page has already been updated in the last batch
        // pthread_mutex_unlock(&nvcache_flush_mutex);
        printinfo(
		  NVTRACE, "CAS SPECIAL : offset=%ld played=%d dirty=%d", p->offset, played,
            radix_get_dirty_level(offset, ramcache.cache_table[fd]->tree));
        return (get_page(fd, offset));
	}
}

//-----------------------------------------------
// nvcache_flush_mutex is used to serialize the following procedures:
// void flush_batch() @nvlog.c
// void flush_entry() @nvlog.c
// int nvlog_play_log_on_page(int fd, page *rampage) @nvlog.c
// void nvlog_flush_file(int fd) @nvlog.c
// page *cache_miss(int fd, off_t offset) @nvcache_ram.c
//-----------------------------------------------
page *cache_miss(int fd, off_t offset) {
    ramcache.misses++;
    if (trace(TRACE_MISS)) {
        printinfo(NVTRACE, GRN "Cache miss : Page (fd=%d, off=%ld)" RST, fd,
                  offset);
    }
    // pthread_mutex_lock(&nvcache_flush_mutex);
    // ramcache_lock_page(fd, offset);
    page *ret = __cache_miss(fd, offset);
    // ramcache_unlock_page(fd, offset);
    // pthread_mutex_unlock(&nvcache_flush_mutex);
    return ret;
}

//-----------------------------------------------
page *__cache_miss(int fd, off_t offset) {
    char content[RAM_PAGE_SIZE];
    ssize_t rd = musl_pread((int)fd, (void *)content, (size_t)RAM_PAGE_SIZE,
                            (off_t)page_base(offset));

    if (rd >= (ssize_t)0) {
        page *p = add_page(page_base(offset), rd, rd == 0 ? NULL : content,
                           ramcache.cache_table[fd]);
        return p;
    } else {
        printinfo(NVCRIT, "pread_musl fd=%d off=%ld returned %ld", fd, offset,
                  rd);
        perror("pread_musl in cache miss");  // Might be because of a bad open
        return NULL;
    }
}

//-----------------------------------------------
page *add_page(off_t offset, ssize_t size, char *buf, radixcache *cache) {
  page *mypage = __add_page(offset, size, buf, cache);
  return mypage;
}


//-----------------------------------------------
page *__add_page(off_t offset, ssize_t size, char *buf, radixcache *cache) {
    // printf("Add_Page : fd=%d off=%ld size=%ld\n", cache->fd, offset,
    // size);
    page *newpage = rm_last_page();
    //The page is locked
    
    if (newpage == NULL) {
        perror("rm_last_page returned NULL");
        return NULL;
    }

    //memset(newpage->content, 0,
    //     RAM_PAGE_SIZE);  // Erase garbage, could be removed ?
    if (buf != NULL) {      // If NULL, the content is empty for the moment
        memcpy(newpage->content, buf, RAM_PAGE_SIZE);
    }
    newpage->next = ramcache.first;
    newpage->previous = NULL;
    newpage->fd = cache->fd;
    newpage->offset = offset;
    newpage->size = size;

    ramcache.first->previous = newpage;
    ramcache.first = newpage;

    // Add into radix tree
    int test = radix_insert(newpage, offset, cache->tree);
    if (test == -1) {
        perror("Insert in RADIX returned an error");
        return NULL;
    }

    if (trace(TRACE_ADD)) {
        printinfo(NVTRACE, GRN "Add page : Page (fd=%d, off=%ld, size=%ld)" RST,
                  cache->fd, offset, size);
    }
    pthread_mutex_unlock(&newpage->lock);
    return newpage;
}

//-----------------------------------------------
page *spinlock_last_page(){
  page *last_page;
  do {
    last_page = ramcache.last;
  } while(pthread_mutex_trylock(&last_page->lock));
  return last_page;
}

//-----------------------------------------------
page *rm_last_page() {
  pthread_mutex_lock(&lru_lock);
  page *last_page;

  tryrm:
  last_page = spinlock_last_page();
  
  if(last_page->touched)
    {
      //Update page position
      move_to_first_page(last_page);
      pthread_mutex_unlock(&last_page->lock);
      goto tryrm;
    }
    
    
    last_page->previous->next = NULL;
    ramcache.last = last_page->previous;
    // Clean the radix tree
    if (last_page->fd != -1) {  // If the page is already in a radix tree
        if (ramcache.cache_table[last_page->fd] != NULL) {
	    radix_evict(last_page, ramcache.cache_table[last_page->fd]->tree);
        }
        if (trace(TRACE_EVICT)) {
            printinfo(NVTRACE,
                      GRN
                      "Eviction : Page (fd=%d, off=%ld, size=%ld) evicted." RST,
                      last_page->fd, last_page->offset, last_page->size);
        }
    }
    //last_page->state = CLEAN;
    pthread_mutex_unlock(&lru_lock);
    pthread_mutex_unlock(&last_page->lock);
    return last_page;
}

//-----------------------------------------------
//                WRITE
//-----------------------------------------------
ssize_t ramcache_pwrite(int fd, off_t offset, const char *buf, size_t size) {
    off_t page_size = RAM_PAGE_SIZE, base = page_base(offset),
          free = page_free(offset);

    // first page
    ssize_t ret = page_write(fd, offset, buf, min(free, size));
    if (ret != free || free == size) {
        return ret;
    }

    // stats
    ramcache.overlaps++;


    size_t buf_offset = ret; //1 is already written
    while(size-buf_offset > RAM_PAGE_SIZE){
      ret = page_write(fd, offset+buf_offset, buf+buf_offset, RAM_PAGE_SIZE);
      buf_offset += ret;
    }

    ret = page_write(fd, offset+buf_offset, buf+buf_offset, size-buf_offset);
    buf_offset += ret;
    
    return buf_offset;



    /*
    // # of additional pages
    unsigned int numpages = 1 + (size - 1 + page_busy(offset)) / page_size;
    if(page_busy(free+size)==0){
      ++numpages;
    }
    for (unsigned int i = 0; i < numpages; ++i) {
        size_t to_write =
            i == (numpages - 1) ? page_busy(size - free) : page_size;
        size_t written = page_write(fd, base + (i + 1) * page_size,
                                    buf + free + i * page_size, to_write);
        ret += written;
        if (written != to_write) {
            return ret;
        }
    }
    return ret;
    */
}

//-----------------------------------------------
size_t page_write(int fd, off_t offset, const char *buf, size_t size) {
    // page *p = get_page(fd, offset);  // Handles cache miss

    size_t ret = 0;
    int dirty = 0;

    page *p =
        radix_find(page_base(offset), ramcache.cache_table[fd]->tree, &dirty);

    if (p != NULL) {
        while(pthread_mutex_trylock(&p->lock)){
	    page *p =
	      radix_find(page_base(offset), ramcache.cache_table[fd]->tree, &dirty);
	    
	    if(p==NULL) return size;
	}
        p->state = DIRTY;
        ret = min(page_free(offset), size);
        off_t busy = page_busy(offset);
        memcpy(p->content + busy, buf, ret);
        // Update page size
        p->size = max(p->size, busy + ret);
        assert(p->size <= RAM_PAGE_SIZE);
	pthread_mutex_unlock(&p->lock);
	return ret;
    }
    return size;
}

//-----------------------------------------------
int ramcache_exists(int fd) { return ramcache.cache_table[fd] != NULL; }

//-----------------------------------------------
radixcache *ramcache_newradix(int fd) {
    radixcache *rcache = malloc(sizeof(radixcache));
    rcache->fd = fd;
    rcache->tree = radix_newtree(RAM_PAGE_SIZE);
    ramcache.cache_table[fd] = rcache;

    return rcache;
}

//-----------------------------------------------
void ramcache_file_clean(int fd) {
    radixcache *cache = ramcache.cache_table[fd];
    if (cache != NULL) {
        ramcache.cache_table[fd] = NULL;
        if (trace(TRACE_CLEAN)) {
            printinfo(NVTRACE, GRN "RAM cache: file (fd=%d) cleaned" RST, fd);
        }
    } else {
        printinfo(NVWARN, "RAM cache clean: fd %d was not found.", fd);
    }
}

//-----------------------------------------------
void ramcache_lower_dirty_level(key k, int size, int fd) {
    int nbpages = 1 + ((size - 1 + page_busy(k)) / RAM_PAGE_SIZE);

    k = page_base(k);
    for (int i = 0; i < nbpages; i++) {
        radix_decrease_dirty_level(k + (i * RAM_PAGE_SIZE),
                                   ramcache.cache_table[fd]->tree);
    }
}

//-----------------------------------------------
void ramcache_greater_dirty_level(key k, int size, int fd) {
    int nbpages = 1 + ((size - 1 + page_busy(k)) / RAM_PAGE_SIZE);

    k = page_base(k);
    for (int i = 0; i < nbpages; i++) {
        radix_increase_dirty_level(k + (i * RAM_PAGE_SIZE),
                                   ramcache.cache_table[fd]->tree);
    }
}

//-----------------------------------------------
int ramcache_get_dirty_level(key k, int fd) {
    return radix_get_dirty_level(k, ramcache.cache_table[fd]->tree);
}

//-----------------------------------------------
int get_cache_length_fw() {
    int i = 1;
    page *current_page = ramcache.first;
    while (current_page->next != NULL) {
        i++;
        current_page = current_page->next;
    }
    return i;
}

//-----------------------------------------------
int get_cache_length_bw() {
    int i = 1;
    page *current_page = ramcache.last;
    while (current_page->previous != NULL) {
        i++;
        current_page = current_page->previous;
    }
    return i;
}

//-----------------------------------------------
//            SUMMARY PRINTING
//-----------------------------------------------
void print_remaining() {
    char fdlist[1024];
    fdlist[0] = 0;
    for (int i = 0; i < 1024; ++i) {
        if (ramcache_exists(i)) {
            size_t cur_size = strlen(fdlist), left = sizeof(fdlist) - cur_size;
            int ret =
                snprintf(fdlist + cur_size, left, fdlist[0] ? ", %d" : "%d", i);
            if (ret < 0 || ret > left) {
                break;
            }
        }
    }
    if (fdlist[0] == 0) {
        printinfo(NVINFO, YEL "\t - No radix tree is left -\n\tfd = %s" RST,
                  fdlist);
    } else {
        printinfo(NVINFO, YEL "\t - Remaining radix trees -\n\tfd = %s" RST,
                  fdlist);
    }
}

//-----------------------------------------------
void ramcache_print() {
    print_remaining();
    printinfo(NVINFO,
              YEL
              "\t----- Global Cache state -----\n"
              "\t Cache hits  | %8lu\n"
              "\tCache misses | %8lu\n"
              "\t             |\n"
              "\t   TOTAL     | %8lu\n"
              "\t             |\n"
              "\t  Overlaps   | %8lu\n"
              "\tDirty misses | %8lu\n"
              "\t--------------------------------\n"
              "\t--------------------------------\n"
              "\tCache length (FW) : %d\n"
              "\tCache length (BW) : %d\n" RST,
              ramcache.hits, ramcache.misses, ramcache.hits + ramcache.misses,
              ramcache.overlaps, ramcache.dirty, get_cache_length_fw(),
              get_cache_length_bw());
}

//-----------------------------------------------
// For debug only
void gdb_print_dirty(ssize_t min_off, ssize_t max_off, int fd) {
    printinfo(NVTRACE, "|  Offset  |  Dirty level  |");
    printinfo(NVTRACE, "+----------+---------------+");
    for (ssize_t k = min_off % RAM_PAGE_SIZE; k < max_off; k += RAM_PAGE_SIZE) {
        int dirty_level =
            radix_get_dirty_level(k, ramcache.cache_table[fd]->tree);
        if (dirty_level > 0) {
            printinfo(NVTRACE, "|%10ld|%15d|", k, dirty_level);
        }
    }
    printinfo(NVTRACE, "+----------+---------------+\n");
}

//-----------------------------------------------
//             AUXILIARY
//-----------------------------------------------
int trace(int bit) { return tracemask & bit; }
