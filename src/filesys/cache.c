#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include <stdint.h>
#include <string.h>
#include <debug.h>

#define BUFFER_CACHE_SIZE 64

/* Cache entry for the file blocks' cache, i.e. buffer_cache. */
struct cache_entry
{
  block_sector_t sector;               /* Sector number of the cached block. */
  block_sector_t new_sector;           /* Sector no of block after eviction. */
  bool accessed;                       /* Whether has been accessed. */
  bool dirty;                          /* Whether has been written. */
  bool evicting;                       /* Whether being evicted. */
  bool flushing;                       /* Whether being flushed. */
  size_t reader;                       /* Number of active readers. */
  size_t writer;                       /* Number of active writers. */
  size_t waiting_reader;               /* Number of waiting readers. */
  size_t waiting_writer;               /* Number of waiting writers. */
  struct lock lock;
  struct condition ready;
  //struct condition ready2read;
  //struct condition ready2write;
  uint8_t content[BLOCK_SECTOR_SIZE];  /* Block content. */
};

/* Buffer cache of file blocks. 
 * In default, as many as 64 blocks can be cached. */
static struct cache_entry buffer_cache[BUFFER_CACHE_SIZE];

static size_t hand;
static struct lock hand_lock;

static void
clock_hand_increase_one (void)
{
  lock_acquire (&hand_lock);
  hand = (hand + 1) % BUFFER_CACHE_SIZE;
  lock_release (&hand_lock);
}

/* Initialization of buffer_cache entries and global variables. */
void
cache_init (void)
{
  size_t i;
  struct cache_entry *entry;
  for (i = 0; i<BUFFER_CACHE_SIZE; i++)
  {
    entry = &buffer_cache[i];
    entry->sector = UINT32_MAX;
    entry->new_sector = UINT32_MAX;
    entry->accessed = false;
    entry->dirty = false;
    entry->evicting = false;
    entry->flushing = false;
    entry->reader = 0;
    entry->writer = 0;
    entry->waiting_reader = 0;
    entry->waiting_writer = 0;
    lock_init (&entry->lock);
    cond_init (&entry->ready);
    //cond_init (&entry->ready2read);
    //cond_init (&entry->ready2write);
    memset (entry->content, 0, BLOCK_SECTOR_SIZE);
  }
  hand = 0;
  lock_init (&hand_lock);
}

void 
cache_flush (void)
{
  size_t i;
  struct cache_entry *entry;
  for (i = 0; i<BUFFER_CACHE_SIZE; i++)
  {
    entry = &buffer_cache[i];
    lock_acquire (&entry->lock);
    if (entry->dirty)
    {
      /* Unecessary to flush if it is already being flushed or evicted. */
      if (entry->flushing || entry->evicting)
      {
        lock_release (&entry->lock);
        continue;
      }
      entry->flushing = true;
      lock_release (&entry->lock);
      /* IO without holding any locks. */
      block_write (fs_device, entry->sector, entry->content);
      lock_acquire (&entry->lock);
      entry->dirty = false;
      entry->flushing = false;
      cond_broadcast (&entry->ready, &entry->lock);
      lock_release (&entry->lock);
    }
  }
}

/* Look up a sector in buffer_cache.
 * Return cache entry index if sector exists; otherwise return -1. */
static int
sector_in_cache (block_sector_t sector, bool to_write)
{
  size_t i;
  struct cache_entry *entry;
  for (i = 0; i<BUFFER_CACHE_SIZE; i++)
  {
    entry = &buffer_cache[i];
    lock_acquire (&entry->lock);
    if (!entry->evicting && entry->sector == sector)
    {
      /* Set waiting flag to prevent the entry from bing evicted. */
      if (to_write)
        entry->waiting_writer++;
      else
        entry->waiting_reader++;
      // Actually, it's unecessary to wait for flushing in case of reading.
      while (entry->flushing)
      {
        cond_wait (&entry->ready, &entry->lock);
      }
      lock_release (&entry->lock);
      return i;
    }
    else if (entry->evicting && entry->new_sector == entry->sector)
    {
      /* Set waiting flag to prevent the entry from bing evicted. */
      if (to_write)
        entry->waiting_writer++;
      else
        entry->waiting_reader++;
      while (entry->evicting)
      {
        cond_wait (&entry->ready, &entry->lock);
      }
      lock_release (&entry->lock);
      return i;
    }
    lock_release (&entry->lock);
  }
  return -1;
}

/* Evict an entry and return its index.
 * the entry's evicting flag is set when function returns. */
static size_t
evict_entry_id (block_sector_t new_sector)
{
  struct cache_entry *entry;
  size_t cur_hand;
  while (1)
  {
    cur_hand = hand;
    entry = &buffer_cache[cur_hand];

    lock_acquire (&entry->lock);
    if (entry->waiting_reader + entry->waiting_reader + 
          entry->reader + entry->writer > 0
          || entry->flushing || entry->evicting)
    {
      lock_release (&entry->lock);
      clock_hand_increase_one ();
      continue;
    }
    if (entry->accessed)
    {
      entry->accessed = false;
      lock_release (&entry->lock);
      clock_hand_increase_one ();
      continue;
    }
    else
    {
      /* Set new_sector so that cache_read/write know that the new_sector is 
       * going to be ready when eviction finishes. */
      entry->new_sector = new_sector;
      entry->evicting = true;
      clock_hand_increase_one ();
      lock_release (&entry->lock);
      if (entry->dirty)
      {
        /* IO without holding any locks. */
        block_write (fs_device, entry->sector, entry->content);
      }
      /* Update the the dirty flag after writing back. */
      lock_acquire (&entry->lock);
      entry->dirty = false;
      lock_release (&entry->lock);
      return cur_hand;
    }
    
  }
}

static void
cache_read_hit (size_t entry_id, void *buffer, off_t start, off_t len)
{
  struct cache_entry *entry = &buffer_cache[entry_id];
  lock_acquire (&entry->lock);
  ASSERT (!entry->evicting);
  while (entry->waiting_writer + entry->writer > 0 || entry->flushing)
  {
    cond_wait (&entry->ready, &entry->lock);
  }
  entry->waiting_reader--;
  entry->reader++;
  lock_release (&entry->lock);

  memcpy (buffer, entry->content + start, len);

  lock_acquire (&entry->lock);
  entry->reader--;
  if (entry->reader == 0 && entry->waiting_writer > 0)
  {
    cond_broadcast (&entry->ready, &entry->lock);
  }
  entry->accessed = true;
  lock_release (&entry->lock);
}

static void
cache_read_miss (block_sector_t sector, void *buffer, off_t start, off_t len)
{
  size_t entry_id = evict_entry_id (sector);
  struct cache_entry *entry = &buffer_cache[entry_id];

  /* IO without holding any locks. */
  block_read (fs_device, sector, entry->content);

  lock_acquire (&entry->lock);
  entry->sector = sector;
  entry->new_sector = UINT32_MAX;
  entry->evicting = false;
  cond_broadcast (&entry->ready, &entry->lock);
  entry->waiting_reader++;
  lock_release (&entry->lock);

  cache_read_hit (entry_id, buffer, start, len);
}

void 
cache_read_partial (block_sector_t sector, void *buffer, off_t start, off_t len)
{
  int entry_id = sector_in_cache (sector, false);
  if (entry_id == -1)
    cache_read_miss (sector, buffer, start, len);
  else
    cache_read_hit (entry_id, buffer, start, len);
}

void
cache_read (block_sector_t sector, void *buffer)
{
  cache_read_partial (sector, buffer, 0, BLOCK_SECTOR_SIZE);
}





static void
cache_write_hit (size_t entry_id, const void *buffer, off_t start, off_t len)
{
  struct cache_entry *entry = &buffer_cache[entry_id];
  lock_acquire (&entry->lock);
  ASSERT (!entry->evicting);
  while (entry->reader + entry->writer > 0 || entry->flushing)
  {
    cond_wait (&entry->ready, &entry->lock);
  }
  entry->waiting_writer--;
  entry->writer++;
  lock_release (&entry->lock);

  memcpy (entry->content + start, buffer, len);

  lock_acquire (&entry->lock);
  entry->writer--;
  cond_broadcast (&entry->ready, &entry->lock);
  entry->accessed = true;
  entry->dirty = true;
  lock_release (&entry->lock);
}

static void
cache_write_miss (block_sector_t sector, const void *buffer, off_t start, off_t len, bool set_to_zero)
{
  size_t entry_id = evict_entry_id (sector);
  struct cache_entry *entry = &buffer_cache[entry_id];

  if (set_to_zero)
  {
    /* Set all content bytes to 0. */
    memset (entry->content, 0, BLOCK_SECTOR_SIZE);
  }
  else
  {
    /* IO without holding any locks. */
    block_read (fs_device, sector, entry->content);
  }

  lock_acquire (&entry->lock);
  entry->sector = sector;
  entry->new_sector = UINT32_MAX;
  entry->evicting = false;
  cond_broadcast (&entry->ready, &entry->lock);
  entry->waiting_writer++;
  lock_release (&entry->lock);

  cache_write_hit (entry_id, buffer, start, len);
}

void 
cache_write_partial (block_sector_t sector, const void *buffer, off_t start, off_t len, bool set_to_zero)
{
  int entry_id = sector_in_cache (sector, true);
  if (entry_id == -1)
    cache_write_miss (sector, buffer, start, len, set_to_zero);
  else
    cache_write_hit (entry_id, buffer, start, len);
}

void 
cache_write (block_sector_t sector, const void *buffer)
{
  cache_write_partial (sector, buffer, 0, BLOCK_SECTOR_SIZE, true);
}

