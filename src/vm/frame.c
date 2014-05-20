#include "vm/frame.h"
#include "threads/palloc.h"
#include <stdio.h>

void 
frame_init (void *base, size_t page_cnt)
{
  printf ("frame_init\n");
  frame_table.size = page_cnt;
  frame_table.frames = (struct fte *) base;
  size_t i;
  for (i = 0; i < page_cnt; i ++)
  {
    frame_table.frames[i].thread = NULL;
    frame_table.frames[i].pte = NULL;
  }
}

size_t
frame_table_size (size_t page_cnt)
{
  return page_cnt * sizeof (struct fte);
}

void
frame_free_multiple (void *pages, size_t page_cnt)
{
  // TODO (rqi) assert pages are from user pool
  palloc_free_multiple (pages, page_cnt);
 
  size_t page_idx = pg_no (pages) - pg_no (user_pool.base);
  size_t i;
  for (i = page_idx; i < page_idx + page_cnt; i++)
  {
    frame_table.frames[i].thread = 0;
    frame_table.frames[i].pte = 0;
  }
}

void
frame_free_page (void *page)
{
  frame_free_multiple (page,1);
}

/* Allocate one page for user and set spt accordingly.
   If no physical page is available, run eviction algo. to get a page. */
void *
frame_get_page (enum frame_flags flags, void *upage)
{
  ASSERT (flags & FRM_USER);
  ASSERT (pg_ofs (upage) == 0);
  //ASSERT (upage < PHYS_BASE);

  void *kpage = palloc_get_multiple (flags, 1);
  if (kpage == NULL)
  {
    PANIC ("frame_get: out of pages");
    // TODO(rqi), call eviction algo. if run out of phy. pages.
    //kpage = evict_and_get_page (...);
  }
  // TODO(rqi), move all frame-related operations into frame.c

  struct thread *t = thread_current ();
  size_t page_idx = pg_no (kpage) - pg_no (user_pool.base);
  printf ("user_pool.base: %u\n", user_pool.base);
  printf ("page_idx: %u\n", page_idx);
  frame_table.frames[page_idx].thread = t;
  frame_table.frames[page_idx].pte = lookup_page (t->pagedir, upage, false);

  return kpage;
}
