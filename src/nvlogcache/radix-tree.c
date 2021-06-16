#define _GNU_SOURCE
#include "radix-tree.h"
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include "nvinfo.h"
#include "nvcache_config.h"


#define TRACE_ADD 0x1
#define TRACE_DELETE 0x2
#define TRACE_DIRTY_LEVEL 0x4

static int tracemask = 0; //TRACE_ADD | TRACE_DELETE | TRACE_DIRTY_LEVEL;

//ram_page_size = 2^12
#define SHORTEN_KEY(k) k/RAM_PAGE_SIZE 
#define REAL_KEY(k) k*RAM_PAGE_SIZE

#define RADIX_MAX_POOL_SIZE 0 //Disabled for the moment

static int pool_size  = 0;
static node nodes_pool;
static int radix_last_level = 0;


//-----------------------------------------------
//              NOT EXPORTED
//-----------------------------------------------
#ifdef NVCACHE_DEBUG
static int trace(int bit);
#else
#define trace(x) 0
#endif
static void add_to_pool(node *node);
static node *take_node(int level, node *parent);
static leaf *find_last_node(key k, radix_tree *tree);
static node *radix_newnode(int level, node *parent);
static int radix_index(key k, int i);
static leaf *get_leaf(key k, radix_tree *tree);
static int free_leaf(leaf *f);
static void radix_free_node(node *base_node);
static void clean_radix(node *last_node, key k);


//-----------------------------------------------
int radix_index(key k, int i) {
  return RADIX_MASK & (k >> ((radix_last_level - i) * RADIXMASK_LEN));
}

//-----------------------------------------------
leaf *get_leaf(key k, radix_tree *tree) {
  //k = k - (k % tree->page_size);
  return find_last_node(k, tree);
}

//-----------------------------------------------
void radix_init_nodes(int cache_size) {
  printinfo(NVINFO,
	    GRN "\tCreating radix nodes.\n\tradix_last_level = %d" RST,
	    radix_last_level);
  for (int i = 0; i < cache_size; i++) {
    add_to_pool(radix_newnode(0, NULL));
  }
}

//-----------------------------------------------
radix_tree *radix_newtree(off_t psize) {
  if(radix_last_level==0){
    radix_last_level =
      ((((sizeof(key) * 8)-log2(RAM_PAGE_SIZE)) / RADIXMASK_LEN) - 1);  // Root is level 0. We remove 12 for page size.
  }
  radix_tree *tree = malloc(sizeof(radix_tree));
  tree->root = radix_newnode(0, NULL);
  tree->page_size = psize;
  // Yolo
  // init_subnodes(tree->root);
  return tree;
}

//-----------------------------------------------
void add_to_pool(node *node) {
  if(pool_size < RADIX_MAX_POOL_SIZE){
    for (int i = 1; i < RADIX_MASK + 1; i++) {
      node->children[i].subnode = NULL;
    }
    node->children[0] = nodes_pool.children[0];
    nodes_pool.children[0].subnode = node;
    atomic_fetch_add(&pool_size, 1);
  }
  else {
    free(node);
  }
}

//-----------------------------------------------
node *take_node(int level, node *parent) {
  // If a node already allocated is available
  if(pool_size>0){
    node *ret = nodes_pool.children[0].subnode;
    if(ret != NULL){
      nodes_pool.children[0] = ret->children[0];
      ret->children[0].subnode = NULL;
      ret->level = level;
      ret->parent = parent;
      atomic_fetch_add(&pool_size, -1);
    }
    return ret;
  }
  return NULL;
}

//-----------------------------------------------
leaf *radix_newleaf(int level, node *parent) {
  leaf *nleaf = malloc(sizeof(leaf));

  for (int i = 0; i < RADIX_MAXCHILDREN; i++) {
    nleaf->pages[i] = NULL;
    nleaf->dirty[i] = 0;
    // Init recursive lock
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&nleaf->lock[i], &attr);
  }
  nleaf->level = level;
  nleaf->parent = parent;
  return nleaf;
}

//-----------------------------------------------
node *radix_newnode(int level, node *parent) {
  if (nodes_pool.children[0].subnode == NULL) {
    node *nnode = malloc(sizeof(node));

    for (int i = 0; i < RADIX_MAXCHILDREN; i++) {
      nnode->children[i].subnode = NULL;
    }
    nnode->level = level;
    nnode->parent = parent;
    return nnode;
  } else {
    return take_node(level, parent);
  }
}

//-----------------------------------------------
void *radix_find(key k, radix_tree *tree, int *dirty) {
  if (k % RAM_PAGE_SIZE != 0) {
    printinfo(NVCRIT,
	      "radix_find: ASKED AN UNALIGNED OFFSET TO THE TREE\n");
  }
  k = SHORTEN_KEY(k);
    
  leaf *leafnode = find_last_node(k, tree);
  if (leafnode != NULL) {
    int idx = radix_index(k, radix_last_level);
    if (dirty) {
      *dirty = leafnode->dirty[idx];
    }
    return leafnode->pages[idx];
  }

  if (dirty) {
    *dirty = -1;
  }
  return NULL;
}

//-----------------------------------------------
leaf *find_last_node(key k, radix_tree *tree) {
  child current_node;
  current_node.subnode = tree->root;
  for (int i = 0; i < radix_last_level; i++) {
    long int value = radix_index(k, i);
    if (current_node.subnode->children[value].subnode == NULL) {
      return NULL;
    }
    current_node = current_node.subnode->children[value];
  }
  return current_node.leafnode;
}

//-----------------------------------------------
int radix_get_dirty_level(key k, radix_tree *tree) {
  k = SHORTEN_KEY(k);
  leaf *l = get_leaf(k, tree);
  if (l == NULL) {
    return -1;
  }
  return l->dirty[radix_index(k, radix_last_level)];
}

//-----------------------------------------------
void radix_decrease_dirty_level(key k, radix_tree *tree) {
  k = SHORTEN_KEY(k);
  leaf *l = get_leaf(k, tree);
  if (l == NULL) return;

  int index = radix_index(k, radix_last_level);
  int dirty = l->dirty[index];
  if (dirty > 0) {
    if (trace(TRACE_DIRTY_LEVEL)) {
      printinfo(
                NVTRACE,
                YEL
                "RADIX : Decrease dirty level (key=%ld, tree=%p) %d => %d" RST,
                k, tree, dirty, dirty - 1);
    }
    atomic_fetch_add(&l->dirty[index], -1);
  }
}

//-----------------------------------------------
void radix_increase_dirty_level(key k, radix_tree *tree) {
  k = SHORTEN_KEY(k);
  leaf *l = get_leaf(k, tree);
  if (l == NULL) {
    radix_insert(NULL, REAL_KEY(k), tree);
    l = get_leaf(k, tree);
    assert(l != NULL);
  }

  int index = radix_index(k, radix_last_level);
  int dirty = l->dirty[index];
  if (trace(TRACE_DIRTY_LEVEL)) {
    printinfo(
	      NVTRACE,
	      YEL "RADIX : Increase dirty level (key=%ld, tree=%p) %d => %d" RST,
	      k, tree, dirty, dirty + 1);
  }

  atomic_fetch_add(&l->dirty[index], 1);
}

//-----------------------------------------------
int radix_lock_page(key k, radix_tree *tree) {
  k = k - k % tree->page_size;
  k = SHORTEN_KEY(k);
  leaf *l = get_leaf(k, tree);
  if (l == NULL) {
    radix_insert(NULL, REAL_KEY(k), tree);
    l = get_leaf(k, tree);
    assert(l != NULL);
  }

  int index = radix_index(k, radix_last_level);
  return pthread_mutex_lock(&l->lock[index]);
}

//-----------------------------------------------
int radix_trylock_page(key k, radix_tree *tree) {
  k = k - k % tree->page_size;
  k = SHORTEN_KEY(k);
  leaf *l = get_leaf(k, tree);
  if (l == NULL) {
    radix_insert(NULL, REAL_KEY(k), tree);
    l = get_leaf(k, tree);
    assert(l != NULL);
  }

  int index = radix_index(k, radix_last_level);

  // Returns 0 on success
  return pthread_mutex_trylock(&l->lock[index]);
}

//-----------------------------------------------
int radix_unlock_page(key k, radix_tree *tree) {
  k = k - k % tree->page_size;
  k = SHORTEN_KEY(k);
  leaf *l = get_leaf(k, tree);
  if (l == NULL) return -1;  // Should create the leaf and lock

  int index = radix_index(k, radix_last_level);
  return pthread_mutex_unlock(&l->lock[index]);
}

//-----------------------------------------------
void radix_free_tree(radix_tree *tree){
  radix_free_node(tree->root);
}


//-----------------------------------------------
void radix_free_node(node *base_node){ // Free subtree of base_node + base_node
  for(int i=0; i<RADIX_MAXCHILDREN; i++){
    if(base_node->children[i].subnode != NULL || base_node->children[i].leafnode != NULL){
      if(base_node->level != radix_last_level-1){
	radix_free_node(base_node->children[i].subnode);
      }
      free(&base_node->children[i]);
    }
  }
  free(base_node);
}


//-----------------------------------------------
int radix_evict(page *p, radix_tree *tree){
  if (p == NULL) {
    return -1;
  }
  
  off_t k = SHORTEN_KEY(p->offset);
  leaf *l = p->radix_parent;
  if (l != NULL) {
    if (trace(TRACE_DELETE)) {
      printinfo(NVTRACE, YEL "RADIX : Remove node (key=%ld, fd=%d)" RST,
		k, p->fd);
    }
    l->pages[radix_index(k, radix_last_level)] =
      NULL;  // Remove pointer to page


    return 0;
  }
  else {
    return radix_remove_and_clean(p->offset, tree);
  }
}

//-----------------------------------------------
// DEPRECATED, use radix_evict
  
int radix_remove_and_clean(key k, radix_tree *tree) { 
  if (tree == NULL) {
    return -1;
  }
  k = SHORTEN_KEY(k);
  leaf *l = find_last_node(k, tree);
  if (l != NULL) {
    if (trace(TRACE_DELETE)) {
      printinfo(NVTRACE, YEL "RADIX : Remove node (key=%ld, tree=%p)" RST,
		k, tree);
    }
    l->pages[radix_index(k, radix_last_level)] =
      NULL;  // Remove pointer to page

    /*
    // Automatic cleanup : disabled.
    // We only remove the entire tree when we close the file.
    if(!l->refc){
    node *last_node = l->parent;
    if(free_leaf(l)){ // If we managed to remove the leaf
    clean_radix(last_node, k); // Clean previous nodes if needed
    }
    }
    */
  }
  return 0;
}

//-----------------------------------------------
int free_leaf(leaf *l){
  for(int i=0; i<RADIX_MAXCHILDREN; i++){
    if(l->dirty[i]){
      return 0;
    }
  }
  free(l);
  return 1;
}

//-----------------------------------------------
void clean_radix(node *last_node, key k){
  if(last_node->level > 0) {return;}
  
  node *parent = last_node->parent;
  parent->children[radix_index(k, parent->level)].subnode = NULL;
  free(last_node);
  clean_radix(parent, k);
  
}

//-----------------------------------------------
int radix_insert(void *content, key k, radix_tree *tree) {
  if (trace(TRACE_ADD)) {
    printinfo(NVTRACE, YEL "RADIX : Add node (key=%ld, tree=%p)" RST, k,
	      tree);
  }
  k = SHORTEN_KEY(k);
    
  child current_node;
  current_node.subnode = tree->root;
  for (int i = 0; i < radix_last_level; i++) {
    int value = radix_index(k, i);
    child expected = current_node.subnode->children[value];
    if (expected.subnode == NULL && expected.leafnode == NULL) {
      if (i + 1 == radix_last_level) {
	child *child_to_add = malloc(sizeof(child));
	child_to_add->leafnode = radix_newleaf(i + 1, current_node.subnode);
	if(!atomic_compare_exchange_strong(&current_node.subnode->children[value].leafnode, &expected.leafnode, child_to_add->leafnode)){
	  // Another thread already added this node : free and continue
	  free(child_to_add);
	}
	      
	      
      }
      else {
	child *child_to_add = malloc(sizeof(child));
	child_to_add->subnode = radix_newnode(i + 1, current_node.subnode);
	if(!atomic_compare_exchange_strong(&current_node.subnode->children[value].subnode, &expected.subnode, child_to_add->subnode)){
	  // Another thread already added this node : free and continue
	  free(child_to_add);
	}
      }
	        
    }
    current_node = current_node.subnode->children[value];
  }

  current_node.leafnode->pages[radix_index(k, radix_last_level)] = content;
  return 0;
}

//-----------------------------------------------
//             AUXILIARY
//-----------------------------------------------
#ifdef NVCACHE_DEBUG
int trace(int bit) { return tracemask & bit; }
#endif
