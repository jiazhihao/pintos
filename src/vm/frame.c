#include "vm/frame.h"

void 
frame_table_init (struct frame_table *ft, size_t page_cnt, void *base,
                  size_t block_size)
{
  ASSERT(block_size >= frame_table_size (page_cnt));
  ft->size = page_cnt;
  ft->frames = (struct fte *) base;
  size_t i;
  for (i = 0; i < page_cnt; i ++)
  {
    ft->frames[i].thread = NULL;
    ft->frames[i].vaddr = NULL;
  }
}

size_t
frame_table_size (size_t page_cnt)
{
  return page_cnt * sizeof (struct fte);
}

