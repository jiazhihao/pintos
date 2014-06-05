#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdio.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
/* Number of indexes in inode's direct block*/
#define DIRECT_IDX_CNT (128 - 6)
/* Number of indexes in a single sector */
#define SECTOR_IDX_CNT (BLOCK_SECTOR_SIZE / 4)
/* Size of direct block in inode_disk */
#define DIRECT_BLOCK_SIZE (DIRECT_IDX_CNT * BLOCK_SECTOR_SIZE)
/* Size of single indirect block in inode_disk */
#define SINGLE_BLOCK_SIZE (SECTOR_IDX_CNT * BLOCK_SECTOR_SIZE)
/* Size of double indirect block in inode_disk */
#define DOUBLE_BLOCK_SIZE (SECTOR_IDX_CNT * SECTOR_IDX_CNT * BLOCK_SECTOR_SIZE)
/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sector;              /* Sector number of disk location. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    int isdir;                          /* 1 if this inode is dir */
    block_sector_t direct_idx[DIRECT_IDX_CNT];
                                        /* Direct indexes*/
    block_sector_t single_idx;          /* Single indirect indexes*/
    block_sector_t double_idx;          /* Double indirect indexes*/
  };

/* A sector-size block for indirect indexes*/
struct indirect_block
  {
    block_sector_t idx [SECTOR_IDX_CNT]; /* indexes in an indirect block*/
  };

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    off_t read_length;                  /* File length that can be read now. */
    struct lock inode_lock;             /* lock for inode. */
    struct lock dir_lock;               /* lock for directory. */
    struct inode_disk data;             /* Inode content. */
  };

/* API to lock INODE*/
void
inode_lock (struct inode *inode)
{
  lock_acquire (&inode->inode_lock);
}

/* API to unlock INODE*/
void
inode_unlock (struct inode *inode)
{
  lock_release (&inode->inode_lock);
}

/* Get the block_sector number in an indirect
 * block SECTOR, with index IDX
 * Return -1 if cannot get the sector #, otherwise
 * returns the sector #*/
static block_sector_t
indirect_get_sector (block_sector_t sector, int idx)
{
  struct indirect_block *block;
  block = malloc (sizeof *block);
  if (block == NULL)
    return -1;
  cache_read (sector, block);
  block_sector_t ret = block->idx[idx];
  free (block);
  return ret;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode_disk *inode, off_t pos)
{
  ASSERT (inode->length >= pos);
  ASSERT (inode != NULL);
  if (pos < DIRECT_BLOCK_SIZE)
  {
    int idx = pos / BLOCK_SECTOR_SIZE;
    return inode->direct_idx[idx];
  }
  else if (pos < DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE)
  {
    pos -= DIRECT_BLOCK_SIZE;
    int idx = pos / BLOCK_SECTOR_SIZE;
    return indirect_get_sector (inode->single_idx, idx);
  }
  else if (pos < DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE + DOUBLE_BLOCK_SIZE)
  {
    pos -= DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE;
    int idx0 = pos / SINGLE_BLOCK_SIZE;
    int idx1 = (pos % SINGLE_BLOCK_SIZE) / BLOCK_SECTOR_SIZE;
    return indirect_get_sector (
               indirect_get_sector(inode->double_idx, idx0), idx1);
  }
  else
    return -1;
}

/*
 * Allocate a sector and returns the block sector number of the
 * allocated sector. Initialize the sector with zeros if SET_ZERO*/

static block_sector_t
allocate_sector (bool set_zero)
{
  block_sector_t sector;
  if (!free_map_allocate (1, &sector))
    return -1;
  if (set_zero)
  {
    char *zero = malloc (BLOCK_SECTOR_SIZE);
    if (zero == NULL)
      return -1;
    memset(zero, 0, BLOCK_SECTOR_SIZE);
    cache_write (sector, zero);
    free (zero);
  }
  return sector;
}

static block_sector_t
allocate_indirect_block (block_sector_t first_sector)
{
  block_sector_t sector;
  if (!free_map_allocate (1, &sector))
    return -1;
  struct indirect_block *blk = malloc (sizeof *blk);
  if (blk == NULL)
    return -1;
  blk->idx[0] = first_sector;
  cache_write (sector, blk);
  free (blk);
  return sector;
}

static bool
inode_extend_single (struct inode_disk *inode)
{
  block_sector_t data_sector = allocate_sector(true);
  if ((int)data_sector == -1)
    return false;

  /* Case 1: Add a sector into direct block. */
  if (inode->length + BLOCK_SECTOR_SIZE <= DIRECT_BLOCK_SIZE)
  {
    int idx = DIV_ROUND_UP(inode->length, BLOCK_SECTOR_SIZE);
    inode->direct_idx[idx] = data_sector;
    return true;
  }
  /* Case 2: Add a sector into single indirect block. */
  else if (inode->length + BLOCK_SECTOR_SIZE
           <= DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE)
  {
    /* Case 2.1: Need to allocate new indirect block*/
    if (inode->length <= DIRECT_BLOCK_SIZE)
    {
      block_sector_t sector
        = allocate_indirect_block (data_sector);
      if ((int)sector == -1)
        return false;
      inode->single_idx = sector;
      return true;
    }
    /* Case 2.2: No need to allocate new indirect block*/
    else
    {
      int idx = DIV_ROUND_UP(inode->length - DIRECT_BLOCK_SIZE,
                         BLOCK_SECTOR_SIZE);
      struct indirect_block *blk = malloc (sizeof *blk);
      if (blk == NULL)
        return false;
      cache_read (inode->single_idx, blk);
      blk->idx[idx] = data_sector;
      cache_write (inode->single_idx, blk);
      free (blk);
      return true;
    }
  }
  /* Case 3: Add a sector into double indirect block. */
  else if (inode->length + BLOCK_SECTOR_SIZE
           <= DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE + DOUBLE_BLOCK_SIZE)
  {
    /* Case 3.1: Need to allocate double/single indirect block. */
    if (inode->length <= DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE)
    {
      block_sector_t sector1
        = allocate_indirect_block (data_sector);
      if ((int)sector1 == -1)
        return false;
      block_sector_t sector2
        = allocate_indirect_block (sector1);
      if ((int)sector2 == -1)
        return false;
      inode->double_idx = sector2;
      return true;
    }
    /* Case 3.2: No need to allocate double indirect block. */
    else
    {
      off_t ofs = inode->length - DIRECT_BLOCK_SIZE - SINGLE_BLOCK_SIZE;
      int idx1 = (ofs - 1) / SINGLE_BLOCK_SIZE;
      int idx2 = (ofs + BLOCK_SECTOR_SIZE -1) / SINGLE_BLOCK_SIZE;
      struct indirect_block *double_blk = malloc (sizeof *double_blk);
      if (double_blk == NULL)
        return false;
      cache_read (inode->double_idx, double_blk);
      /* Case 3.2.1: Need to allocate a new indirect block*/
      if (idx1 != idx2)
      {
        block_sector_t sector
          = allocate_indirect_block (data_sector);
        if ((int)sector == -1)
        {
          free (double_blk);
          return false;
        }
        double_blk->idx[idx2] = sector;
        cache_write (inode->double_idx, double_blk);
        free (double_blk);
        return true;
      }
      /* Case 3.2.2: No need to allocate new indirect block*/
      else
      {
        struct indirect_block *single_blk = malloc (sizeof *single_blk);
        if (single_blk == NULL)
        {
          free (double_blk);
          return false;
        }
        cache_read (double_blk->idx[idx1], single_blk);
        off_t ofs_lvl2 = ofs % SINGLE_BLOCK_SIZE;
        int idx_lvl2 = DIV_ROUND_UP(ofs_lvl2, BLOCK_SECTOR_SIZE);
        single_blk->idx[idx_lvl2] = data_sector;
        cache_write (double_blk->idx[idx1], single_blk);
        free (double_blk);
        free (single_blk);
        return true;
      }
    }
  }
  /* Case 4: Beyond max file length. */
  else
    return false;
}

static bool
inode_extend_file (struct inode_disk *inode, off_t length)
{
  //printf("inode_extend_file(BEGIN)\n");
  off_t cur_left = ROUND_UP (inode->length, BLOCK_SECTOR_SIZE)
                   - inode->length;
  off_t extend_len = length - inode->length;

  /* Check if the length exceeds file size limitation*/
  if (length > DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE + DOUBLE_BLOCK_SIZE)
    return false;

  /* In case no need to allocate new sectors. */
  if (cur_left >= extend_len)
  {
    inode->length = length;
    //printf("inode_extend_file(END)\n");
    cache_write(inode->sector, inode);
    return true;
  }

  inode->length = ROUND_UP (inode->length, BLOCK_SECTOR_SIZE);
  while (inode->length < length)
  {
    if (!inode_extend_single (inode))
    {
      //printf("inode_extend_file(END)\n");
      return false;
    }
    inode->length += BLOCK_SECTOR_SIZE;
  }
  inode->length = length;
  cache_write(inode->sector, inode);
  //printf("inode_extend_file(END)\n");
  return true;
}

static void
free_inode_disk (struct inode_disk *inode)
{
  off_t length = ROUND_UP (inode->length, BLOCK_SECTOR_SIZE);
  off_t cur;
  block_sector_t sector;

  /*Free sectors that store file data*/
  for (cur = 0; cur < length; cur = cur + BLOCK_SECTOR_SIZE)
  {
    sector = byte_to_sector (inode, cur);
    free_map_release (sector, 1);
  }

  /*Free sector for indirect block if any*/
  if (inode->length > DIRECT_BLOCK_SIZE)
    free_map_release (inode->single_idx, 1);

  /*Free sectors for double indirect blocks if any*/
  if (inode->length > DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE)
  {
    struct indirect_block block;
    cache_read (inode->double_idx, &block);
    int idx = 0;
    for (cur = DIRECT_BLOCK_SIZE + SINGLE_BLOCK_SIZE;
         cur < length; cur = cur + SINGLE_BLOCK_SIZE)
      free_map_release (block.idx[idx++], 1);
    free_map_release (inode->double_idx, 1);
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
/* lock for open inode list to avoid race condition. */
static struct lock open_inodes_lock;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
  cache_init ();
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
{
  //printf("inode_create(BEGIN)\n");
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->sector = sector;
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->isdir = isdir? 1 : 0;
      if (!inode_extend_file (disk_inode, length))
      {
        free_inode_disk (disk_inode);
        free (disk_inode);
        return false;
      }
      cache_write (sector, disk_inode);
      free (disk_inode);
      success = true;
    }
  //printf("inode_create(END)\n");
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire (&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          lock_release (&open_inodes_lock);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    lock_release (&open_inodes_lock);
    return NULL;
  }
  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->inode_lock);
  lock_init (&inode->dir_lock);
  lock_release (&open_inodes_lock);
  cache_read (inode->sector, &inode->data);
  inode->read_length = inode->data.length;
  //printf("inode_open: length = %d\n", inode->read_length);
  //printf("inode->direct_idx[0] = %d\n", inode->data.direct_idx[0]);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire (&inode->inode_lock);
  //printf("read_length = %d, length = %d\n", inode->read_length, inode->data.length);
  //printf("inode->direct_idx[0] = %d\n", inode->data.direct_idx[0]);
  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    lock_acquire (&open_inodes_lock);
    /* Remove from inode list and release lock. */
    list_remove (&inode->elem);
    lock_release (&open_inodes_lock);

    /* Deallocate blocks if removed. */
    if (inode->removed)
    {
      free_inode_disk (&inode->data);
      /*Free sector for inode_disk*/
      free_map_release (inode->sector, 1);
    }
    /* otherwise, we write inode->data back to disk */
    else
    {
      cache_write (inode->sector, &inode->data);
    }
    lock_release (&inode->inode_lock);
    free (inode);
  }
  else
  {
    lock_release (&inode->inode_lock);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  if (offset >= inode_length (inode))
    return 0;
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (&inode->data, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_read_partial (sector_idx, buffer + bytes_read, 
                              sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  /* Read-ahead if there is sector left in the file. */
  int sector_ofs = offset % BLOCK_SECTOR_SIZE;
  off_t inode_left = inode_length (inode) - offset;
  int sector_left = sector_ofs>0? (BLOCK_SECTOR_SIZE - sector_ofs) : 0;
  if (inode_left > sector_left)
  {
    block_sector_t sector_idx = byte_to_sector (&inode->data, offset + sector_left);
    cache_read_ahead (sector_idx);
  }
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
  {
    //printf("Deny write!!!\n");
    return 0;
  }

  while (size > 0) 
    {
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < sector_left ? size : sector_left;

      /* Acquire inode_lock to avoid race condition in which case another
       * process may extend this inode at the same time. The length of the
       * file is extended in a progressive way, therefore if we are going
       * to extend this file, we need to hold this per inode lock till the
       * end of writing.*/
      lock_acquire (&inode->inode_lock);
      if (offset + chunk_size > inode->data.length)
        inode_extend_file (&inode->data, offset + chunk_size);
      lock_release (&inode->inode_lock);
      
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (&inode->data, offset);
      
      /* If the sector contains data before or after the chunk
         we're writing, then we need to read in the sector
         first.  Otherwise we start with a sector of all zeros. */
      bool set_to_zero = !(sector_ofs > 0 || chunk_size < sector_left);
      //printf("write: sec = %d, ofs = %d, size = %d\n", sector_idx, sector_ofs, chunk_size);
      cache_write_partial (sector_idx, buffer + bytes_written, sector_ofs, 
                           chunk_size, set_to_zero);

      /* After we have written the extended data, we can let readers see this part of file, simply by
       * setting read_length to be equal to real length.*/
      lock_acquire (&inode->inode_lock);
      inode->read_length = inode->data.length;
      lock_release (&inode->inode_lock);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->inode_lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->inode_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire (&inode->inode_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release (&inode->inode_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->read_length;
}

void lock_dir (struct inode *inode)
{
  lock_acquire (&inode->dir_lock);
}

void unlock_dir (struct inode *inode)
{
  lock_release (&inode->dir_lock);
}

bool inode_isdir (struct inode *inode)
{
  return inode->data.isdir != 0;
}

int inode_open_cnt (struct inode *inode)
{
  return inode->open_cnt;
}
