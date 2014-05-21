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
struct mte *mt_get (struct thread *t, mapid_t mapid);
#endif /* userprog/syscall.h */
