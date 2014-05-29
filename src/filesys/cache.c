#include "filesys/cache.h"
#include <stdint.h>

#define BUFFER_CACHE_SIZE 64

/* Cache entry for the file blocks' cache, i.e. buffer_cache. */
struct cache_entry
{
  block_sector_t sector;               /* Sector number of the cached block. */
  bool accessed;                       /* Whether the block has been accessed. */
  bool dirty;                          /* Whether the block has been written. */
  uint8_t content[BLOCK_SECTOR_SIZE];  /* Block content. */
};

typedef struct cache_entry cache_entry_t;

/* Buffer cache of file blocks. 
 * In default, as many as 64 blocks can be cached. */
static cache_entry_t buffer_cache[BUFFER_CACHE_SIZE];


/* Look up a sector for read (to_write==false) or write (to_write==true).
 * Return cache entry index if exists; otherwise return -1. */
static int
sector_exists (block_sector_t sector, bool to_write)
{
  size_t i;
  for (i = 0; i<BUFFER_CACHE_SIZE; i++)
  {
    if (buffer_cache[i].sector == sector)
    {
      return i;
    }
  }
  return -1;
}

/* Initialization of buffer_cache entries. */
void
cache_init (void)
{
  size_t i;
  for (i = 0; i<BUFFER_CACHE_SIZE; i++)
  {
    buffer_cache[i].sector = UINT32_MAX;
    buffer_cache[i].accessed = false;
    buffer_cache[i].dirty = false;
    memset (buffer_cache[i].content, 0, BLOCK_SECTOR_SIZE);
  }
}
