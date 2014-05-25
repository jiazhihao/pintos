#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"
#define FILE_NAME_LEN 16
#define WORD_SIZE 4
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

bool get_file_name (char *cmd_line, char *file_name);
bool argument_passing (char *argv, void **esp);
bool calculate_len (char *argv, int *argc, int *len);
bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                   uint32_t read_bytes, uint32_t zero_bytes,
                   bool writable, bool executable);

struct start_status
{
  bool success;                     /* True if load is successful */
  struct semaphore sema;            /* Used to synchronize between
                                       process_execute() and start_process */
  char *cmd_line;                   /* Used to pass the cmd_line to loader */
};

#endif /* userprog/process.h */
