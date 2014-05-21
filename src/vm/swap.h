#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include <bitmap.h>
#include <stdio.h>

/* Number of sectors per page = 4kB / 512B = 8. */
#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

/* Swap talbe used to track used disk space. */
struct swap_table
{
  struct block *swap_block;    /* Swap block for paging. */
  struct bitmap *used_map;     /* Bitmap for free swap pages. */
  struct lock block_lock;      /* Lock to protect swap_block. */
  struct lock bitmap_lock;     /* Lock to protect used_map. */
};

struct swap_table swap_table;  /* Global table tracking swap frames in swap block. */

void swap_table_init (struct swap_table *);
size_t swap_get_page (struct swap_table *);
void swap_free_page (struct swap_table *, size_t swap_page_no);
void swap_read_page (struct swap_table *, size_t swap_page_no, void *buf);
void swap_write_page (struct swap_table *, size_t swap_page_no, void *buf);

#endif
