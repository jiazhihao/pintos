#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#include <stdint.h>
#include <string.h>
#include <list.h>
#include <stdio.h>
#include <debug.h>

#define BUFFER_CACHE_SIZE 64           /* Number of cache entries. */
#define CACHE_FLUSH_PERIOD 10          /* Flush period in second. */
#define CACHE_FLUSH_PERIOD_TICKS (TIMER_FREQ * CACHE_FLUSH_PERIOD)

/* Cache entry for the file blocks' cache, i.e. buffer_cache. */
struct cache_entry
{
  block_sector_t sector;               /* Sector number of the cached block. */
  block_sector_t new_sector;           /* Sector no of block after eviction. */
  bool accessed;                       /* Whether has been accessed. */
  bool dirty;                          /* Whether has been written. */
  bool evicting;                       /* Entry being evicted. */
  bool flushing;                       /* Entry being flushed. */
  size_t reader;                       /* Number of active readers. */
  size_t writer;                       /* Number of active writers. */
  size_t waiting_reader;               /* Number of waiting readers. */
  size_t waiting_writer;               /* Number of waiting writers. */
  struct lock lock;                    /* Per entry lock. */
  struct condition ready;              /* Per entry condition variable. */
  uint8_t content[BLOCK_SECTOR_SIZE];  /* Block content. */
};

/* Buffer cache of file blocks. 
 * In default, as many as 64 blocks can be cached. */
static struct cache_entry buffer_cache[BUFFER_CACHE_SIZE];
/* Global buffer cache lock to prevent two threads from evicting 2 entries
 * for the same sector, i.e. gurantee that there is only ONE cache entry for 
 * each sector. Concurrency is kept by releasing the lock before IO. */
static struct lock buffer_cache_lock;

/* Clock hand for eviction algorithm. */
static size_t hand;
static struct lock hand_lock;

/* Read-ahead task struct. */
struct read_ahead_task
{
  block_sector_t sector;
  struct list_elem elem;
};
/* Read-ahead task queue. */
static struct list read_ahead_list;
static struct lock read_ahead_lock;
static struct condition read_ahead_ready;


/* Atmoically increase clock hand by one. */
static void
clock_hand_increase_one (void)
{
  lock_acquire (&hand_lock);
  hand = (hand + 1) % BUFFER_CACHE_SIZE;
  lock_release (&hand_lock);
}

static void periodic_flush_daemon (void *aux);
static void read_ahead_daemon (void *aux);

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
    memset (entry->content, 0, BLOCK_SECTOR_SIZE);
  }
  hand = 0;
  list_init (&read_ahead_list);
  lock_init (&buffer_cache_lock);
  lock_init (&hand_lock);
  lock_init (&read_ahead_lock);
  cond_init (&read_ahead_ready);
  thread_create ("perodic_flush", PRI_DEFAULT, periodic_flush_daemon, NULL);
  thread_create ("read_ahead", PRI_DEFAULT, read_ahead_daemon, NULL);
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
    }
    lock_release (&entry->lock);
  }
}

/* Background thread that periodically flush the cache. */
static void
periodic_flush_daemon (void *aux UNUSED)
{
  while (1)
  {
    timer_sleep (CACHE_FLUSH_PERIOD_TICKS);
    cache_flush ();
  }
}

/* Helper function to check if a sector is already in read-ahead task queue. */
static bool
sector_in_ra_queue (block_sector_t sector)
{
  struct read_ahead_task *task;
  struct list_elem *e;
  for (e = list_begin (&read_ahead_list); e != list_end (&read_ahead_list);
       e = list_next (e)) 
  {
    task = list_entry (e, struct read_ahead_task, elem);
    if (task->sector == sector) 
    {
      return true; 
    }
  }
  return false;
}

/* Read-ahead of a sector, returns immediately. */
void
cache_read_ahead (block_sector_t sector)
{
  lock_acquire (&read_ahead_lock);
  /* Check whether this sector is already in read-ahead queue. 
   * If it is, return directly. otherwise, push it to end of queue. */
  if (sector_in_ra_queue (sector))
  {
    lock_release (&read_ahead_lock);
    return;
  }
  struct read_ahead_task *task;
  task = (struct read_ahead_task *) malloc (sizeof (struct read_ahead_task));
  task->sector = sector;
  list_push_back (&read_ahead_list, &task->elem);
  cond_signal (&read_ahead_ready, &read_ahead_lock);
  lock_release (&read_ahead_lock);
}

/* Cancel read_ahead task with the same sector number by
 * removing it from read_ahead queue and free its memory. */
static void
read_ahead_cancel (block_sector_t sector)
{
  lock_acquire (&read_ahead_lock);
  struct read_ahead_task *task;
  struct list_elem *e;
  for (e = list_begin (&read_ahead_list); e != list_end (&read_ahead_list);
       e = list_next (e)) 
  {
    task = list_entry (e, struct read_ahead_task, elem);
    if (task->sector == sector) 
    {
      list_remove (e);
      free (task);
      lock_release (&read_ahead_lock);
      return; 
    }
  }
  lock_release (&read_ahead_lock);
}

static int sector_in_cache (block_sector_t sector, bool to_write);
static void cache_read_hit (size_t entry_id, void *buffer, off_t start, off_t len);
static size_t evict_entry_id (block_sector_t new_sector);
static bool wait_until_sector_flushed (block_sector_t sector);

/* Background thread that is in charge of prefetching. */
static void 
read_ahead_daemon (void *aux UNUSED)
{
  while (1)
  {
    lock_acquire (&read_ahead_lock);
    while (list_empty (&read_ahead_list))
    {
      cond_wait (&read_ahead_ready, &read_ahead_lock);
    }
    struct list_elem *front = list_pop_front (&read_ahead_list);
    struct read_ahead_task *task = list_entry (front, struct read_ahead_task, 
                                               elem);
    block_sector_t sector = task->sector;
    free (task);
    lock_release (&read_ahead_lock);

    void *buffer = malloc (BLOCK_SECTOR_SIZE);
    cache_read (sector, buffer);
/*
//////////////////////////////////////////////////
    lock_acquire (&buffer_cache_lock);
    int entry_id = sector_in_cache (sector, false);
    if (entry_id == -1)
    {
      printf ("### Before evict_entry_id ...\n");
      ASSERT (lock_held_by_current_thread(&buffer_cache_lock));
      entry_id = evict_entry_id (sector);
      printf ("### After evict_entry_id of sector: %u...\n", buffer_cache[entry_id].sector);
      struct cache_entry *entry = &buffer_cache[entry_id];
      printf ("### Before block_read ...\n");

      printf ("1before wait sector: %u\n", sector);
      wait_until_sector_flushed (sector);
      printf ("1after wait sector: %u\n", sector);
      block_read (fs_device, sector, entry->content);
      printf ("### After block_read ...\n");

      lock_acquire (&entry->lock);
      entry->sector = sector;
      entry->new_sector = UINT32_MAX;
      entry->dirty = false;
      entry->accessed = false;
      entry->evicting = false;
      cond_broadcast (&entry->ready, &entry->lock);
      entry->waiting_reader++;
      lock_release (&entry->lock);
    }
    printf ("### Before cache_read_hit ...\n");
    cache_read_hit (entry_id, buffer, 0, BLOCK_SECTOR_SIZE);
    printf ("### After cache_read_hit ...\n");
 //////////////////////////////////////////////////
*/

    free (buffer);
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
    if (entry->sector == sector && !entry->evicting)
    {
      /* Set waiting flag to prevent the entry from bing evicted. */
      if (to_write)
        entry->waiting_writer++;
      else
        entry->waiting_reader++;
      /* Release buffer_cache_lock before cond_wait. */
      lock_release (&buffer_cache_lock);
      while (entry->flushing)
      {
        cond_wait (&entry->ready, &entry->lock);
      }
      lock_release (&entry->lock);
      return i;
    }
    else if (entry->new_sector == sector && entry->evicting)
    {
      /* Set waiting flag to prevent the entry from bing evicted. */
      if (to_write)
        entry->waiting_writer++;
      else
        entry->waiting_reader++;
      /* Release buffer_cache_lock before cond_wait. */
      lock_release (&buffer_cache_lock);
      while (entry->evicting)
      {
        cond_wait (&entry->ready, &entry->lock);
      }
      lock_release (&entry->lock);
      return i;
    }
    if (entry->evicting && entry->sector == sector)
    {
      /*
      while (entry->flushing)
      {
        cond_wait (&entry->ready, &entry->lock);
      }
      lock_release (&entry->lock);
      return -1;
      */
      //printf ("@sector: %u is being evicted. to_write: %d\n", sector, to_write);
    }
    lock_release (&entry->lock);
  }
  /* Miss: buffer_cache_lock will be released in evict_entry_id. */
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
    if (entry->waiting_reader + entry->waiting_writer + 
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
    else /* Evictable entry found! */
    {
      /* Set new_sector so that cache_read/write know that the new_sector is 
       * going to be ready when eviction finishes. */
      entry->new_sector = new_sector;
      entry->evicting = true;
      if (entry->dirty)
      {
        /* Set flushing flag if the sector needs to be writen back. 
         * a sector cannot be read if it being flushed. */
        entry->flushing = true;
        lock_release (&entry->lock);
        lock_release (&buffer_cache_lock);

        /* IO without holding any locks. */
        block_write (fs_device, entry->sector, entry->content);

        lock_acquire (&entry->lock);
        entry->dirty = false;
        entry->flushing = false;
        cond_broadcast (&entry->ready, &entry->lock);
        lock_release (&entry->lock);
      }
      else
      {
        lock_release (&entry->lock);
        lock_release (&buffer_cache_lock);
      }      
      clock_hand_increase_one ();
      return cur_hand;
    }
  }
}

/* Cache read hit routine: acquire read lock, memcpy, release read lock. */
static void
cache_read_hit (size_t entry_id, void *buffer, off_t start, off_t len)
{
  struct cache_entry *entry = &buffer_cache[entry_id];
  lock_acquire (&entry->lock);
  while (entry->waiting_writer + entry->writer > 0 || entry->flushing)
  {
    cond_wait (&entry->ready, &entry->lock);
  }
  entry->waiting_reader--;
  entry->reader++;
  lock_release (&entry->lock);

  //printf ("read sector: %u\n", entry->sector);
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

/* Helper function. Make sure the sector is not being flushed 
 * from another entry in another thread. */
static bool
wait_until_sector_flushed (block_sector_t sector)
{
  size_t i;
  struct cache_entry *entry;
  for (i = 0; i<BUFFER_CACHE_SIZE; i++)
  {
    entry = &buffer_cache[i];
    lock_acquire (&entry->lock);
    if (entry->sector == sector)
    {
      //if (!entry->evicting)
      //{
       // printf ("~~ wait, flushing: %d, sector: %u, next_sector: %u\n", entry->flushing, entry->sector, entry->new_sector);
      //}
      ASSERT (entry->evicting);

      while (entry->flushing)
      {
        cond_wait (&entry->ready, &entry->lock);
      }
      lock_release (&entry->lock);
      return true;
    }
    lock_release (&entry->lock);
  }
  return false;
}

/* Cache read miss routine: evict, block_read, set entry metadata, 
 * call cache_read_hit. */
static void
cache_read_miss (block_sector_t sector, void *buffer, off_t start, off_t len)
{
  size_t entry_id = evict_entry_id (sector);
  struct cache_entry *entry = &buffer_cache[entry_id];

  /* Before IO, make sure the sector has been flushed to disk. */
  //printf ("before_r wait sector: %u\n", sector);
  bool flag = wait_until_sector_flushed (sector);
  //printf ("after_r wait sector: %u, %d\n", sector, flag);
  /* IO without holding any locks. */
  block_read (fs_device, sector, entry->content);

  lock_acquire (&entry->lock);
  entry->sector = sector;
  entry->new_sector = UINT32_MAX;
  entry->accessed = false;
  entry->evicting = false;
  cond_broadcast (&entry->ready, &entry->lock);
  entry->waiting_reader++;
  lock_release (&entry->lock);

  cache_read_hit (entry_id, buffer, start, len);
}

/* Read part of or entire disk sector. */
void 
cache_read_partial (block_sector_t sector, void *buffer, 
                    off_t start, off_t len)
{
  /* Cancel read_ahead task with the same sector number. */
  read_ahead_cancel (sector);

  /* Lock released in sector_in_cache before cond_wait (entry found)
   * or in evict_entry_id before IO (entry not found). 
   * Since no thread acquires buffer_cache_lock while holding
   * entry lock, there is no deadlock. */
  lock_acquire (&buffer_cache_lock);
  int entry_id = sector_in_cache (sector, false);
  if (entry_id == -1)
    cache_read_miss (sector, buffer, start, len);
  else
    cache_read_hit (entry_id, buffer, start, len);
}

/* Read entire disk sector. */
void
cache_read (block_sector_t sector, void *buffer)
{
  cache_read_partial (sector, buffer, 0, BLOCK_SECTOR_SIZE);
}




/* Cache write hit routine: acquire write lock, memcpy, release write lock. */
static void
cache_write_hit (size_t entry_id, const void *buffer, off_t start, off_t len)
{
  struct cache_entry *entry = &buffer_cache[entry_id];
  lock_acquire (&entry->lock);
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

/* Cache write miss routine: evict, memset or block_read, set entry metadata,
 * call cache_write_hit. */
static void
cache_write_miss (block_sector_t sector, const void *buffer, off_t start, off_t len, bool set_to_zero)
{
  size_t entry_id = evict_entry_id (sector);
  struct cache_entry *entry = &buffer_cache[entry_id];

  if (set_to_zero)
  {
    wait_until_sector_flushed (sector);
    /* Set all content bytes to 0. */
    memset (entry->content, 0, BLOCK_SECTOR_SIZE);
  }
  else
  {      
    /* Before IO, make sure the sector has been flushed to disk. */
    //printf ("before_w wait sector: %u\n", sector);
    wait_until_sector_flushed (sector);
    //printf ("after_w wait sector: %u\n", sector);
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

/* Write part of or entire disk sector. */
void 
cache_write_partial (block_sector_t sector, const void *buffer, off_t start, off_t len, bool set_to_zero)
{
  /* lock released in sector_in_cache before cond_wait (entry found)
   * or in evict_entry_id before IO (entry not found). 
   * Since no thread acquires buffer_cache_lock while holding
   * entry lock, there is no deadlock. */
  lock_acquire (&buffer_cache_lock);
  int entry_id = sector_in_cache (sector, true);
  if (entry_id == -1)
    cache_write_miss (sector, buffer, start, len, set_to_zero);
  else
    cache_write_hit (entry_id, buffer, start, len);
}

/* Write entire disk sector. */
void 
cache_write (block_sector_t sector, const void *buffer)
{
  cache_write_partial (sector, buffer, 0, BLOCK_SECTOR_SIZE, true);
}

