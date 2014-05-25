#include "vm/page.h"
#include "lib/debug.h"
#include "threads/malloc.h"

/* Use the pte as the hash key and the hash function */
static unsigned spt_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct spte *spte = hash_entry (e, struct spte, elem);
  return (unsigned)spte->pte;
}

/* Compare the key */
static bool spt_hash_less (const struct hash_elem *a, 
                           const struct hash_elem *b, void *aux UNUSED)
{
  struct spte *spte_a = hash_entry (a, struct spte, elem);
  struct spte *spte_b = hash_entry (b, struct spte, elem);
  return spte_a->pte < spte_b->pte;
}

/* Initiate the hash table and the lock */
void spt_init (struct spt *spt)
{
  hash_init (&spt->table, spt_hash_func, spt_hash_less, NULL);
  lock_init (&spt->lock);
}

/* Free the space of one hash entry */
static void spte_clear (struct hash_elem *e, void *aux UNUSED)
{
  free (hash_entry (e, struct spte, elem));
}

/* Clear the supplemental table */
void spt_clear (struct spt *spt)
{
  hash_clear (&spt->table, spte_clear);
}

/* Destroy the supplemental table */
void spt_destroy (struct spt *spt)
{
  hash_destroy (&spt->table, spte_clear);
}

/* Number of entries in the supplemental page table */
size_t spt_size (struct spt *spt)
{
  return hash_size (&spt->table);
}

/* Return true if the supplemental page table is empty */
bool spt_empty (struct spt *spt)
{
  return hash_empty (&spt->table);
}

/* Create an entry and insert into the table
 * If the entry is inserted, return its address, otherwise return NULL */
struct spte *spt_insert (struct spt *spt, uint32_t *pte, union daddr *daddr)
{
  struct spte *spte;
  if (!(spte = (struct spte *)malloc (sizeof(struct spte))))
  {
    return NULL;
  }
  spte->pte = pte;
  spte->daddr = *daddr;
  if (hash_insert (&spt->table, &spte->elem))
  {
    free (spte);
    return NULL;
  }
  return spte;
}

/* Create an entry and replace the entry in the table
 * Free the original entry in the table */
struct spte *spt_replace (struct spt *spt, uint32_t *pte, union daddr *daddr)
{
  struct spte *spte;
  struct hash_elem *e;
  if (!(spte = (struct spte *)malloc (sizeof(struct spte))))
  {
    return NULL;
  }
  spte->pte = pte;
  spte->daddr = *daddr;
  if ((e = hash_replace (&spt->table, &spte->elem)))
  {
    free (hash_entry(e, struct spte, elem));
  }
  return spte;
}

/* Search the entry by the key */
struct spte *spt_find (struct spt *spt, uint32_t *pte)
{
  struct spte spte;
  struct hash_elem *e;
  spte.pte = pte;
  if ((e = hash_find (&spt->table, &spte.elem)))
  {
    return hash_entry (e, struct spte, elem);
  }
  return NULL;
}

/* Delete the entry by the key */
void spt_delete (struct spt *spt, uint32_t *pte)
{
  struct spte spte;
  struct hash_elem *e;
  spte.pte = pte;
  if ((e = hash_delete (&spt->table, &spte.elem)))
  {
    free(hash_entry (e, struct spte, elem));
  }
}
