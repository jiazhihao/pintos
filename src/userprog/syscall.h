#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/thread.h"
#include "threads/interrupt.h"
typedef int mapid_t;
typedef tid_t pid_t;
void syscall_init (void);
void _close (int fd);
void _exit (int);
bool _page_fault (void *intr_esp, void *fault_addr);
void _munmap (mapid_t mapping);
bool mte_empty (struct mte *mte);
void update_pte (void *kpage, uint32_t *pte, uint32_t flags);
struct mte *mt_get (struct thread *t, mapid_t mapid);
size_t pin_multiple (const void *vaddr, size_t size);
void unpin_multiple (const void *vaddr, size_t size);
#endif /* userprog/syscall.h */
