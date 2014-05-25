#ifndef VM_PAGE_H
#define VM_PAGE_H
#include "lib/kernel/hash.h"
#include "filesys/off_t.h"
#include "threads/synch.h"

/* file metadata for each mmap file*/
struct file_meta
{
  struct file *file;            /* pointer to a file structure */
  off_t offset;                 /* offset in the above file */
  size_t read_bytes;            /* total amount of bytes to read */
};

/* union used by supplemental page table
 * entry to find corresponding files */
union daddr
{
  size_t swap_addr;             /* If this page is stored in as a swap page,
                                   swap_add indicates the slot num of the page
                                   in swap table */
  struct file_meta file_meta;   /* If the page is stored in disk as a mmap
                                   file, file_meta data stores necessary
                                   information to regain the page */
};

/* supplemental page table */
struct spt
{
  struct hash table;            /* hash table to implement supplemental page
                                   table */
  struct lock lock;             /* lock for supplemental page table */
};

/* supplemental page table entry*/
struct spte
{
  uint32_t *pte;                /* pointer to a page table entry*/
  union daddr daddr;            /* union used to find cooresponding file*/
  struct hash_elem elem;        /* element for hash table */
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
