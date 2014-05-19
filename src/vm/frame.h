#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include "threads/thread.h"

/* Frame table entry */
struct fte
{
  struct thread *thread;      /* Thread that owns this frame table entry. */
  void *vaddr;                /* The beginning virtual address that cooresponds
                                 to this frame table entry. */
};

/* Frame table*/
struct frame_table
{
  size_t size;                /* total number of frames in this frame table */
  struct fte *frames;         /* frames in the table */
};

void frame_table_init (struct frame_table *ft, size_t page_cnt, void *base,
                       size_t block_size);
size_t frame_table_size (size_t page_cnt);

#endif /* vm/frame.h */
