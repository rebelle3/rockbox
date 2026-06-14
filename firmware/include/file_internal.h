/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2014 by Michael Sevakis
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
#ifndef _FILE_INTERNAL_H_
#define _FILE_INTERNAL_H_

#include <sys/types.h>
#include <stdlib.h>
#include "mv.h"
#include "linked_list.h"
#include "mutex.h"
#include "mrsw_lock.h"
#include "fs_attr.h"
#include "fs_defines.h"
#include "fat.h"
#ifdef HAVE_HFSPLUS
#include "hfsplus.h"
#endif
#include "dir.h"
#ifdef HAVE_DIRCACHE
#include "dircache.h"
#endif

#define MAX_OPEN_HANDLES (MAX_OPEN_FILES+MAX_OPEN_DIRS)

/* default attributes when creating new files and directories */
#define ATTR_NEW_FILE       (ATTR_ARCHIVE)
#define ATTR_NEW_DIRECTORY  (ATTR_DIRECTORY)

#define ATTR_SYSTEM_ROOT    (ATTR_SYSROOT | ATTR_DIRECTORY)
#define ATTR_MOUNT_POINT    (ATTR_VOLUME | ATTR_DIRECTORY)

/** File sector cache **/

enum filestr_cache_flags
{
    FSC_DIRTY = 0x1,        /* buffer is dirty (needs writeback) */
    FSC_NEW   = 0x2,        /* buffer is new (never yet written) */
};

struct filestr_cache
{
    sector_t      sector;   /* file sector that is in buffer */
    uint8_t       *buffer;  /* buffer to hold sector */
    unsigned int  flags;    /* FSC_* bits */
};

void file_cache_init(struct filestr_cache *cachep);
void file_cache_reset(struct filestr_cache *cachep);
void file_cache_alloc(struct filestr_cache *cachep);
void file_cache_free(struct filestr_cache *cachep);


/** Common bitflags used throughout **/

/* bitflags used by open files and descriptors */
enum fildes_and_obj_flags
{
    /* used in descriptor and common */
    FDO_BUSY       = 0x0001,     /* descriptor/object is in use */
    /* only used in individual stream descriptor */
    FD_VALID       = 0x0002,     /* descriptor is valid but not registered */
    FD_WRITE       = 0x0004,     /* descriptor has write mode */
    FD_WRONLY      = 0x0008,     /* descriptor is write mode only */
    FD_APPEND      = 0x0010,     /* descriptor is append mode */
    FD_NONEXIST    = 0x8000,     /* closed but not freed (uncombined) */
    /* only used as common flags */
    FO_DIRECTORY   = 0x0020,     /* fileobj is a directory */
    FO_TRUNC       = 0x0040,     /* fileobj is opened to be truncated */
    FO_REMOVED     = 0x0080,     /* fileobj was deleted while open */
    FO_SINGLE      = 0x0100,     /* fileobj has only one stream open */
    FO_MOUNTTARGET = 0x0200,     /* fileobj kept open as a mount target */
    FDO_MASK       = 0x03ff,
    FDO_CHG_MASK   = FO_TRUNC,
   /* fileobj permitted external change */   /* fileobj permitted external change */
    /* bitflags that instruct various 'open' functions how to behave;
     * saved in stream flags (only) but not used by manager */
    FF_FILE        = 0x00000000, /* expect file; accept file only */
    FF_DIR         = 0x00010000, /* expect dir; accept dir only */
    FF_ANYTYPE     = 0x00020000, /* succeed if either file or dir */
    FF_TYPEMASK    = 0x00030000, /* mask of typeflags */
    FF_CHECKPREFIX = 0x00040000, /* detect if file is prefix of path */
    FF_NOISO       = 0x00080000, /* do not decode ISO filenames to UTF-8 */
    FF_PROBE       = 0x00100000, /* only test existence; don't open */
    FF_CACHEONLY   = 0x00200000, /* succeed only if in dircache */
    FF_INFO        = 0x00400000, /* return info on self */
    FF_PARENTINFO  = 0x00800000, /* return info on parent */
    FF_DEVPATH     = 0x01000000, /* path is a device path, not root-based */
    FF_NOFS        = 0x02000000, /* no filesystem mounted here */
    FF_MASK        = 0x03ff0000,
};

/** Common data structures used throughout **/

/* basic file information about its location */
struct file_base_info
{
    union {
#ifdef HAVE_MULTIVOLUME
    int                  volume;  /* file's volume (overlaps fatfile.volume) */
#endif
#if CONFIG_PLATFORM & PLATFORM_NATIVE
    struct fat_file      fatfile; /* FS driver file info */
#endif
#ifdef HAVE_HFSPLUS
    struct hfs_file      hfsfile; /* HFS+ driver file info (volume/e overlap) */
#endif
    };
#ifdef HAVE_DIRCACHE
    struct dircache_file dcfile;  /* dircache file info */
#endif
};

#define BASEINFO_VOL(infop) \
    IF_MV_VOL((infop)->volume)

/* open files binding item */
struct file_base_binding
{
    struct ll_node        node;   /* list item node (first!) */
    struct file_base_info info;   /* basic file info */
};

#define BASEBINDING_VOL(bindp) \
    BASEINFO_VOL(&(bindp)->info)

/* directory scanning position info */
struct dirscan_info
{
    union {
#if CONFIG_PLATFORM & PLATFORM_NATIVE
    struct fat_dirscan_info fatscan; /* FS driver scan info */
#endif
#ifdef HAVE_HFSPLUS
    struct hfs_dirscan_info hfsscan; /* HFS+ scan info (overlaps fatscan) */
#endif
    };
#ifdef HAVE_DIRCACHE
    struct dircache_file    dcscan;  /* dircache scan info */
#endif
};

/* describes the file as an open stream */
struct filestr_base
{
    struct ll_node           node;    /* list item node (first!) */
    uint32_t                 flags;   /* F[DF]_* bits of this stream */
    struct filestr_cache     cache;   /* stream-local cache */
    struct filestr_cache     *cachep; /* the cache in use (local or shared) */
    struct file_base_info    *infop;  /* base file information */
    union {
    struct fat_filestr       fatstr;  /* FS driver information */
#ifdef HAVE_HFSPLUS
    struct hfs_filestr       hfsstr;  /* HFS+ driver stream information */
#endif
    };
    struct file_base_binding *bindp;  /* common binding for file/dir */
    struct mutex             *mtx;    /* serialization for this stream */
};

void filestr_base_init(struct filestr_base *stream);
void filestr_base_destroy(struct filestr_base *stream);
void filestr_alloc_cache(struct filestr_base *stream);
void filestr_free_cache(struct filestr_base *stream);
void filestr_assign_cache(struct filestr_base *stream,
                          struct filestr_cache *cachep);
void filestr_copy_cache(struct filestr_base *stream,
                        struct filestr_cache *cachep);
void filestr_discard_cache(struct filestr_base *stream);

/* allocates a cache buffer if needed and returns the cache pointer */
static inline struct filestr_cache *
filestr_get_cache(struct filestr_base *stream)
{
    struct filestr_cache *cachep = stream->cachep;

    if (!cachep->buffer)
        filestr_alloc_cache(stream);

    return cachep;
}

static inline void filestr_lock(struct filestr_base *stream)
{
    mutex_lock(stream->mtx);
}

static inline void filestr_unlock(struct filestr_base *stream)
{
    mutex_unlock(stream->mtx);
}

/* stream lock doesn't have to be used if getting RW lock writer access */
#define FILESTR_WRITER 0
#define FILESTR_READER 1

#define FILESTR_LOCK(type, stream) \
    ({ if (FILESTR_##type) filestr_lock(stream); })

#define FILESTR_UNLOCK(type, stream) \
    ({ if (FILESTR_##type) filestr_unlock(stream); })

/* auxilliary attributes - out of the way of regular ATTR_* bits */
#define ATTR_SYSROOT (0x8000)
#define ATTR_PREFIX  (0x4000)

/* structure to return detailed information about what you opened */
struct path_component_info
{
    const char   *name;               /* OUT: pointer to name within 'path' */
    size_t       length;              /* OUT: length of component within 'path' */
    file_size_t  filesize;            /* OUT: size of the opened file (0 if dir) */
    unsigned int attr;                /* OUT: attributes of this component */
    struct file_base_info info;       /* OUT: base info on file
                                              (FF_INFO) */
    struct file_base_info parentinfo; /* OUT: base parent directory info
                                              (FF_PARENTINFO) */
    struct file_base_info *prefixp;   /* IN:  base info to check as prefix
                                              (FF_CHECKPREFIX) */
};

int open_stream_internal(const char *path, unsigned int callflags,
                         struct filestr_base *stream,
                         struct path_component_info *compinfo);
int close_stream_internal(struct filestr_base *stream);
int create_stream_internal(struct file_base_info *parentinfop,
                           const char *basename, size_t length,
                           unsigned int attr, unsigned int callflags,
                           struct filestr_base *stream);
int remove_stream_internal(const char *path, struct filestr_base *stream,
                           unsigned int callflags);
int test_stream_exists_internal(const char *path, unsigned int callflags);

int open_noiso_internal(const char *path, int oflag); /* file.c */
void force_close_writer_internal(struct filestr_base *stream); /* file.c */

struct DIRENT;
int uncached_readdir_dirent(struct filestr_base *stream,
                            struct dirscan_info *scanp,
                            struct DIRENT *entry);
void uncached_rewinddir_dirent(struct dirscan_info *scanp);

int uncached_readdir_internal(struct filestr_base *stream,
                              struct file_base_info *infop,
                              struct fat_direntry *fatent);
void uncached_rewinddir_internal(struct file_base_info *infop);

int test_dir_empty_internal(struct filestr_base *stream);

struct dirinfo_internal
{
    unsigned int attr;
    file_size_t  size;
    uint16_t     wrtdate;
    uint16_t     wrttime;
};

/** Synchronization used throughout **/

/* acquire the filesystem lock as READER */
static inline void file_internal_lock_READER(void)
{
    extern struct mrsw_lock file_internal_mrsw;
    mrsw_read_acquire(&file_internal_mrsw);
}

/* release the filesystem lock as READER */
static inline void file_internal_unlock_READER(void)
{
    extern struct mrsw_lock file_internal_mrsw;
    mrsw_read_release(&file_internal_mrsw);
}

/* acquire the filesystem lock as WRITER */
static inline void file_internal_lock_WRITER(void)
{
    extern struct mrsw_lock file_internal_mrsw;
    mrsw_write_acquire(&file_internal_mrsw);
}

/* release the filesystem lock as WRITER */
static inline void file_internal_unlock_WRITER(void)
{
    extern struct mrsw_lock file_internal_mrsw;
    mrsw_write_release(&file_internal_mrsw);
}

#define ERRNO 0 /* maintain errno value */
#define RC    0 /* maintain rc value */

/* NOTES: if _errno is a non-constant expression, it must set an error
 *        number and not return the ERRNO constant which will merely set
 *        errno to zero, not preserve the current value; if you must set
 *        errno to zero, set it explicitly, not in the macro
 *
 *        if _rc is constant-expression evaluation to 'RC', then rc will
 *        NOT be altered; i.e. if you must set rc to zero, set it explicitly,
 *        not in the macro
 */

#define FILE_SET_CODE(_name, _keepcode, _value) \
    ({  __builtin_constant_p(_value) ?                             \
            ({ if ((_value) != (_keepcode)) _name = (_value); }) : \
            ({ _name = (_value); }); })

/* set errno and rc and proceed to the "file_error:" label */
#define FILE_ERROR(_errno, _rc) \
    ({  FILE_SET_CODE(errno, ERRNO, (_errno)); \
        FILE_SET_CODE(rc, RC, (_rc));          \
        goto file_error; })

/* set errno and return a value at the point of invocation */
#define FILE_ERROR_RETURN(_errno, _rc...) \
    ({ FILE_SET_CODE(errno, ERRNO, _errno); \
       return _rc; })

/* set errno and return code, no branching */
#define FILE_ERROR_SET(_errno, _rc) \
    ({ FILE_SET_CODE(errno, ERRNO, (_errno)); \
       FILE_SET_CODE(rc, RC, (_rc)); })


/** Misc. stuff **/

/* iterate through all the volumes if volume < 0, else just the given volume */
#define FOR_EACH_VOLUME(volume, i) \
    for (int i = (IF_MV_VOL(volume) >= 0 ? IF_MV_VOL(volume) : 0), \
             _end = (IF_MV_VOL(volume) >= 0 ? i : NUM_VOLUMES-1);  \
         i <= _end; i++)

/* return a pointer to the static struct fat_direntry */
static inline struct fat_direntry *get_dir_fatent(void)
{
    extern struct fat_direntry dir_fatent;
    return &dir_fatent;
}

void iso_decode_d_name(char *d_name);

#ifdef HAVE_DIRCACHE
void empty_dirent(struct DIRENT *entry);
void fill_dirinfo_native(struct dirinfo_native *din);
#endif /* HAVE_DIRCACHE */

void filesystem_init(void) INIT_ATTR;

/** Filesystem dispatch wrappers **/
/* These take the filesystem-agnostic base structures and route each VFS
   operation to the FAT or HFS+ driver depending on the volume's filesystem
   type. When HFS+ support is not compiled in, every wrapper folds to a direct
   FAT call with no overhead and no behavioural change. */

#ifndef HAVE_HFSPLUS

/* FAT-only build: thin pass-throughs */
static inline int fs_open_rootdir(IF_MV(int volume,) struct file_base_info *infop)
    { return fat_open_rootdir(IF_MV(volume,) &infop->fatfile); }
static inline int fs_open(struct filestr_base *parent, long id,
                          struct file_base_info *infop)
    { return fat_open(parent->fatstr.fatfilep, id, &infop->fatfile); }
static inline void fs_filestr_init(struct filestr_base *stream,
                                   struct file_base_info *infop)
    { fat_filestr_init(&stream->fatstr, &infop->fatfile); }
static inline void fs_rewind(struct filestr_base *stream)
    { fat_rewind(&stream->fatstr); }
static inline int fs_readdir_info(struct filestr_base *stream,
                                  struct file_base_info *infop,
                                  struct fat_direntry *fatent)
    { return fat_readdir(&stream->fatstr, &infop->fatfile.e,
                         filestr_get_cache(stream), fatent); }
static inline int fs_readdir_scan(struct filestr_base *stream,
                                  struct dirscan_info *scanp,
                                  struct fat_direntry *fatent)
    { return fat_readdir(&stream->fatstr, &scanp->fatscan,
                         filestr_get_cache(stream), fatent); }
static inline void fs_rewinddir_info(struct file_base_info *infop)
    { fat_rewinddir(&infop->fatfile.e); }
static inline void fs_rewinddir_scan(struct dirscan_info *scanp)
    { fat_rewinddir(&scanp->fatscan); }
static inline int fs_seek(struct filestr_base *stream, unsigned long sector)
    { return fat_seek(&stream->fatstr, sector); }
static inline sector_t fs_query_sectornum(struct filestr_base *stream)
    { return fat_query_sectornum(&stream->fatstr); }
static inline long fs_readwrite(struct filestr_base *stream,
                                unsigned long sectorcount, void *buf, bool write)
    { return fat_readwrite(&stream->fatstr, sectorcount, buf, write); }
static inline bool fs_file_is_same(const struct file_base_info *a,
                                   const struct file_base_info *b)
    { return fat_file_is_same(&a->fatfile, &b->fatfile); }
#if defined(MAX_VARIABLE_LOG_SECTOR)
static inline int fs_file_sector_size(struct filestr_base *stream)
    { return fat_file_sector_size(IF_MV(stream->fatstr.fatfilep)); }
#else
#define fs_file_sector_size(stream) SECTOR_SIZE
#endif

#else /* HAVE_HFSPLUS */

/* derive the filesystem type from a base info / stream (the union always
   exposes the volume number) */
static inline int fs_type_of_info(const struct file_base_info *infop)
    { return volume_get_fstype(IF_MV(infop->volume)); }
static inline int fs_type_of_stream(const struct filestr_base *stream)
    { return volume_get_fstype(IF_MV(stream->infop->volume)); }

static inline int fs_open_rootdir(IF_MV(int volume,) struct file_base_info *infop)
{
    if (volume_get_fstype(IF_MV(volume)) == FS_HFSPLUS)
        return hfs_open_rootdir(IF_MV(volume,) &infop->hfsfile);
    return fat_open_rootdir(IF_MV(volume,) &infop->fatfile);
}
static inline int fs_open(struct filestr_base *parent, long id,
                          struct file_base_info *infop)
{
    if (fs_type_of_stream(parent) == FS_HFSPLUS)
        return hfs_open(parent->hfsstr.filep, id, &infop->hfsfile);
    return fat_open(parent->fatstr.fatfilep, id, &infop->fatfile);
}
static inline void fs_filestr_init(struct filestr_base *stream,
                                   struct file_base_info *infop)
{
    if (fs_type_of_info(infop) == FS_HFSPLUS)
        hfs_filestr_init(&stream->hfsstr, &infop->hfsfile);
    else
        fat_filestr_init(&stream->fatstr, &infop->fatfile);
}
static inline void fs_rewind(struct filestr_base *stream)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        hfs_rewind(&stream->hfsstr);
    else
        fat_rewind(&stream->fatstr);
}
static inline int fs_readdir_info(struct filestr_base *stream,
                                  struct file_base_info *infop,
                                  struct fat_direntry *fatent)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_readdir(&stream->hfsstr, &infop->hfsfile.e, fatent);
    return fat_readdir(&stream->fatstr, &infop->fatfile.e,
                       filestr_get_cache(stream), fatent);
}
static inline int fs_readdir_scan(struct filestr_base *stream,
                                  struct dirscan_info *scanp,
                                  struct fat_direntry *fatent)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_readdir(&stream->hfsstr, &scanp->hfsscan, fatent);
    return fat_readdir(&stream->fatstr, &scanp->fatscan,
                       filestr_get_cache(stream), fatent);
}
/* rewinddir resets the scan cursor to the "not started" sentinel. The FAT and
   HFS+ scan structures overlap (both are union members and the embedded copy
   in the file info is laid out to coincide), and both drivers treat the same
   reset state as "rewound", so no fs-type lookup is needed here. */
static inline void fs_rewinddir_info(struct file_base_info *infop)
    { fat_rewinddir(&infop->fatfile.e); }
static inline void fs_rewinddir_scan(struct dirscan_info *scanp)
    { fat_rewinddir(&scanp->fatscan); }
static inline int fs_seek(struct filestr_base *stream, unsigned long sector)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_seek(&stream->hfsstr, sector);
    return fat_seek(&stream->fatstr, sector);
}
static inline sector_t fs_query_sectornum(struct filestr_base *stream)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_query_sectornum(&stream->hfsstr);
    return fat_query_sectornum(&stream->fatstr);
}
static inline long fs_readwrite(struct filestr_base *stream,
                                unsigned long sectorcount, void *buf, bool write)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_readwrite(&stream->hfsstr, sectorcount, buf, write);
    return fat_readwrite(&stream->fatstr, sectorcount, buf, write);
}
static inline bool fs_file_is_same(const struct file_base_info *a,
                                   const struct file_base_info *b)
{
    if (fs_type_of_info(a) == FS_HFSPLUS)
        return hfs_file_is_same(&a->hfsfile, &b->hfsfile);
    return fat_file_is_same(&a->fatfile, &b->fatfile);
}
#if defined(MAX_VARIABLE_LOG_SECTOR)
static inline int fs_file_sector_size(struct filestr_base *stream)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_file_sector_size(IF_MV(stream->hfsstr.filep));
    return fat_file_sector_size(IF_MV(stream->fatstr.fatfilep));
}
#else
#define fs_file_sector_size(stream) SECTOR_SIZE
#endif

#endif /* HAVE_HFSPLUS */

/* write-path wrappers: route to the FAT or HFS+ driver by volume type. The
   HFS+ driver implements these for non-journaled volumes and returns
   HFS_RC_READONLY for journaled ones. On FAT-only builds they are direct
   pass-throughs. */
#ifdef HAVE_HFSPLUS
static inline int fs_truncate(struct filestr_base *stream)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_truncate(&stream->hfsstr);
    return fat_truncate(&stream->fatstr);
}
static inline int fs_closewrite(struct filestr_base *stream, uint32_t size,
                                struct fat_direntry *fatentp)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        return hfs_closewrite(&stream->hfsstr, size, fatentp);
    return fat_closewrite(&stream->fatstr, size, fatentp);
}
static inline void fs_seek_to_stream(struct filestr_base *stream,
                                     const struct filestr_base *seekto)
{
    if (fs_type_of_stream(stream) == FS_HFSPLUS)
        hfs_seek_to_stream(&stream->hfsstr, &seekto->hfsstr);
    else
        fat_seek_to_stream(&stream->fatstr, &seekto->fatstr);
}
static inline int fs_create_file(struct file_base_info *parentinfop,
                                 const char *name, uint8_t attr,
                                 struct file_base_info *infop,
                                 struct fat_direntry *fatentp)
{
    if (fs_type_of_info(parentinfop) == FS_HFSPLUS)
        return hfs_create(&parentinfop->hfsfile, name, attr,
                          &infop->hfsfile, fatentp);
    return fat_create_file(&parentinfop->fatfile, name, attr,
                           &infop->fatfile, fatentp);
}
static inline int fs_remove(struct file_base_info *infop, int what)
{
    if (fs_type_of_info(infop) == FS_HFSPLUS)
        return hfs_remove(&infop->hfsfile, what);
    return fat_remove(&infop->fatfile, what);
}
static inline int fs_rename(struct file_base_info *parentinfop,
                            struct file_base_info *fileinfop,
                            const unsigned char *newname)
{
    if (fs_type_of_info(parentinfop) == FS_HFSPLUS)
        return hfs_rename(&parentinfop->hfsfile, &fileinfop->hfsfile, newname);
    return fat_rename(&parentinfop->fatfile, &fileinfop->fatfile, newname);
}
static inline int fs_modtime(struct file_base_info *parentinfop,
                             struct filestr_base *stream, time_t modtime)
{
    if (fs_type_of_info(parentinfop) == FS_HFSPLUS)
        return hfs_modtime(&parentinfop->hfsfile, stream->hfsstr.filep, modtime);
    return fat_modtime(&parentinfop->fatfile, stream->fatstr.fatfilep, modtime);
}
#else /* !HAVE_HFSPLUS */
static inline int fs_truncate(struct filestr_base *stream)
    { return fat_truncate(&stream->fatstr); }
static inline int fs_closewrite(struct filestr_base *stream, uint32_t size,
                                struct fat_direntry *fatentp)
    { return fat_closewrite(&stream->fatstr, size, fatentp); }
static inline void fs_seek_to_stream(struct filestr_base *stream,
                                     const struct filestr_base *seekto)
    { fat_seek_to_stream(&stream->fatstr, &seekto->fatstr); }
static inline int fs_create_file(struct file_base_info *parentinfop,
                                 const char *name, uint8_t attr,
                                 struct file_base_info *infop,
                                 struct fat_direntry *fatentp)
    { return fat_create_file(&parentinfop->fatfile, name, attr,
                             &infop->fatfile, fatentp); }
static inline int fs_remove(struct file_base_info *infop, int what)
    { return fat_remove(&infop->fatfile, what); }
static inline int fs_rename(struct file_base_info *parentinfop,
                            struct file_base_info *fileinfop,
                            const unsigned char *newname)
    { return fat_rename(&parentinfop->fatfile, &fileinfop->fatfile, newname); }
static inline int fs_modtime(struct file_base_info *parentinfop,
                             struct filestr_base *stream, time_t modtime)
    { return fat_modtime(&parentinfop->fatfile, stream->fatstr.fatfilep, modtime); }
#endif /* HAVE_HFSPLUS */

#endif /* _FILE_INTERNAL_H_ */
