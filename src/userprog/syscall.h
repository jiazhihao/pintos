#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/thread.h"
void syscall_init (void);
void _close (int fd);
typedef tid_t pid_t;
typedef int mapid_t;
#endif /* userprog/syscall.h */
