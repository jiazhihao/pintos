#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry), true);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  lock_dir (dir->inode);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  unlock_dir (dir->inode);

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;
  lock_dir (dir->inode);
  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  unlock_dir (dir->inode);
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  
  if (!strcmp (name, ".") || !strcmp (name, ".."))
  {
    return false;
  }
  lock_dir (dir->inode);
  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;
  if (inode_isdir(inode))
  {
    /* Non-empty dir and open dir cannot be removed */
    if (!dir_empty (inode) || inode_open_cnt(inode) > 1)
    {
      goto done;
    }
  }
  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

done:
  unlock_dir (dir->inode);
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}

/* Parse the string in path, open the relevant directory and store in dir,
 * name point to the real file name */
bool dir_parser (const char *path, struct dir **dir, char **name)
{
  struct dir *cur;
  struct thread *t = thread_current ();
  char *tail, *start, *token, *save_ptr;
  if (path == NULL || name == NULL || path[0] == 0)
  {
    return false;
  }
  char *buf = malloc (strlen (path) + 1);
  if (buf == NULL)
  {
    return false;
  }
  memcpy (buf, path, strlen(path) + 1);
  start = buf;
  /* Skip all the blanks at the start*/
  while (*start == ' ')
  {
    start++;
  }
  tail = buf + strlen (buf) - 1;
  /* Skip all blanks at end */
  while (*tail == ' ')
  {
    tail--;
  }
  /* Skip all '/' at end */
  while (*tail == '/')
  {
    tail--;
  }
  /* Find the last name in the recursive path */
  while (*tail != '/' && tail >= start)
  {
    tail--;
  }
  tail++;
  /* Name point to the final fine name */
  *name = (char *)path + (tail - buf);

  /* If dir == NULL, this function is just used to seperate name from path */
  if (dir == NULL)
  {
    return true;
  }

  /* Choose root dir or thread's current dir */
  if (*start == '/')
  {
    cur = dir_open_root ();
    start++;
  }
  else
  {
    cur = dir_open (inode_reopen (t->cur_dir->inode));
  }

  struct inode *inode = NULL;
  /* strtok_r may change the original string, so copy to buf
  * now start and tail point to the relevant position in buf */
  for (token = strtok_r (start, "/", &save_ptr); 
       token != tail && token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
  {
    if (!dir_lookup (cur, token, &inode) || !inode_isdir(inode))
    {
      dir_close (cur);
      free (buf);
      return false;
    }
    dir_close (cur);
    cur = dir_open (inode);
  }
  *dir = cur;
  free (buf);
  return true;
}

/* Check whether the directory is empty */
bool dir_empty (struct inode *inode)
{
  ASSERT (inode_isdir(inode));
  struct dir_entry e;
  size_t ofs;
  for (ofs = 0; inode_read_at (inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
  {
    if (e.in_use && strcmp (e.name, ".") && strcmp (e.name, ".."))
    {
      return false;
    }
  }
  return true;
}

struct inode *dir_inode (struct dir *dir)
{
  return dir->inode;
}

void dir_set_pos (struct dir *dir, off_t pos)
{
  dir->pos = pos;
}

off_t dir_get_pos (struct dir *dir)
{
  return dir->pos;
}
