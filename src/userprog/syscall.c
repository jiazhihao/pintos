#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h" // is_user_vaddr(), pg_round_down(), PGSIZE
#include "devices/shutdown.h" // shutdown_power_off()
#include "threads/pte.h" // PTE_U, PTE_P, PTE_W
#include "userprog/pagedir.h" // pagedir_check_userpage()
#include "userprog/process.h" // process_wait()
#include <string.h>
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/palloc.h"

static void syscall_handler (struct intr_frame *);
static bool check_user_memory (const void *vaddr, size_t size, bool to_write);
static uint32_t get_stack_entry (uint32_t *esp, size_t offset);
static void _halt (void);
static void _exit (int status);
static int _wait (int pid);
static int _write (int fd, const void *buffer, unsigned size);
static pid_t _exec (const char *cmd_line);
static bool _create (const char *file, unsigned initial_size);
static bool _remove (const char *file);
static int _open (const char *file);

static int _filesize (int fd);

extern struct lock filesys_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  uint32_t *esp = (uint32_t *) f->esp;

  uint32_t syscall_number = get_stack_entry (esp, 0);
  uint32_t arg1, arg2, arg3;

  switch (syscall_number)
  {
    case SYS_HALT:
      _halt();
      break;
    case SYS_EXIT:
      arg1 = get_stack_entry (esp, 1);
      _exit ((int)arg1);
      break;
    case SYS_EXEC:
      arg1 = get_stack_entry (esp, 1);
      f->eax = (uint32_t) _exec ((const char *)arg1);
      break;
    case SYS_WAIT:
      arg1 = get_stack_entry (esp, 1);
      f->eax = (uint32_t) _wait ((int)arg1);
      break;
    case SYS_CREATE:
      arg1 = get_stack_entry (esp, 1);
      arg2 = get_stack_entry (esp, 2);
      f->eax = (uint32_t)_create ((const char *)arg1, (unsigned)arg2);
      break;
    case SYS_REMOVE:
      arg1 = get_stack_entry (esp, 1);
      f->eax = (uint32_t)_remove ((const char *)arg1);
      break;
    case SYS_OPEN:
      arg1 = get_stack_entry (esp, 1);
      f->eax = (uint32_t)_open ((char *)arg1);
      break;
    case SYS_FILESIZE:
      arg1 = get_stack_entry (esp, 1);
      f->eax = (uint32_t)_filesize ((int)arg1);
      break;
    case SYS_READ:
    case SYS_WRITE:
      arg1 = get_stack_entry (esp, 1);
      arg2 = get_stack_entry (esp, 2);
      arg3 = get_stack_entry (esp, 3);
      f->eax = (uint32_t) _write ((int)arg1, (const void *)arg2, (unsigned)arg3);
      break;
    case SYS_SEEK:
    case SYS_TELL:
    case SYS_CLOSE:
      break;
  }	
}

/* Check whether a range of user viritual memory is valid. */
static bool
check_user_memory (const void *vaddr, size_t size, bool to_write)
{
  if (vaddr == NULL || !is_user_vaddr (vaddr + size))
  	return false;

  struct thread *t = thread_current ();
  void *upage = pg_round_down (vaddr);

  for (; upage < vaddr + size; upage += PGSIZE)
  {
  	if (!pagedir_check_userpage (t->pagedir, upage, to_write))
      return false;
  }

  return true;
}

static uint32_t
get_stack_entry (uint32_t *esp, size_t offset)
{
  if (!check_user_memory (esp+offset, sizeof(uint32_t), false))
    _exit (-1);
  return *(esp + offset);
}

/* Check whether a string given by the user is valid. */
static bool
check_user_string (const char *str)
{
  unsigned strlen_max;
  if (!check_user_memory (str, 0, false))
    return 0;
  if (!check_user_memory (str, PGSIZE, false))
    strlen_max = pg_round_up (str) - (const void *)str;
  else
    strlen_max = PGSIZE;
  if (strnlen (str, strlen_max) >= strlen_max)
    return 0;
  return 1;
}

static void 
_halt (void)
{
  shutdown_power_off();
}

static void
_exit (int status)
{
  struct thread* cur = thread_current();
  if (cur->exit_status != NULL)
    cur->exit_status->exit_value = status;
  thread_exit ();
}

static int
_wait (int pid)
{
  return process_wait(pid);
}

static int
_write (int fd, const void *buffer, unsigned size)
{
  // TODO
  if (!check_user_memory (buffer, size, false))
  	_exit(-1);
  if (fd == STDOUT_FILENO)
  {
  	putbuf (buffer, size);
  	return size;
  }
  return 0;
}

static pid_t
_exec (const char *cmd_line)
{
/*
  unsigned strlen_max;
  if (!check_user_memory (cmd_line, 0, false))
    _exit (-1);
  if (!check_user_memory (cmd_line, PGSIZE, false))
    strlen_max = pg_round_up (cmd_line) - (const void *)cmd_line;
  else
    strlen_max = PGSIZE;
  if (strnlen (cmd_line, strlen_max) >= strlen_max)
    _exit (-1);
*/
  if (!check_user_string (cmd_line))
    _exit (-1);
  pid_t pid = (pid_t)process_execute (cmd_line);
  return pid;
}

static bool
_create (const char *file, unsigned initial_size)
{
  //if (!check_filename (file))
  //{
    //return false;
  //}
  if (!check_user_string (file))
    _exit (-1);
  lock_acquire (&filesys_lock);
  bool success = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return success;
}

static bool
_remove (const char *file)
{
  //if (!check_filename (file))
  //{
    //return false;
  //}
  if (!check_user_string (file))
    _exit (-1);
  lock_acquire (&filesys_lock);
  bool success = filesys_remove (file);
  lock_release (&filesys_lock);
  return success;
}

static int
_open (const char *file)
{
  //if (!check_filename (file))
  //{
  //  return -1;
  //}

  if (!check_user_string (file))
    _exit (-1);

  lock_acquire (&filesys_lock);
  struct file *fp = filesys_open (file);
  int fd = thread_add_file (fp);
  lock_release (&filesys_lock);
  return fd;
}

static int
_filesize (int fd)
{
  struct file *file = thread_get_file (thread_current(), fd);
  if (file == NULL)
  {
    return -1;
  }
  else
  {
    lock_acquire (&filesys_lock);
    int size = file_length (file);
    lock_release (&filesys_lock);
    return size;
  }
}
