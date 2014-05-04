#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"
#define FILE_NAME_LEN 16
#define LONG_SIZE 4
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

bool get_file_name (char *cmd_line, char *file_name);
bool argv_passer (char *argv, void **esp);
bool calculate_len (char *argv, int *argc, int *len);

struct start_status
{
  struct semaphore sema;
  char *cmd_line;
};

#endif /* userprog/process.h */
