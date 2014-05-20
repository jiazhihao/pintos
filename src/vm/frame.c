#include "vm/frame.h"
#include "threads/palloc.h"
#include <stdio.h>
#include "userprog/pagedir.h"

void 
frame_init (void *base, size_t page_cnt)
{
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
  void *pg = pages;
  int i;
  for (i = 0; i<page_cnt; i++)
  {
    ASSERT (page_from_pool (&user_pool, pg));
    pg += PGSIZE;
  }
  
  /* Reset frame table before resetting bitmap. */
  size_t page_idx = pg_no (pages) - pg_no (user_pool.base);
  for (i = page_idx; i < page_idx + page_cnt; i++)
  {
    frame_table.frames[i].thread = NULL;
    frame_table.frames[i].pte = NULL;
  }

  palloc_free_multiple (pages, page_cnt);
 
}

void
frame_free_page (void *page)
{
  frame_free_multiple (page,1);
}

/* Allocate one page for user and set spt accordingly.
   If no physical page is available, run eviction algo. to get a page. */
void *
frame_get_page (enum frame_flags flags, uint32_t *pte)
{
  ASSERT (flags & FRM_USER);
  ASSERT (pte != NULL);

  void *kpage = palloc_get_multiple (flags, 1);
  if (kpage == NULL)
  {
    PANIC ("frame_get: out of pages");
    // TODO(rqi), call eviction algo. if run out of phy. pages.
    //kpage = evict_and_get_page (...);
  }

  struct thread *t = thread_current ();
  size_t page_idx = pg_no (kpage) - pg_no (user_pool.base);
  frame_table.frames[page_idx].thread = t;
  frame_table.frames[page_idx].pte = pte;

  return kpage;
}
