#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "threads/pte.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "filesys/free-map.h"

static void syscall_handler (struct intr_frame *);
static bool check_user_memory (const void *vaddr, size_t size, bool to_write);
static uint32_t get_stack_entry (uint32_t *esp, size_t offset);
static void _halt (void);
static void _exit (int status);
static int _wait (int pid);
static int _read (int fd, void *buffer, unsigned size);
static int _write (int fd, const void *buffer, unsigned size);
static pid_t _exec (const char *cmd_line);
static bool _create (const char *file, unsigned initial_size);
static bool _remove (const char *file);
static int _open (const char *file);
static int _filesize (int fd);
static void _seek (int fd, uint32_t position);
static uint32_t _tell (int fd);
static bool _chdir (const char *dir);
static bool _mkdir (const char *dir);
static bool _readdir (int fd, char *name);
static bool _isdir (int fd);
static int _inumber (int fd);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED)
{
  uint32_t *esp = (uint32_t *)f->esp;
  uint32_t syscall_number = get_stack_entry (esp, 0);
  uint32_t arg1, arg2, arg3;

  switch (syscall_number)
  {
  case SYS_HALT:
    _halt ();
    break;
  case SYS_EXIT:
    arg1 = get_stack_entry (esp, 1);
    _exit ((int)arg1);
    break;
  case SYS_EXEC:
    arg1 = get_stack_entry (esp, 1);
    f->eax = (uint32_t)_exec ((const char *)arg1);
    break;
  case SYS_WAIT:
    arg1 = get_stack_entry (esp, 1);
    f->eax = (uint32_t)_wait ((int)arg1);
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
    arg1 = get_stack_entry (esp, 1);
    arg2 = get_stack_entry (esp, 2);
    arg3 = get_stack_entry (esp, 3);
    f->eax = (uint32_t)_read ((int)arg1, (void *)arg2, (unsigned)arg3);
    break;
  case SYS_WRITE:
    arg1 = get_stack_entry (esp, 1);
    arg2 = get_stack_entry (esp, 2);
    arg3 = get_stack_entry (esp, 3);
    f->eax = (uint32_t)_write ((int)arg1, (const void *)arg2,
                                (unsigned)arg3);
    break;
  case SYS_SEEK:
    arg1 = get_stack_entry (esp, 1);
    arg2 = get_stack_entry (esp, 2);
    _seek ((int)arg1, (uint32_t)arg2);
    break;
  case SYS_TELL:
    arg1 = get_stack_entry (esp, 1);
    f->eax = (uint32_t)_tell ((int)arg1);
    break;
  case SYS_CLOSE:
    arg1 = get_stack_entry (esp, 1);
    _close ((int)arg1);
    break;
  case SYS_CHDIR:
    arg1 = get_stack_entry (esp, 1);
    f->eax = (uint32_t)_chdir ((char *)arg1);
    break;
  case SYS_MKDIR:
    arg1 = get_stack_entry (esp, 1);
    f->eax = (uint32_t)_mkdir ((char *)arg1);
    break;
  case SYS_READDIR:
    arg1 = get_stack_entry (esp, 1);
    arg2 = get_stack_entry (esp, 2);
    f->eax = (uint32_t)_readdir ((int)arg1, (char *)arg2);
    break;
  case SYS_ISDIR:
    arg1 = get_stack_entry (esp, 1);
    f->eax = (uint32_t)_isdir ((int)arg1);
    break;
  case SYS_INUMBER:
    arg1 = get_stack_entry (esp, 1);
    f->eax = (uint32_t)_inumber ((int)arg1);
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

/* Get an entry from user process stack. */
static uint32_t
get_stack_entry (uint32_t *esp, size_t offset)
{
  if (!check_user_memory (esp + offset, sizeof(uint32_t), false))
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
  shutdown_power_off ();
}

static void
_exit (int status)
{
  struct thread* cur = thread_current ();
  cur->exit_value = status;
  if (cur->exit_status != NULL)
    cur->exit_status->exit_value = status;
  thread_exit ();
}

static int
_wait (int pid)
{
  return process_wait (pid);
}

static int
_read (int fd, void *buffer, unsigned size)
{
  if (!check_user_memory (buffer, size, true))
    _exit (-1);

  if (fd == STDOUT_FILENO)
    _exit (-1);

  if (fd == STDIN_FILENO)
  {
    unsigned i;
    uint8_t *char_buf = (uint8_t *)buffer;
    for (i = 0; i < size; i++)
      char_buf[i] = input_getc ();
    return size;
  }

  struct file *f = thread_get_file (thread_current (), fd);
  if (f == NULL)
  {
    _exit (-1);
  }
  int result = file_read (f, buffer, size);
  return result;
}

static int
_write (int fd, const void *buffer, unsigned size)
{
  if (!check_user_memory (buffer, size, false))
    _exit (-1);

  if (fd == STDIN_FILENO)
    _exit (-1);

  if (fd == STDOUT_FILENO)
  {
    putbuf (buffer, size);
    return size;
  }
  if (_isdir (fd))
  {
    return -1;
  }
  struct file *f = thread_get_file (thread_current (), fd);
  if (f == NULL)
  {
    _exit (-1);
  }
  int result = file_write (f, buffer, size);
  return result;
}

static pid_t
_exec (const char *cmd_line)
{
  if (!check_user_string (cmd_line))
    _exit (-1);
  pid_t pid = (pid_t)process_execute (cmd_line);
  return pid;
}

static bool
_create (const char *file, unsigned initial_size)
{
  if (!check_user_string (file))
    _exit (-1);
  bool success = filesys_create (file, initial_size);
  return success;
}

static bool
_remove (const char *file)
{
  if (!check_user_string (file))
    _exit (-1);
  bool success = filesys_remove (file);
  return success;
}

static int
_open (const char *file)
{
  if (!check_user_string (file))
    _exit (-1);
  struct file *fp = filesys_open (file);
  int fd = thread_add_file (thread_current (), fp);
  return fd;
}

static int
_filesize (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (file == NULL)
  {
    _exit (-1);
  }
  int size = file_length (file);
  return size;
}
static void
_seek (int fd, uint32_t position)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (!file)
    _exit (-1);
  file_seek (file, position);
}

static uint32_t
_tell (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (file == NULL)
    _exit (-1);
  uint32_t result = file_tell (file);
  return result;
}

void
_close (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (!file)
    _exit (-1);
  file_close (file);
  thread_rm_file (thread_current (), fd);
}

static bool _chdir (const char *dir)
{
  if (!check_user_string (dir))
    _exit (-1);
  struct thread *t = thread_current ();
  struct dir *trgt;
  char *name;
  if (!dir_parser (dir, &trgt, &name))
  {
    return false;
  }
  struct inode *inode;
  if (!dir_lookup (trgt, name, &inode))
  {
    dir_close (trgt);
    return false;
  }
  if (!inode_isdir(inode))
  {
    return false;
  }
  dir_close (trgt);
  trgt = dir_open (inode);
  if (trgt == NULL)
  {
    return false;
  }
  dir_close (t->cur_dir);
  t->cur_dir = trgt;
  return true;
}

static bool _mkdir (const char *dir)
{
  if (!check_user_string (dir))
    _exit (-1);
  struct dir *trgt, *new_dir;
  char *name;
  bool success = false;
  if (!dir_parser (dir, &trgt, &name))
  {
    return false;
  }
  if (!check_file_name (name))
  {
    return false;
  }
  struct inode *inode;
  block_sector_t inode_sector = 0;
  if (free_map_allocate (1, &inode_sector))
  {
    //TODO should only create a dir with 2 entry
    if (!dir_create (inode_sector, 10))
    {
      free_map_release (inode_sector, 1);
    }
    else
    {
      inode = inode_open (inode_sector);
      new_dir = dir_open (inode);
      if (new_dir == NULL)
      {
        inode_remove (inode);
        inode_close (inode);
      }
      else
      {
        success = dir_add (new_dir, ".", inode_sector) &&
          dir_add (new_dir, "..", inode_get_inumber (dir_inode (trgt))) &&
          dir_add (trgt, name, inode_sector);
        if (!success)
        {
          inode_remove (inode);
        }
        dir_close (new_dir);
      }
    }
  }
  dir_close (trgt);
  return success;
}

static bool _readdir (int fd, char *name)
{
  if (!check_user_memory (name, NAME_MAX + 1, true))
    _exit (-1);
  struct file *file = thread_get_file (thread_current (), fd);
  if (!file)
  {
    _exit (-1);
  }
  struct inode *inode = file_get_inode (file);
  if (!inode_isdir(inode))
  {
    return false;
  }
  struct dir *dir = dir_open (inode_reopen(inode));
  dir_set_pos(dir, file_tell (file));
  while (dir_readdir (dir, name))
  {
    file_seek (file, dir_get_pos(dir));
    if (strcmp (name, ".") && strcmp (name, ".."))
    {
      dir_close (dir);
      return true;
    }
  }
  dir_close (dir);
  return false;

}

static bool _isdir (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (!file)
  {
    _exit (-1);
  }
  struct inode *inode = file_get_inode (file);
  return inode_isdir(inode);
}

static int _inumber (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (!file)
  {
    _exit (-1);
  }
  return inode_get_inumber (file_get_inode (file));
}
