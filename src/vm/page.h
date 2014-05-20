#ifndef VM_PAGE_H
#define VM_PAGE_H
#include "lib/kernel/hash.h"
#include "filesys/off_t.h"
#include "threads/synch.h"


struct file_meta
{
  struct file *file;
  off_t offset;
  size_t read_bytes;
};

union daddr
{
  size_t swap_addr;
  struct file_meta file_meta;
};

struct spt
{
  struct hash table;
  struct lock lock;
};

struct spte
{
  uint32_t *pte;
  union daddr daddr;
  struct hash_elem elem;
};

void spt_init (struct spt *spt);
void spt_clear (struct spt *spt);
void spt_destroy (struct spt *spt);
size_t spt_size (struct spt *spt);
bool spt_empty (struct spt *spt);
struct spte *spt_insert (struct spt *spt, uint32_t *pte, union daddr *daddr);
struct spte *spt_replace (struct spt *spt, uint32_t *pte, union daddr *daddr);
struct spte *spt_find (struct spt *spt, uint32_t *pte);
void spt_delete (struct spt *spt, uint32_t *pte);

#endif /* vm/page.h */
