#include "vm/frame.h"
#include "threads/palloc.h"
#include <stdio.h>
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"
#include "threads/pte.h"
#include "threads/synch.h"
#include <string.h>

void 
frame_init (void *base, size_t page_cnt)
{
  frame_table.size = page_cnt;
  frame_table.clock_hand = 0;
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
  size_t i;
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

static inline void
clock_hand_increase_one ()
{
  frame_table.clock_hand = (frame_table.clock_hand + 1) % frame_table.size;
}

/* Evict a frame, write back if necessary, update PTE and SPTE.
   Returns free-to-use kpage. */ 
// TODO (rqi) consider race conditions on frames
static void *
evict_and_get_page (enum frame_flags flags)
{
  uint32_t *pte;
  struct spte *spte;
  void *kpage;
  struct thread *cur = thread_current ();

  while (1) 
  {
    pte = frame_table.frames[frame_table.clock_hand].pte;
    kpage = user_pool.base + frame_table.clock_hand * PGSIZE;
    ASSERT (pte != NULL);

    /* Case 1: if the page is pinned, skip it. */
    if (*pte & PTE_I)
    {
      clock_hand_increase_one ();
      continue;
    }
    /* Case 2: if the page is accessed recently, reset access bit and skip it. */
    if (*pte & PTE_A)
    {
      *pte &= ~PTE_A;
      clock_hand_increase_one ();
      continue;
    }
    else /* Case 3: if PTE_A is 0 */
    {
      /* Case 3.1: if the page is dirty. */
      if (*pte & PTE_D)
      {
        /* mmaped file. */
        if ((*pte & PTE_F) && !(*pte & PTE_E))
        {
          // 1. Write page back and set Dirty bit to 0
          // 2. Increase clock hand by 1 and continue.
        }
      } 
      else /* Case 3.2: if the page is neither accessed or dirty. swap it! */
      {
        /* Update PTE */
        // ? *pte |= PTE_A;
        *pte &= ~PTE_P;
        *pte &= PTE_FLAGS;
        /* Get one slot from swap block and write page to swap. */
        size_t swap_page_no = swap_get_page (&swap_table);
        swap_write_page (&swap_table, swap_page_no, kpage);
        /* Update SPTE. */
        lock_acquire (&cur->spt.lock);
        spte = spt_find (&cur->spt, pte);
        spte->daddr.swap_addr = swap_page_no;
        lock_release (&cur->spt.lock);
      }
    } /* End of case 3. */
    
    if (flags & FRM_ZERO)
      memset (kpage, 0, PGSIZE);
    return kpage;
  } /* End of while. */
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
    //PANIC ("frame_get: out of pages");
    kpage = evict_and_get_page (flags);
  }

  struct thread *t = thread_current ();
  size_t page_idx = pg_no (kpage) - pg_no (user_pool.base);
  frame_table.frames[page_idx].thread = t;
  frame_table.frames[page_idx].pte = pte;

  return kpage;
}
