#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/thread.h"
#include "threads/interrupt.h"
void syscall_init (void);
void _close (int fd);
bool _page_fault (void *intr_esp, void *fault_addr);
typedef tid_t pid_t;
typedef int mapid_t;
#endif /* userprog/syscall.h */
