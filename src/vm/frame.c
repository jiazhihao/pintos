#include "vm/frame.h"
#include "threads/palloc.h"

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

/* Allocate one page for user and set spt accordingly.
   If no physical page is available, run eviction algo. to get a page. */
void *
frame_get_page (enum frame_flags flags)
{
  ASSERT (flags & FRM_USER);
  void *kpage = palloc_get_multiple (flags, 1);
  if (kpage == NULL)
  {
    PANIC ("frame_get: out of pages");
    // TODO(rqi), call eviction algo. if run out of phy. pages.
    //kpage = evict_and_get_page (...);
  }
  // TODO(rqi), move all frame-related operations into frame.c
  return kpage;
}
