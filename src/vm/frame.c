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
#include "userprog/syscall.h"
#include "filesys/file.h"

extern struct lock filesys_lock;

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
    lock_init (&frame_table.frames[i].lock);
  }
  lock_init (&frame_table.clock_lock);
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
    lock_acquire (&frame_table.frames[i].lock);
    frame_table.frames[i].thread = NULL;
    frame_table.frames[i].pte = NULL;
    lock_release (&frame_table.frames[i].lock);
  }

  palloc_free_multiple (pages, page_cnt);
 
}

void
frame_free_page (void *page)
{
  frame_free_multiple (page,1);
}

static inline void
clock_hand_increase_one (void)
{
  lock_acquire (&frame_table.clock_lock); 
  frame_table.clock_hand = (frame_table.clock_hand + 1) % frame_table.size;
  lock_release (&frame_table.clock_lock); 
}

/* Evict a frame, write back if necessary, update PTE and SPTE.
   Returns free-to-use kpage. */ 
// TODO (rqi) consider race conditions on frames
static void *
evict_and_get_page (enum frame_flags flags)
{
  struct fte *fte;
  uint32_t *pte;
  struct spte *spte;
  void *kpage;

  while (1) 
  {
    lock_acquire (&frame_table.clock_lock); 
    fte = &frame_table.frames[frame_table.clock_hand];
    kpage = user_pool.base + frame_table.clock_hand * PGSIZE;
    lock_release (&frame_table.clock_lock);

    lock_acquire (&fte->lock);
    pte = fte->pte;
    ASSERT ((fte->thread != NULL));    

    /* Case 1: if the page or frame is pinned, skip it.
       fte->pte==NULL means the fte is not yet set (i.e. fte is pinned) */
    if ((pte == NULL) || (*pte & PTE_I))
    {
      clock_hand_increase_one ();
      lock_release (&fte->lock);
      continue;
    }
    /* Case 2: if the page is accessed recently, reset access bit and skip it. */
    if (*pte & PTE_A)
    {
      *pte &= ~PTE_A;
      clock_hand_increase_one ();
      lock_release (&fte->lock);
      continue;
    }
    bool is_mmap_page = (*pte & PTE_F) && !(*pte & PTE_E);
    bool is_exec_page = (*pte & PTE_F) && (*pte & PTE_E);
    lock_acquire (&fte->thread->spt.lock);
    spte = spt_find (&fte->thread->spt, pte);
    bool has_swap_page = !(*pte & PTE_F) && (spte != NULL);
    /* Case 3: if unaccessed but dirty. */
    if (*pte & PTE_D)
    {
      /* Case 3.1: mmaped file. */
      if (is_mmap_page)
      {
        spte = spt_find (&fte->thread->spt, pte);
        ASSERT ((spte != NULL) && (spte->daddr.file_meta.file != NULL));
        struct file_meta *fm = &spte->daddr.file_meta;
        lock_acquire (&filesys_lock);
        file_write_at (fm->file, kpage, fm->read_bytes, fm->offset);
        lock_release (&filesys_lock);
      }
      /* Case 3.2: exec. file or non-file. without swap_page*/
      if (!is_mmap_page && !has_swap_page)
      {
        size_t swap_page_no = swap_get_page (&swap_table);
        swap_write_page (&swap_table, swap_page_no, kpage);
        spte = spt_find (&fte->thread->spt, pte);
        if (spte == NULL)
        {
          union daddr daddr;
          daddr.swap_addr = swap_page_no;
          spte = spt_insert (&fte->thread->spt, pte, &daddr);
          if (spte == NULL) {
            lock_release (&fte->thread->spt.lock);
            lock_release (&fte->lock);
            _exit (-1);
          }
        }
        spte->daddr.swap_addr = swap_page_no;
        /* Once swapped, read from swap next time. */
        *pte &= ~PTE_F;
        *pte &= ~PTE_E;
      }
      /* Case 3.3: exec. file or non-file with swap_page */
      if (!is_mmap_page && has_swap_page)
      {
        spte = spt_find (&fte->thread->spt, pte);
        ASSERT ((spte != NULL) && (spte->daddr.swap_addr != 0));
        swap_write_page (&swap_table, spte->daddr.swap_addr, kpage);  
      }
      *pte &= ~PTE_D;
      clock_hand_increase_one ();
      lock_release (&fte->thread->spt.lock);
      lock_release (&fte->lock);
      continue;
    }
    /* At this point, a evictable frame has been found. */ 

    /* Case 4: the page is neither accessed nor dirty. swap it! */
    /* Case 4.1: mmaped file. */
    if (is_mmap_page)
    {
    }
    /* Case 4.2: exec. file or non-file. without swap_page*/
    /* Case 4.2.1: exec. file. No need to write back. */
    if (!is_mmap_page && is_exec_page && !has_swap_page)
    {
    }
    /* Case 4.2.2: non-file. Write to swap. */
    if (!is_mmap_page && !is_exec_page && !has_swap_page)
    {
      size_t swap_page_no = swap_get_page (&swap_table);
      swap_write_page (&swap_table, swap_page_no, kpage);
      spte = spt_find (&fte->thread->spt, pte);
      if (spte == NULL)
      {
        union daddr daddr;
        daddr.swap_addr = swap_page_no;
        spte = spt_insert (&fte->thread->spt, pte, &daddr);
        if (spte == NULL) {
          lock_release (&fte->thread->spt.lock);
          lock_release (&fte->lock);
          _exit (-1);
        }
      }
      spte->daddr.swap_addr = swap_page_no;
    }
    /* Case 4.3: exec. file or non-file with swap_page */
    if (!is_mmap_page && has_swap_page)
    {
    }

    *pte |= PTE_A;
    *pte &= ~PTE_P;
    *pte &= PTE_FLAGS; 
    fte->thread = NULL;
    fte->pte = NULL; 
    lock_release (&fte->thread->spt.lock);
    lock_release (&fte->lock);
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
    kpage = evict_and_get_page (flags);
  }

  struct thread *t = thread_current ();
  size_t page_idx = pg_no (kpage) - pg_no (user_pool.base);
  lock_acquire (&frame_table.frames[page_idx].lock);
  frame_table.frames[page_idx].thread = t;
  frame_table.frames[page_idx].pte = pte;
  lock_release (&frame_table.frames[page_idx].lock);

  return kpage;
}
