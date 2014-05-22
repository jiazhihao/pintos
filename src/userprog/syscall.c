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
#include "vm/swap.h"
#include "lib/round.h"

static void syscall_handler (struct intr_frame *);
static bool check_user_memory (const void *vaddr, size_t size, bool to_write);
static uint32_t get_stack_entry (uint32_t *esp, size_t offset);
static void _halt (void);
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
static mapid_t _mmap (int fd, void *addr);
static mapid_t mt_add (struct thread *t, struct mte* mte);
static void mt_rm (struct thread *t, mapid_t mapid);
static bool load_page_from_file (uint32_t *);
static bool load_page_from_swap (uint32_t *);
static bool stack_growth (void *);


extern struct lock filesys_lock;

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

  struct thread *cur = thread_current ();
  ASSERT(cur->esp == NULL);
  cur->esp = f->esp;

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
  case SYS_MMAP:
    arg1 = get_stack_entry (esp, 1);
    arg2 = get_stack_entry (esp, 2);
    f->eax = (uint32_t)_mmap ((int)arg1, (void *)arg2);
    break;
  case SYS_MUNMAP:
    arg1 = get_stack_entry (esp, 1);
    _munmap ((mapid_t)arg1);
    break;
  }

  ASSERT(cur->esp != NULL);
  cur->esp = NULL;
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
    {
      if (!_page_fault(NULL, upage))
        return false;
    }
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

void
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
    return -1;

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
    return -1;
  }
  else
  {
    lock_acquire (&filesys_lock);
    int result = file_read (f, buffer, size);
    lock_release (&filesys_lock);
    return result;
  }
}


static int
_write (int fd, const void *buffer, unsigned size)
{
  if (!check_user_memory (buffer, size, false))
    _exit (-1);

  if (fd == STDIN_FILENO)
    return -1;

  if (fd == STDOUT_FILENO)
  {
    putbuf (buffer, size);
    return size;
  }

  struct file *f = thread_get_file (thread_current (), fd);
  if (f == NULL)
  {
    return -1;
  }
  else
  {
    lock_acquire (&filesys_lock);
    int result = file_write (f, buffer, size);
    lock_release (&filesys_lock);
    return result;
  }

  return 0;
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
  lock_acquire (&filesys_lock);
  bool success = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return success;
}

static bool
_remove (const char *file)
{
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
  if (!check_user_string (file))
    _exit (-1);
  lock_acquire (&filesys_lock);
  struct file *fp = filesys_open (file);
  int fd = thread_add_file (thread_current (), fp);
  lock_release (&filesys_lock);
  return fd;
}

static int
_filesize (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
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
static void
_seek (int fd, uint32_t position)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (file)
  {
    lock_acquire (&filesys_lock);
    file_seek (file, position);
    lock_release (&filesys_lock);
  }
}

static uint32_t
_tell (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (file == NULL)
  {
    return 0;
  }
  lock_acquire (&filesys_lock);
  uint32_t result = file_tell (file);
  lock_release (&filesys_lock);
  return result;
}

void
_close (int fd)
{
  struct file *file = thread_get_file (thread_current (), fd);
  if (file)
  {
    lock_acquire (&filesys_lock);
    file_close (file);
    lock_release (&filesys_lock);
    thread_rm_file (thread_current (), fd);
  }
}

static mapid_t _mmap (int fd, void *addr)
{
  if (fd == 0 || fd == 1)
  {
    return -1;
  }
  struct file * file = thread_get_file (thread_current (), fd);
  if (!file)
  {
    return -1;
  }
  lock_acquire (&filesys_lock);
  file = file_reopen(file);
  lock_release (&filesys_lock);
  if (file == NULL)
  {
    return -1;
  }
  int size = _filesize (fd);
  if (size <= 0)
  {
    return -1;
  }
  if (!is_user_vaddr (addr) || !is_user_vaddr (addr + size))
  {
    _exit (-1);
  }
  if (pg_ofs (addr) || !addr)
  {
    return -1;
  }
  size_t page_cnt = DIV_ROUND_UP(size, PGSIZE);
  if (!load_segment (file, 0, addr, size, PGSIZE * page_cnt - size, true, false))
  {
    return -1;
  }
  mapid_t mapid;
  struct mte mte = { addr, size };
  mapid = mt_add (thread_current (), &mte);
  if (mapid < 0)
  {
    return -1;
  }
  return mapid;
}

void _munmap (mapid_t mapping)
{
  struct thread *cur = thread_current ();
  struct mte *mte = mt_get (cur, mapping);
  if (mte && (!mte_empty (mte)))
  {
    uint32_t *pte = lookup_page (cur->pagedir, mte->vaddr, false);
    ASSERT (pte && *pte && (*pte & PTE_F) && (*pte & PTE_U) && (*pte & PTE_W) && !((*pte & PTE_E)));
    if (pte && *pte && (*pte & PTE_F) && (*pte & PTE_U))
    {
      lock_acquire (&cur->spt.lock);
      struct spte *spte = spt_find (&cur->spt, pte);
      if (!spte)
      {
        return;
      }
      struct file *file = spte->daddr.file_meta.file;
      if (!file)
      {
        spt_delete (&cur->spt, pte);
        return;
      }
      void *vaddr = mte->vaddr;
      size_t size = mte->size;
      size_t write_bytes = 0;
      off_t offset = 0;
      while (size > 0)
      {
        pte = lookup_page (cur->pagedir, vaddr, false);
        spt_delete (&cur->spt, pte);
        write_bytes = size > PGSIZE ? PGSIZE : size;
        if (pte && *pte && (*pte & PTE_F) && (*pte & PTE_U))
        {
          if ((*pte & PTE_P) && (*pte & PTE_D))
          {
            void *kpage = pte_get_page (*pte);
            lock_acquire (&filesys_lock);
            file_write_at (file, kpage, write_bytes, offset);
            lock_release (&filesys_lock);
            *pte = 0;
            frame_free_page (kpage);
          }
          else
          {
            *pte = 0;
          }
        }
        size -= write_bytes;
        offset += write_bytes;
      }
      lock_acquire (&filesys_lock);
      file_close (file);
      lock_release (&filesys_lock);
      lock_release (&cur->spt.lock);
    }
    mt_rm (cur, mapping);
  }
}

/* Add a mmap entry to the thread's mmap table.
* Double space for mmap table if necessary.
* Return the mapid */
static mapid_t mt_add (struct thread *t, struct mte* mte)
{
  if (mte == NULL)
  {
    return -1;
  }
  mapid_t mapid = 0;

  if (t->mt_size == 0)
  {
    t->mt = (struct mte *)palloc_get_page (PAL_ZERO);
    if (t->mt == NULL)
      return -1;
    t->mt_size = PGSIZE / sizeof(struct mte);
  }
  else
  {
    for (mapid = 0; mapid < t->mt_size; mapid++)
    {
      if (mte_empty (&(t->mt[mapid])))
      {
        break;
      }
    }
    /* Didn't find empty slot in original file table, need to double
    * file table size. */
    if (mapid == t->mt_size)
    {
      int ft_page_num = t->mt_size * sizeof(struct mte) / PGSIZE * 2;
      struct mte * new_mt =
        (struct mte *) palloc_get_multiple (PAL_ZERO, ft_page_num);
      if (new_mt == NULL)
        return -1;
      memcpy (new_mt, t->mt,
              t->mt_size * sizeof(struct mte));
      palloc_free_multiple (t->mt,
                            t->mt_size * sizeof(struct mte) / PGSIZE);
      t->mt = new_mt;
      t->mt_size = t->mt_size * 2;
      mapid = mapid + 1;
    }
  }
  t->mt[mapid] = *mte;
  return mapid;
}

/* Remove file from file table. */
static void mt_rm (struct thread *t, mapid_t mapid)
{
  if (mapid < t->mt_size)
  {
    t->mt[mapid].vaddr = NULL;
    t->mt[mapid].size = 0;
  }
}

bool mte_empty (struct mte *mte)
{
  return mte->vaddr == NULL && mte->size == 0;
}

struct mte *mt_get (struct thread *t, mapid_t mapid)
{
  if (mapid >= 0 && mapid < t->mt_size)
    return &(t->mt[mapid]);
  else
    return NULL;
}

/* Return true if we can successfully access the page, false ow.*/

bool
_page_fault (void *intr_esp, void *fault_addr)
{
  if (!is_user_vaddr (fault_addr))
  {
    return false;
  }

  struct thread *cur = thread_current ();
  void *fault_page = pg_round_down(fault_addr);
  uint32_t *pte = lookup_page (cur->pagedir, fault_addr, false);

  if (pte && (*pte & PTE_P))
  {
    return false;
  }

  /* Case 1: Stack Growth */
  void *esp;
  if (cur->esp == NULL)
    esp = intr_esp;
  else
    esp = cur->esp;
  if ((fault_addr == esp - 4 ||
       fault_addr == esp - 32 ||
       fault_addr >= esp)
    && fault_addr >= STACK_BOUNDARY
	&& (pte == NULL || *pte == 0))
  {
    return stack_growth(fault_page);
  }

  /* Case 2: executable file */
  if (pte && (*pte & PTE_F))
  {
    return load_page_from_file (pte);
  }

  /* Case 3: page in swap block. */
  if (pte && *pte != 0 && !(*pte && PTE_P) && !(*pte && PTE_F))
  {
    return load_page_from_swap (pte);
  }

  return false;
}

static void
update_pte (void *kpage, uint32_t *pte, uint32_t flags)
{
  ASSERT (!(*pte & PTE_P));
  *pte = vtop (kpage) | flags;
  *pte |= PTE_P;
}

static bool
load_page_from_file (uint32_t *pte)
{
  size_t read_bytes = 0;
  ASSERT (*pte & PTE_F);
  struct thread *cur = thread_current ();
  uint8_t *kpage = frame_get_page (FRM_USER | FRM_ZERO, pte);
  if (!kpage)
  {
    return false;
  }
  ASSERT (pg_ofs (kpage) == 0);
  lock_acquire (&cur->spt.lock);
  struct spte *spte = spt_find (&cur->spt, pte);
  if (spte)
  {
    struct file_meta meta = spte->daddr.file_meta;
    //TODO may need a file sys lock
    if (meta.read_bytes > 0)
    {
      lock_acquire (&filesys_lock);
      read_bytes = file_read_at (meta.file, kpage, meta.read_bytes, meta.offset);
      lock_release (&filesys_lock);
    }
    if (read_bytes == meta.read_bytes)
    {
      update_pte (kpage, pte, (*pte & PTE_FLAGS));
      lock_release (&cur->spt.lock);
      return true;
    }
  }
  lock_release (&cur->spt.lock);
  frame_free_page (kpage);
  return false;
}

static bool
load_page_from_swap (uint32_t *pte)
{
  ASSERT (pte != NULL);

  void *kpage = frame_get_page (FRM_USER, pte);
  if (!kpage)
  {
    return false;
  }
  struct thread *cur = thread_current ();
  /* Get swap_page_no from spte, read page from swap to kpage and
     clear spte by removing it. */
  // TODO (rqi) consider parallism support: T1 read its frame X
  // while T2 evicting frame Y owned by T1.
  lock_acquire (&cur->spt.lock);
  struct spte *spte = spt_find (&cur->spt, pte);
  ASSERT ((spte != NULL) && (spte->daddr.swap_addr != 0));
  size_t swap_page_no = spte->daddr.swap_addr;
  swap_read_page (&swap_table, swap_page_no, kpage);
  swap_free_page (&swap_table, swap_page_no);
  spt_delete (&cur->spt, pte);
  lock_release (&cur->spt.lock);
  
  update_pte (kpage, pte, (*pte | PTE_FLAGS));
  return true;
}

static bool
stack_growth (void *upage)
{
  uint32_t *pte = lookup_page (thread_current()->pagedir, upage, true);
  void *kpage = frame_get_page (FRM_USER | FRM_ZERO, pte);
  if (kpage == NULL)
    return false;
  update_pte (kpage, pte, PTE_U | PTE_P | PTE_W);
  return true;
}
