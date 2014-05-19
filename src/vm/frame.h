#ifndef VM_FRAME_H
#define VM_FRAME_H

/* Frame table entry */
struct fte
{
  uint32_t *pte;              /* Pointer to a page table entry */
};

/* Frame table*/
struct frame_table
{
  size_t size;                /* total number of frames in this frame table */
  struct fte *frames;         /* frames in the table */
};

#endif /* vm/frame.h */
