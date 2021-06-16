#pragma once
#include <sys/types.h>
#include <stdatomic.h>
#include "nvcache_config.h"

//---------- RADIX ------------

#define RADIXMASK_LEN 8
#define RADIX_MASK 0xFF  // 011 1111 1111
#define RADIX_MAXCHILDREN (RADIX_MASK + 1)

typedef off_t key;  // Here, the key is an offset in a file

struct node_s;
struct leaf_s;
typedef union child_u {
    struct node_s *subnode;
    struct leaf_s *leafnode;
} child;

typedef struct node_s {
    struct node_s *parent;
    int level;
    child children[RADIX_MAXCHILDREN];
} node;

typedef struct leaf_s {
    struct node_s *parent;
    int level;
    void *pages[RADIX_MAXCHILDREN];
    int dirty[RADIX_MAXCHILDREN];
    pthread_mutex_t lock[RADIX_MAXCHILDREN];
} leaf;

typedef struct radix_tree_s {
    node *root;
    off_t page_size;
} radix_tree;

typedef struct radixcache_s {
    int fd;
    radix_tree *tree;
} radixcache;



//----------- NVLOG -------------

// TODO : reopen / close with this struct in NVRAM
typedef struct {
    char path[200];
    int flags;
    mode_t mode;
    char opened;
} file_t;

typedef struct {
    int fd;  // TODO : change for file_id       // Index in the file_table
    size_t offset;          // Unaligned
    size_t size;            // The n firts bytes are the change
    size_t waiting;         // Index of the log you're waiting to be committed
    atomic_char committed;  // Is the entry ready to be written on disk ?
    char already_written;   // If the file has been closed already, entry has
                            // been flushed.
    char content[LOGENTRY_SIZE];
} log_entry_t;

typedef struct {
    file_t file_table[MAX_FILES];
    volatile size_t nvlog_tail;
    log_entry_t entries[];
} nvlog_t;



//----------- RAM CACHE -------------

enum page_state_e { CLEAN, DIRTY, LAST_CHANCE };
typedef enum page_state_e page_state;


typedef struct page_s {
    int fd;
    off_t offset;
    ssize_t size;
    char content[RAM_PAGE_SIZE];
    struct page_s *previous;
    struct page_s *next;
    page_state state;
    pthread_mutex_t lock; // Sync between read/writes and eviction
    char touched; // For LRU
    leaf *radix_parent; // Pointer to corresponding leaf in the radix tree
} page;

typedef struct ramcache_s {

    page *first, *last;
    unsigned long int hits, misses, overlaps, writes, dirty;
    double w_latency;
    radixcache *cache_table[MAX_FILES];  // One radix root per file
    page* page_table;  // One page table for all files
} ramcache_t;

