/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Apple HFS+ (and HFSX) filesystem support for Mac-formatted iPods.
 * Read-write on non-journaled volumes; journaled volumes are read-only.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef HFSPLUS_H
#define HFSPLUS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "config.h"
#include "mv.h"

/* returned by write entry points when the volume cannot be written (e.g. a
   journaled volume, which this driver does not maintain a journal for) */
#define HFS_RC_READONLY (-30)
/* out of space */
#define HFS_RC_ENOSPC   (-31)

/* a single contiguous run of allocation blocks within a fork */
struct hfs_extent
{
    uint32_t start_block;   /* first allocation block (volume-relative) */
    uint32_t block_count;   /* number of allocation blocks */
};

/* cursor used to scan a directory's catalog records; mirrors the FAT scan
   layout so that the embedded copy in struct hfs_file overlaps
   struct fat_file::e exactly (see file_internal.h). node == 0xFFFFFFFF means
   "rewound / not started", node == 0 means "no more entries". */
struct hfs_dirscan_info
{
    uint32_t node;          /* current catalog B-tree leaf node */
    uint32_t record;        /* next record index within that node */
};

/* basic HFS+ file/dir information: where to find it and who houses it.
   The leading padding makes 'e' line up with struct fat_file::e so the
   filesystem-agnostic rewinddir helpers work without knowing the fs type. */
struct hfs_file
{
#ifdef HAVE_MULTIVOLUME
    int    volume;              /* file resides on which volume (first!) */
#endif
    long   _pad_firstcluster;   /* aligns with fat_file::firstcluster */
    long   _pad_dircluster;     /* aligns with fat_file::dircluster */
    struct hfs_dirscan_info e;  /* entry/scan information (aligns with fat) */
    /* HFS+ specific below this point */
    uint32_t cnid;              /* catalog node id of this file/dir */
    uint32_t parent;            /* parent folder cnid (0 if unknown) */
    uint8_t  is_dir;            /* nonzero if a directory */
    uint64_t size;              /* data fork logical size in bytes */
    uint32_t total_blocks;      /* data fork size in allocation blocks */
    struct hfs_extent extents[8]; /* first eight data-fork extents (inline) */
};

/* per-open read cursor; analogue of struct fat_filestr */
struct hfs_filestr
{
    struct hfs_file *filep;     /* common file information */
    uint32_t cur_sector;        /* next file-relative sector to read */
    bool     eof;               /* end-of-file reached */
};

/** File entity functions **/
bool hfs_file_is_same(const struct hfs_file *file1,
                      const struct hfs_file *file2);
int hfs_open(const struct hfs_file *parent, long cnid, struct hfs_file *file);
int hfs_open_rootdir(IF_MV(int volume,) struct hfs_file *dir);

#if defined(MAX_VARIABLE_LOG_SECTOR)
int hfs_file_sector_size(IF_MV_NONVOID(const struct hfs_file *file));
#else
#define hfs_file_sector_size(__file) SECTOR_SIZE
#endif

/** File stream functions **/
void hfs_filestr_init(struct hfs_filestr *filestr, struct hfs_file *file);
sector_t hfs_query_sectornum(const struct hfs_filestr *filestr);
long hfs_readwrite(struct hfs_filestr *filestr, unsigned long sectorcount,
                   void *buf, bool write);
void hfs_rewind(struct hfs_filestr *filestr);
int hfs_seek(struct hfs_filestr *filestr, unsigned long sector);

/** Directory stream functions **/
struct fat_direntry; /* readdir fills a (shared) struct fat_direntry */
int hfs_readdir(struct hfs_filestr *dirstr, struct hfs_dirscan_info *scan,
                struct fat_direntry *entry);
void hfs_rewinddir(struct hfs_dirscan_info *scan);

/** Write functions (not built for the bootloader) **/
void hfs_seek_to_stream(struct hfs_filestr *filestr,
                        const struct hfs_filestr *seekto);
int hfs_truncate(struct hfs_filestr *filestr);
int hfs_closewrite(struct hfs_filestr *filestr, uint32_t size,
                   struct fat_direntry *fatentp);
int hfs_create(struct hfs_file *parent, const char *name, unsigned int attr,
               struct hfs_file *file, struct fat_direntry *fatentp);
int hfs_remove(struct hfs_file *file, int what);
int hfs_rename(struct hfs_file *parent, struct hfs_file *file,
               const unsigned char *newname);
int hfs_modtime(struct hfs_file *parent, struct hfs_file *file, time_t modtime);

/** Mounting and unmounting functions **/
bool hfs_ismounted(IF_MV_NONVOID(int volume));
int hfs_mount(IF_MV(int volume,) IF_MD(int drive,) unsigned long startsector);
int hfs_unmount(IF_MV_NONVOID(int volume));
bool hfs_size(IF_MV(int volume,) sector_t *size, sector_t *free);
void hfs_init(void);

#endif /* HFSPLUS_H */
