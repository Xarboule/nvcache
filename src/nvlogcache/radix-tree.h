#pragma once
#include <unistd.h>
#include <pthread.h>
#include "nvcache_types.h"


#ifdef __cplusplus
extern "C" {
#endif


void radix_init_nodes(int cache_size);
radix_tree *radix_newtree(off_t psize);
void radix_increase_dirty_level(key k, radix_tree *tree);
void radix_decrease_dirty_level(key k, radix_tree *tree);
int radix_lock_page(key k, radix_tree *tree);
int radix_trylock_page(key k, radix_tree *tree);
int radix_unlock_page(key k, radix_tree *tree);
int radix_get_dirty_level(key k, radix_tree *tree);
void *radix_find(key k, radix_tree *tree, int *dirty);
void radix_free_tree(radix_tree *tree);
  int radix_evict(page *p, radix_tree *tree);
int radix_remove_and_clean(key k, radix_tree *tree);
int radix_insert(void *content, key k, radix_tree *tree);

#ifdef __cplusplus
}
#endif
