#include "vm/swap.h"

/* Init the swap_table. */
void 
swap_table_init (struct swap_table *swap_table)
{
  swap_table->swap_block = block_get_role (BLOCK_SWAP);
  ASSERT (swap_table->swap_block != NULL);
  
  int pages_in_block = block_size (swap_table->swap_block) / SECTORS_PER_PAGE;
  swap_table->used_map = bitmap_create (pages_in_block);

  lock_init (&swap_table->block_lock);
  lock_init (&swap_table->bitmap_lock);
}

/* Get a free page-size disk space from swap block.
   returns the swap_page_no. */
size_t 
swap_get_page (struct swap_table *swap_table)
{
  lock_acquire (&swap_table->bitmap_lock);
  size_t swap_page_no = bitmap_scan_and_flip (swap_table->used_map, 
                                              0, 1, false); 
  lock_release (&swap_table->bitmap_lock);
  if (swap_page_no == BITMAP_ERROR)
    PANIC ("Swap block is full.");
  return swap_page_no;
}

/* Set the page in swap block as free. */
void 
swap_free_page (struct swap_table *swap_table, size_t swap_page_no)
{
  lock_acquire (&swap_table->bitmap_lock);
  bitmap_set (swap_table->used_map, swap_page_no, false);
  lock_release (&swap_table->bitmap_lock); 
}

/* Read a page from swap block (disk) to buf. */
void 
swap_read_page (struct swap_table *swap_table, size_t swap_page_no, void *buf)
{
  ASSERT (bitmap_test (swap_table->used_map, swap_page_no) == true);
  int i;
  lock_acquire (&swap_table->block_lock);
  for (i = 0; i < SECTORS_PER_PAGE; i++)
  {
    block_read (swap_table->swap_block, swap_page_no*SECTORS_PER_PAGE+i, buf);
    buf += BLOCK_SECTOR_SIZE;
  } 
  lock_release (&swap_table->block_lock);
}

/* Write a page to swap block (disk) from buf. */
void
swap_write_page (struct swap_table *swap_table, size_t swap_page_no, void *buf)
{
  ASSERT (bitmap_test (swap_table->used_map, swap_page_no) == true);
  int i;
  lock_acquire (&swap_table->block_lock);
  for (i = 0; i < SECTORS_PER_PAGE; i++)
  {
    block_write (swap_table->swap_block, swap_page_no*SECTORS_PER_PAGE+i, buf);
    buf += BLOCK_SECTOR_SIZE;
  } 
  lock_release (&swap_table->block_lock);
}
