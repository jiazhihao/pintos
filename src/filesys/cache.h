#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "filesys/off_t.h"
#include <stdbool.h>

void cache_init (void);
void cache_read (block_sector_t sector, void *buffer);
void cache_write (block_sector_t sector, const void *buffer);
void cache_flush (void);
void cache_read_partial (block_sector_t, void *, off_t, off_t); 
void cache_write_partial (block_sector_t, const void *, off_t, off_t, bool);
void cache_read_ahead (block_sector_t);

#endif /* filesys/cache.h */
