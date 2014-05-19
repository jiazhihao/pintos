#ifndef VM_PAGE_H
#define VM_PAGE_H
#include "lib/kernel/hash.h"
#include "filesys/off_t.h"
#include "threads/synch.h"


struct file_addr
{
  struct file *file;
  off_t offset;
};

union daddr
{
  size_t swap_addr;
  struct file_addr file_addr;
};

struct spt
{
  struct hash table;
  struct lock lock;
};

struct spte
{
  void *vaddr;
  union daddr daddr;
  struct hash_elem elem;
};

void spt_init (struct spt *spt);
void spt_clear (struct spt *spt);
void spt_destroy (struct spt *spt);
size_t spt_size (struct spt *spt);
bool spt_empty (struct spt *spt);
struct spte *spt_insert (struct spt *spt, void *vaddr, union daddr *daddr);
struct spte *spt_replace (struct spt *spt, void *vaddr, union daddr *daddr);
struct spte *spt_find (struct spt *spt, void *vaddr);
void spt_delete (struct spt *spt, void *vaddr);

#endif /* vm/page.h */
