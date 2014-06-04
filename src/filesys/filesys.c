#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/synch.h"
#include "userprog/process.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  cache_flush ();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  struct dir *dir;
  char *file_name;
  if (!dir_parser (name, &dir, &file_name))
  {
    return false;
  }
  bool success = false;
  if (check_file_name (file_name))
  {
    success = (dir != NULL
                    && free_map_allocate (1, &inode_sector)
                    && inode_create (inode_sector, initial_size, false)
                    && dir_add (dir, file_name, inode_sector));
    if (!success && inode_sector != 0)
      free_map_release (inode_sector, 1);
  }
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir;
  char *file_name;
  if (!dir_parser (name, &dir, &file_name))
  {
    return NULL;
  }
  char buf[FILE_NAME_LEN + 1];
  strlcpy (buf, file_name, FILE_NAME_LEN);
  unify_file_name (buf);
  if (!check_file_name (buf))
  {
    return NULL;
  }
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, buf, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir;
  char *file_name;
  if (!dir_parser (name, &dir, &file_name))
  {
    return false;
  }
  char buf[FILE_NAME_LEN + 1];
  strlcpy (buf, file_name, FILE_NAME_LEN);
  unify_file_name (buf);
  if (!check_file_name (buf))
  {
    return false;
  }
  bool success = dir != NULL && dir_remove (dir, buf);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  struct dir *dir = dir_open_root ();
  dir_add (dir, ".", ROOT_DIR_SECTOR);
  dir_add (dir, "..", ROOT_DIR_SECTOR);
  dir_close (dir);
  free_map_close ();
  printf ("done.\n");
}

/* check whether the file_name is legal
 * currently only check file_name do not has blank */
bool check_file_name (char *file_name)
{
  if (file_name == NULL)
  {
    return false;
  }
  if (strnlen (file_name, FILE_NAME_LEN) >= FILE_NAME_LEN)
  {
    return false;
  }
  while (*file_name != 0)
  {
    if (*file_name == ' ' || *file_name == '/')
    {
      return false;
    }
    file_name++;
  }
  return true;
}

/* Check tail of the file name, if '/', replace it to '\0' */
void unify_file_name (char *name)
{
  char *tail = name + strlen (name) - 1;
  while (*tail == '/' && tail >= name)
  {
    *tail = 0;
    tail--;
  }
  if (name[0] == 0)
  {
    name[0] = '.';
    name[1] = 0;
  }
}
