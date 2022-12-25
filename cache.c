#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "cache.h"
#include "jbod.h"

static cache_entry_t *cache = NULL;
static int cache_size = 0;
static int num_queries = 0;
static int num_hits = 0;

int cache_create(int num_entries) {
  // prevent double allocation
  if (cache != NULL) return -1;
  if ((num_entries >= 2) && (num_entries <= 4096)) {
    cache = calloc(num_entries, sizeof(cache_entry_t));
    cache_size = num_entries;
    // reset number of queries and hits
    num_queries = 0;
    num_hits = 0;
    return 1;
  }
  return -1;
}

int cache_destroy(void) {
  // prevent double freeing the memory
  if (cache == NULL) return -1;
  free(cache);
  cache = NULL; // remove dangling pointer
  cache_size = 0;
  return 1;
}

int lookup_precheck(int disk_num, int block_num, uint8_t *buf) {
  if (cache == NULL) return -1; // uninitialized cache
  if (cache->valid == false) return -1; // empty cache
  if (buf == NULL) return -1; // lookup with NULL buf pointer
  if ((disk_num < 0) || (disk_num >= JBOD_NUM_DISKS)) return -1; // invalid disk
  if ((block_num < 0) || (block_num >= JBOD_BLOCK_SIZE)) return -1; // invalid block
  return 1;
}

int insert_precheck(int disk_num, int block_num, const uint8_t *buf) {
  if (cache == NULL) return -1; // uninitialized cache
  if (buf == NULL) return -1; // insert NULL buf pointer
  if ((disk_num < 0) || (disk_num >= JBOD_NUM_DISKS)) return -1; // invalid disk
  if ((block_num < 0) || (block_num >= JBOD_BLOCK_SIZE)) return -1; // invalid block
  cache_entry_t *p = cache;
  while (p < cache+cache_size) {
    // duplicate value exists
    if (p->disk_num == disk_num && p->block_num == block_num && p->valid == 1) { return -1; }
    p++;
  }
  return 1;
}


int cache_lookup(int disk_num, int block_num, uint8_t *buf) {
  if (lookup_precheck(disk_num, block_num, buf) == -1) return -1;
  bool isPresent = false;
  // iterate through the cache
  cache_entry_t *p = cache;
  while (p < cache+cache_size) {
    // if found
    if (p->disk_num == disk_num && p->block_num == block_num) {
      memcpy(buf, p->block, JBOD_BLOCK_SIZE); // copy block into buf
      ++num_hits;
      ++(p->num_accesses);
      isPresent = true;
      break;
    }
    p++;
  }
  // lookup performed
  ++num_queries;
  return (isPresent) ? 1 : -1;
}

void cache_update(int disk_num, int block_num, const uint8_t *buf) {
  cache_entry_t *p = cache;
  while (p < cache+cache_size) {
    // if found
    if (p->disk_num == disk_num && p->block_num == block_num) {
      // update content block
      memcpy(p->block, buf, JBOD_BLOCK_SIZE);
      ++(p->num_accesses);
      break; // terminate
    }
    p++;
  }
}

int cache_insert(int disk_num, int block_num, const uint8_t *buf) {
  if (insert_precheck(disk_num, block_num, buf) == -1) return -1;
  cache_entry_t *overwrite = cache; // problem with this
  int freq = 0;
  cache_entry_t *p = cache;
  while (p < cache+cache_size) {
    // cache entry is empty
    if (p->valid == 0) {
      // insert block data
      p->valid = 1;
      p->disk_num = disk_num;
      p->block_num = block_num;
      p->num_accesses = 1;
      // copy buf into the block
      memcpy(p->block, buf, JBOD_BLOCK_SIZE);
      return 1;
    }
    // cache entry is not empty
    else {
      // update LFU status
      if (freq > p->num_accesses) {
        freq = p->num_accesses;
        overwrite = p;
      }
    }
    p++;
  }
  // overwrite to LFU cache entry
  overwrite->valid = 1;
  overwrite->disk_num = disk_num;
  overwrite->block_num = block_num;
  overwrite->num_accesses = 1;
  // copy buf into the block
  memcpy(overwrite->block, buf, JBOD_BLOCK_SIZE);
  return 1; // terminate with success
}

bool cache_enabled(void) {
	return cache != NULL && cache_size > 0;
}

void cache_print_hit_rate(void) {
	fprintf(stderr, "num_hits: %d, num_queries: %d\n", num_hits, num_queries);
	fprintf(stderr, "Hit rate: %5.1f%%\n", 100 * (float) num_hits / num_queries);
}