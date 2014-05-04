#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/vaddr.h" // is_user_vaddr(), pg_round_down(), PGSIZE
#include "devices/shutdown.h" // shutdown_power_off()
#include "threads/pte.h" // PTE_U, PTE_P, PTE_W
#include "userprog/pagedir.h" // pagedir_check_userpage()
#include <string.h>

static void syscall_handler (struct intr_frame *);
static bool check_user_memory (const void *vaddr, size_t size, bool to_write);
static uint32_t get_stack_entry (uint32_t *esp, size_t offset);
static void _halt (void);
static void _exit (int status);
static int _wait (int pid);
static int _write (int fd, const void *buffer, unsigned size);
static pid_t _exec (char *cmd_line);
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t *esp = (uint32_t *) f->esp;
  if (!is_user_vaddr (f->esp))
  	_exit (-1);

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
      f->eax = (uint32_t)_exec ((char *)arg1);
    case SYS_WAIT:
      arg1 = get_stack_entry (esp, 1);
      f->eax = (uint32_t) _wait ((int)arg1);
      break;
    case SYS_CREATE:
    case SYS_REMOVE:
    case SYS_OPEN:
    case SYS_FILESIZE:
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
  if (!is_user_vaddr (esp+offset))
  	_exit (-1);
  return *(esp + offset);
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
_exec (char *cmd_line)
{
  if (strlen (cmd_line) > PGSIZE)
  {
    return -1;
  }
  if (!check_user_memory (cmd_line, strlen (cmd_line), false))
  {
    return -1;
  }
  pid_t pid = (pid_t)process_execute (cmd_line);
  return pid;
}
