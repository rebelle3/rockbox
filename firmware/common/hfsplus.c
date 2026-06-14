/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Apple HFS+ / HFSX filesystem driver (for Mac-formatted iPods).
 *
 * Read support implements the operations the Rockbox VFS needs for browsing
 * and reading: mounting, opening the root directory, reading directory entries,
 * opening a file/dir by its catalog node id (CNID) and reading file data.
 *
 * The catalog is read by walking the B-tree leaf nodes linearly (the leaves
 * are chained and sorted by parent CNID), so no key comparison or HFS+ Unicode
 * case-folding is needed for reading: directory listing filters by parent CNID
 * and opening matches the object's own CNID. Path resolution is handled by the
 * generic VFS (readdir + strcasecmp), exactly as for FAT.
 *
 * Write support (see the second half of this file, omitted from the boot
 * loader) creates/deletes/renames files and directories and grows, overwrites
 * and truncates file data, keeping the catalog B-tree, allocation bitmap and
 * volume header consistent. Catalog inserts use the proper key ordering
 * (parent id, then Apple case-folded Unicode name), so the result passes
 * fsck_hfs and mounts cleanly elsewhere.
 *
 * Limitations: data fork only (no resource fork), first eight (inline) extents
 * only (the extents-overflow B-tree is neither read nor written, so a file may
 * use at most eight fragments), names are not Unicode-decomposed when created
 * (fine for the ASCII names Rockbox writes), and journaled volumes are mounted
 * read-only (no journal is maintained), with writes returning HFS_RC_READONLY.
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
#include <string.h>
#include "config.h"
#include "system.h"
#include "rbendian.h"
#include "rbunicode.h"
#include "storage.h"
#include "disk.h"
#include "fs_attr.h"
#include "fat.h"        /* struct fat_direntry, FAT_DIRENTRY_NAME_MAX */
#include "hfsplus.h"
#include "debug.h"

/* on-disk constants */
#define HFSP_VH_OFFSET      1024        /* volume header byte offset */
#define HFSP_SIG_HFSPLUS    0x482B      /* 'H+' */
#define HFSP_SIG_HFSX       0x4858      /* 'HX' */
#define HFSP_ROOT_CNID      2           /* kHFSRootFolderID */

/* catalog record types */
#define HFSP_FOLDER         0x0001
#define HFSP_FILE           0x0002
#define HFSP_FOLDER_THREAD  0x0003
#define HFSP_FILE_THREAD    0x0004

/* dir scan sentinels (must match struct fat_dirscan_info reset value, which is
   (unsigned)-1; see file_internal.h) */
#define HFSP_SCAN_REWOUND   0xFFFFFFFFu
#define HFSP_SCAN_DONE      0u

#define HFS_MAX_NODE_SIZE   16384

struct hfs_volinfo
{
    bool     mounted;
    int      drive;
    sector_t part_start;        /* partition start (logical sectors) */
    uint32_t block_size;        /* allocation block size (bytes) */
    uint32_t sectors_per_block; /* block_size / log_sector_size */
    uint32_t log_sector_size;   /* device logical sector size (bytes) */
    uint32_t total_blocks;      /* volume size in allocation blocks */
    /* catalog file fork */
    struct hfs_extent cat_extents[8];
    uint64_t cat_size;
    /* catalog B-tree geometry */
    uint16_t node_size;
    uint32_t first_leaf;
    /* --- fields used by the write path --- */
    bool     writable;          /* false for journaled volumes (refuse writes) */
    uint16_t sig;               /* volume signature */
    uint8_t  key_compare;       /* catalog keyCompareType (0xCF fold, 0xBC bin) */
    uint32_t free_blocks;       /* cached volume free block count */
    uint32_t next_cnid;         /* next catalog node id to hand out */
    uint32_t file_count;        /* volume file count */
    uint32_t folder_count;      /* volume folder count */
    uint32_t write_count;       /* volume write count */
    struct hfs_extent alloc_extents[8]; /* allocation (bitmap) file fork */
    uint64_t alloc_size;        /* allocation file size in bytes */
    /* catalog B-tree mutable header geometry */
    uint32_t cat_root;          /* root node */
    uint32_t cat_last_leaf;     /* last leaf node */
    uint16_t cat_tree_depth;    /* tree depth */
    uint32_t cat_total_nodes;   /* total nodes */
    uint32_t cat_free_nodes;    /* free nodes */
    uint32_t cat_leaf_records;  /* number of leaf records */
};

static struct hfs_volinfo hfs_vols[NUM_VOLUMES];

/* shared scratch buffers (filesystem operations are serialized by the VFS
   reader/writer lock, so single buffers are safe) */
static uint8_t hfs_sec_buf[HFS_MAX_NODE_SIZE > 4096 ? 4096 : 4096];
static uint8_t hfs_node_buf[HFS_MAX_NODE_SIZE];
static struct hfs_volinfo *node_cache_v;   /* which volume's node is buffered */
static uint32_t node_cache_node;           /* which node number is buffered */

static inline int hfs_volidx(const struct hfs_file *f)
{
#ifdef HAVE_MULTIVOLUME
    return f->volume;
#else
    (void)f;
    return 0;
#endif
}

static int hfs_read_sectors(struct hfs_volinfo *v, sector_t sec, int count,
                            void *buf)
{
#ifndef HAVE_MULTIDRIVE
    (void)v;
#endif
    return storage_read_sectors(IF_MD(v->drive,) sec, count, buf);
}

/* read 'len' bytes from a fork (given its inline extents) starting at byte
   'offset'; returns 0 on success, -1 if the offset maps past the inline
   extents or on I/O error */
static int hfs_fork_read(struct hfs_volinfo *v, const struct hfs_extent *ext,
                         uint64_t offset, void *buf, uint32_t len)
{
    uint8_t *out = buf;

    while (len > 0)
    {
        uint32_t fork_block = offset / v->block_size;
        uint32_t in_block   = offset % v->block_size;

        /* find the allocation block in the inline extents */
        uint32_t acc = 0, vblock = 0xFFFFFFFFu;
        for (int i = 0; i < 8; i++)
        {
            if (ext[i].block_count == 0)
                break;
            if (fork_block < acc + ext[i].block_count)
            {
                vblock = ext[i].start_block + (fork_block - acc);
                break;
            }
            acc += ext[i].block_count;
        }
        if (vblock == 0xFFFFFFFFu)
            return -1;

        uint32_t sec_in_block = in_block / v->log_sector_size;
        uint32_t off_in_sec   = in_block % v->log_sector_size;
        sector_t dev_sec = v->part_start +
                           (sector_t)vblock * v->sectors_per_block + sec_in_block;

        if (hfs_read_sectors(v, dev_sec, 1, hfs_sec_buf) < 0)
            return -1;

        uint32_t chunk = v->log_sector_size - off_in_sec;
        if (chunk > len)
            chunk = len;
        memcpy(out, hfs_sec_buf + off_in_sec, chunk);

        out    += chunk;
        offset += chunk;
        len    -= chunk;
    }

    return 0;
}

/* load a catalog B-tree node into hfs_node_buf (cached) */
static int hfs_read_node(struct hfs_volinfo *v, uint32_t node)
{
    if (node_cache_v == v && node_cache_node == node)
        return 0;

    uint64_t off = (uint64_t)node * v->node_size;
    if (hfs_fork_read(v, v->cat_extents, off, hfs_node_buf, v->node_size) < 0)
    {
        node_cache_v = NULL;
        return -1;
    }

    node_cache_v = v;
    node_cache_node = node;
    return 0;
}

/* number of records in the node currently in hfs_node_buf */
static uint16_t hfs_node_numrecords(void)
{
    return load_be16(hfs_node_buf + 10);
}

/* pointer to record 'idx' within the node currently in hfs_node_buf */
static const uint8_t *hfs_node_record(struct hfs_volinfo *v, int idx)
{
    uint16_t off = load_be16(hfs_node_buf + v->node_size - 2 * (idx + 1));
    return hfs_node_buf + off;
}

/* convert an HFS+ UTF-16BE name to a NUL-terminated UTF-8 string, bounded to
   FAT_DIRENTRY_NAME_MAX bytes */
static void hfs_name_to_utf8(const uint8_t *name16, uint16_t nchars, char *out)
{
    unsigned char *p = (unsigned char *)out;
    unsigned char *end = p + FAT_DIRENTRY_NAME_MAX - 4; /* leave room for one char + NUL */

    for (uint16_t i = 0; i < nchars && p < end; i++)
    {
        uint32_t uc = load_be16(name16 + i * 2);

        /* combine UTF-16 surrogate pairs */
        if (uc >= 0xD800 && uc <= 0xDBFF && (i + 1) < nchars)
        {
            uint32_t lo = load_be16(name16 + (i + 1) * 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                uc = 0x10000 + ((uc - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }

        if (uc == 0)
            uc = '_';       /* avoid embedded NUL */
        /* HFS+ stores '/' in a displayed name as ':' on disk; present it as
           ':' so it can't be confused with a path separator */
        p = utf8encode(uc, p);
    }

    *p = '\0';
}

/* parse an HFSPlusForkData (80 bytes) into inline extents + size */
static void hfs_parse_fork(const uint8_t *fd, struct hfs_extent *ext,
                           uint64_t *size)
{
    if (size)
        *size = load_be64(fd);
    for (int i = 0; i < 8; i++)
    {
        ext[i].start_block = load_be32(fd + 16 + i * 8);
        ext[i].block_count = load_be32(fd + 16 + i * 8 + 4);
    }
}

/** Mounting **/

int hfs_mount(IF_MV(int volume,) IF_MD(int drive,) unsigned long startsector)
{
    int vi = IF_MV_VOL(volume);
    struct hfs_volinfo *v = &hfs_vols[vi];

    memset(v, 0, sizeof(*v));
    v->drive = IF_MD_DRV(drive);
    v->part_start = startsector;
    v->log_sector_size = disk_get_log_sector_size(IF_MD(v->drive));

    if (v->log_sector_size == 0)
        goto fail;

    /* read the volume header */
    uint64_t vh_byte = HFSP_VH_OFFSET;
    sector_t vh_sec = startsector + vh_byte / v->log_sector_size;
    uint32_t vh_off = vh_byte % v->log_sector_size;

    if (hfs_read_sectors(v, vh_sec, 1, hfs_sec_buf) < 0)
        goto fail;

    const uint8_t *vh = hfs_sec_buf + vh_off;
    uint16_t sig = load_be16(vh);
    if (sig != HFSP_SIG_HFSPLUS && sig != HFSP_SIG_HFSX)
    {
        DEBUGF("hfs: bad signature 0x%04x\n", sig);
        goto fail;
    }

    v->sig          = sig;
    v->block_size   = load_be32(vh + 40);
    v->total_blocks = load_be32(vh + 44);
    v->free_blocks  = load_be32(vh + 48);
    v->next_cnid    = load_be32(vh + 64);
    v->file_count   = load_be32(vh + 32);
    v->folder_count = load_be32(vh + 36);
    v->write_count  = load_be32(vh + 68);
    /* writes are only allowed on a non-journaled volume (no journal to keep
       consistent). Journaled bit is kHFSVolumeJournaledMask (attributes bit 13). */
    v->writable = (load_be32(vh + 4) & (1u << 13)) == 0;
    /* allocation (bitmap) file fork lives at volume-header offset 112 */
    hfs_parse_fork(vh + 112, v->alloc_extents, &v->alloc_size);

    if (v->block_size == 0 || (v->block_size % v->log_sector_size) != 0)
    {
        DEBUGF("hfs: bad block size %lu\n", (unsigned long)v->block_size);
        goto fail;
    }
    v->sectors_per_block = v->block_size / v->log_sector_size;

    /* catalog file fork lives at volume-header offset 272 */
    hfs_parse_fork(vh + 272, v->cat_extents, &v->cat_size);

    /* read enough of catalog node 0 (the B-tree header node) to learn the
       node size and first leaf node */
    node_cache_v = NULL;
    if (hfs_fork_read(v, v->cat_extents, 0, hfs_node_buf, 512) < 0)
        goto fail;

    /* BTNodeDescriptor is 14 bytes; BTHeaderRec follows */
    v->cat_tree_depth   = load_be16(hfs_node_buf + 14 + 0);
    v->cat_root         = load_be32(hfs_node_buf + 14 + 2);
    v->cat_leaf_records = load_be32(hfs_node_buf + 14 + 6);
    v->first_leaf       = load_be32(hfs_node_buf + 14 + 10);
    v->cat_last_leaf    = load_be32(hfs_node_buf + 14 + 14);
    v->node_size        = load_be16(hfs_node_buf + 14 + 18);
    v->cat_total_nodes  = load_be32(hfs_node_buf + 14 + 22);
    v->cat_free_nodes   = load_be32(hfs_node_buf + 14 + 26);
    v->key_compare      = hfs_node_buf[14 + 37];

    if (v->node_size < 512 || v->node_size > HFS_MAX_NODE_SIZE)
    {
        DEBUGF("hfs: unsupported node size %u\n", v->node_size);
        goto fail;
    }

    node_cache_v = NULL; /* invalidate (we only read 512 bytes above) */
    v->mounted = true;
    DEBUGF("hfs: mounted vol %d bs=%lu nodes=%u\n", vi,
           (unsigned long)v->block_size, v->node_size);
    return 0;

fail:
    v->mounted = false;
    return -1;
}

int hfs_unmount(IF_MV_NONVOID(int volume))
{
    int vi = IF_MV_VOL(volume);
    hfs_vols[vi].mounted = false;
    if (node_cache_v == &hfs_vols[vi])
        node_cache_v = NULL;
    return 0;
}

bool hfs_ismounted(IF_MV_NONVOID(int volume))
{
    return hfs_vols[IF_MV_VOL(volume)].mounted;
}

void hfs_init(void)
{
    memset(hfs_vols, 0, sizeof(hfs_vols));
    node_cache_v = NULL;
}

bool hfs_size(IF_MV(int volume,) sector_t *size, sector_t *free)
{
    struct hfs_volinfo *v = &hfs_vols[IF_MV_VOL(volume)];
    if (!v->mounted)
        return false;
    if (size)
        *size = (sector_t)v->total_blocks * v->sectors_per_block;
    if (free)
        *free = (sector_t)v->free_blocks * v->sectors_per_block;
    return true;
}

/** Directory / file lookup **/

int hfs_open_rootdir(IF_MV(int volume,) struct hfs_file *dir)
{
    int vi = IF_MV_VOL(volume);
    if (!hfs_vols[vi].mounted)
        return -1;

    memset(dir, 0, sizeof(*dir));
#ifdef HAVE_MULTIVOLUME
    dir->volume = volume;
#endif
    dir->cnid = HFSP_ROOT_CNID;
    dir->parent = 1; /* kHFSRootParentID */
    dir->is_dir = 1;
    dir->e.node = HFSP_SCAN_REWOUND;
    return 0;
}

/* fill a struct hfs_file from a catalog file/folder record body */
static void hfs_fill_from_record(struct hfs_file *file, const uint8_t *data,
                                 int16_t type)
{
    if (type == HFSP_FOLDER)
    {
        file->is_dir = 1;
        file->cnid = load_be32(data + 8);   /* folderID */
        file->size = 0;
    }
    else /* HFSP_FILE */
    {
        file->is_dir = 0;
        file->cnid = load_be32(data + 8);   /* fileID */
        /* HFSPlusForkData for the data fork starts at record offset 88 */
        hfs_parse_fork(data + 88, file->extents, &file->size);
        file->total_blocks = load_be32(data + 88 + 12);
    }
    file->e.node = HFSP_SCAN_REWOUND;
}

int hfs_open(const struct hfs_file *parent, long cnid, struct hfs_file *file)
{
    int vi = hfs_volidx(parent);
    struct hfs_volinfo *v = &hfs_vols[vi];

    if (!v->mounted)
        return -1;

    memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
    file->volume = parent->volume;
#endif
    file->e.node = HFSP_SCAN_REWOUND;

    if ((uint32_t)cnid == HFSP_ROOT_CNID)
    {
        file->cnid = HFSP_ROOT_CNID;
        file->is_dir = 1;
        return 0;
    }

    /* linear walk of the catalog leaves for the matching CNID */
    uint32_t node = v->first_leaf;
    while (node != 0)
    {
        if (hfs_read_node(v, node) < 0)
            return -1;

        uint16_t nrec = hfs_node_numrecords();
        for (int r = 0; r < nrec; r++)
        {
            const uint8_t *rec = hfs_node_record(v, r);
            uint16_t keylen = load_be16(rec);
            const uint8_t *data = rec + 2 + keylen;
            int16_t type = (int16_t)load_be16(data);

            if ((type == HFSP_FOLDER || type == HFSP_FILE) &&
                load_be32(data + 8) == (uint32_t)cnid)
            {
                hfs_fill_from_record(file, data, type);
                file->parent = load_be32(rec + 2); /* key parentID */
                return 0;
            }
        }
        node = load_be32(hfs_node_buf); /* fLink */
    }

    return -1; /* not found */
}

bool hfs_file_is_same(const struct hfs_file *file1, const struct hfs_file *file2)
{
#ifdef HAVE_MULTIVOLUME
    if (file1->volume != file2->volume)
        return false;
#endif
    return file1->cnid == file2->cnid;
}

int hfs_readdir(struct hfs_filestr *dirstr, struct hfs_dirscan_info *scan,
                struct fat_direntry *entry)
{
    struct hfs_file *dir = dirstr->filep;
    int vi = hfs_volidx(dir);
    struct hfs_volinfo *v = &hfs_vols[vi];
    uint32_t dir_cnid = dir->cnid;

    /* clear the result; the VFS copies fields unconditionally */
    memset(entry, 0, sizeof(*entry));

    if (!v->mounted)
        return -1;

    if (scan->node == HFSP_SCAN_REWOUND)
    {
        scan->node = v->first_leaf;
        scan->record = 0;
    }

    while (scan->node != HFSP_SCAN_DONE)
    {
        if (hfs_read_node(v, scan->node) < 0)
            return -1;

        uint16_t nrec = hfs_node_numrecords();
        while (scan->record < nrec)
        {
            const uint8_t *rec = hfs_node_record(v, scan->record);
            uint16_t keylen = load_be16(rec);
            uint32_t parent = load_be32(rec + 2);

            if (parent < dir_cnid)
            {
                scan->record++;
                continue;
            }
            if (parent > dir_cnid)
            {
                scan->node = HFSP_SCAN_DONE; /* sorted: no more children */
                return 0;
            }

            /* parent == dir_cnid */
            const uint8_t *data = rec + 2 + keylen;
            int16_t type = (int16_t)load_be16(data);

            if (type == HFSP_FOLDER || type == HFSP_FILE)
            {
                uint16_t namelen = load_be16(rec + 6);
                hfs_name_to_utf8(rec + 8, namelen, entry->name);

                if (type == HFSP_FOLDER)
                {
                    entry->attr = ATTR_DIRECTORY;
                    entry->firstcluster = load_be32(data + 8);
                    entry->filesize = 0;
                }
                else
                {
                    entry->attr = 0;
                    entry->firstcluster = load_be32(data + 8);
                    entry->filesize = (uint32_t)load_be64(data + 88);
                }

                scan->record++;
                return 1;
            }

            /* thread record or anything else: skip */
            scan->record++;
        }

        /* advance to the next leaf node */
        scan->node = load_be32(hfs_node_buf); /* fLink */
        scan->record = 0;
    }

    return 0; /* end of directory */
}

void hfs_rewinddir(struct hfs_dirscan_info *scan)
{
    scan->node = HFSP_SCAN_REWOUND;
    scan->record = 0;
}

/** File stream read path **/

void hfs_filestr_init(struct hfs_filestr *filestr, struct hfs_file *file)
{
    filestr->filep = file;
    filestr->cur_sector = 0;
    filestr->eof = false;
}

void hfs_rewind(struct hfs_filestr *filestr)
{
    filestr->cur_sector = 0;
    filestr->eof = false;
}

int hfs_seek(struct hfs_filestr *filestr, unsigned long sector)
{
    filestr->cur_sector = sector;
    filestr->eof = false;
    return 0;
}

sector_t hfs_query_sectornum(const struct hfs_filestr *filestr)
{
    return filestr->cur_sector;
}

#ifndef BOOTLOADER
static long hfs_write_data(struct hfs_filestr *filestr,
                           unsigned long sectorcount, void *buf);
#endif

long hfs_readwrite(struct hfs_filestr *filestr, unsigned long sectorcount,
                   void *buf, bool write)
{
    if (write)
#ifndef BOOTLOADER
        return hfs_write_data(filestr, sectorcount, buf);
#else
        return HFS_RC_READONLY;
#endif

    struct hfs_file *f = filestr->filep;
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(f)];
    uint32_t spb = v->sectors_per_block;
    uint8_t *out = buf;
    long done = 0;

    uint64_t file_sectors =
        (f->size + v->log_sector_size - 1) / v->log_sector_size;

    while (sectorcount > 0)
    {
        if (filestr->cur_sector >= file_sectors)
        {
            filestr->eof = true;
            break;
        }

        uint32_t fork_block   = filestr->cur_sector / spb;
        uint32_t sec_in_block = filestr->cur_sector % spb;

        /* locate the extent run containing this fork block */
        uint32_t acc = 0, vblock = 0xFFFFFFFFu, run_blocks = 0;
        for (int i = 0; i < 8; i++)
        {
            if (f->extents[i].block_count == 0)
                break;
            if (fork_block < acc + f->extents[i].block_count)
            {
                uint32_t off = fork_block - acc;
                vblock = f->extents[i].start_block + off;
                run_blocks = f->extents[i].block_count - off;
                break;
            }
            acc += f->extents[i].block_count;
        }
        if (vblock == 0xFFFFFFFFu)
            return done > 0 ? done : -1; /* needs extents-overflow B-tree */

        sector_t dev_sec = v->part_start +
                           (sector_t)vblock * spb + sec_in_block;

        /* contiguous sectors available within this extent run */
        uint64_t run = (uint64_t)run_blocks * spb - sec_in_block;
        if (run > sectorcount)
            run = sectorcount;
        if (filestr->cur_sector + run > file_sectors)
            run = file_sectors - filestr->cur_sector;

        if (hfs_read_sectors(v, dev_sec, (int)run, out) < 0)
            return done > 0 ? done : -1;

        out += run * v->log_sector_size;
        filestr->cur_sector += run;
        sectorcount -= run;
        done += run;
    }

    return done;
}

#ifndef BOOTLOADER
/*===========================================================================
 * Write support (not built for the bootloader, which only reads).
 *
 * Implements the operations the Rockbox VFS needs to modify a non-journaled
 * HFS+/HFSX volume: growing, overwriting and truncating file data, creating
 * and deleting files and directories, and renaming/moving. The catalog B-tree
 * is kept correctly ordered (by parent id, then case-folded Unicode name) so
 * the result mounts cleanly elsewhere and passes fsck_hfs.
 *
 * Limitations: only the eight inline data-fork extents are used (the
 * extents-overflow B-tree is not written, so a single file may end up in at
 * most eight fragments), the resource fork is left empty, names are not
 * Unicode-decomposed (fine for the ASCII names Rockbox creates), and journaled
 * volumes are refused (v->writable == false).
 *==========================================================================*/

#include "hfsplus_casefold.h"   /* hfs_case_fold_table[] */
#include "time.h"
#include "timefuncs.h"          /* get_time() */

/* HFS+ timestamps count seconds from 1904-01-01; Unix time from 1970. */
#define HFS_MAC_EPOCH_OFFSET    2082844800u

/* current time as an HFS+ (Mac-epoch, local) timestamp */
static uint32_t hfs_now(void)
{
    return (uint32_t)mktime(get_time()) + HFS_MAC_EPOCH_OFFSET;
}

/* B-tree node kinds */
#define HFS_BT_LEAF     0xFF    /* kBTLeafNode (stored as int8 -1) */
#define HFS_BT_INDEX    0x00
#define HFS_BT_MAP      0x02

#define HFS_MAX_BTREE_DEPTH     16
#define HFS_KEY_NAME_MAX        255 /* code units in a catalog name */

/* scratch buffers; filesystem ops are serialized by the VFS reader/writer lock
   so single shared buffers are safe */
static uint8_t hfs_node_buf2[HFS_MAX_NODE_SIZE]; /* sibling / header scratch */
static uint8_t hfs_node_buf3[HFS_MAX_NODE_SIZE]; /* split right-half scratch */
static uint8_t hfs_rec_buf[800];                 /* assemble a catalog record */
static uint8_t hfs_promo_buf[800];               /* promoted index record */
static uint8_t hfs_promo_buf2[800];
static uint8_t hfs_body_buf[256];                /* saved record body (rename) */
static uint16_t hfs_name16[HFS_KEY_NAME_MAX];    /* working UTF-16 name */

struct hfs_bt_path { uint32_t node; int idx; };

/* big-endian store helpers */
static inline void put_be16(uint8_t *p, uint16_t v)
    { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put_be32(uint8_t *p, uint32_t v)
    { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
      p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void put_be64(uint8_t *p, uint64_t v)
    { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }

static int hfs_write_sectors(struct hfs_volinfo *v, sector_t sec, int count,
                             const void *buf)
{
#ifndef HAVE_MULTIDRIVE
    (void)v;
#endif
    return storage_write_sectors(IF_MD(v->drive,) sec, count, buf);
}

/* map a fork byte offset (via inline extents) to a device sector + remainder */
static int hfs_fork_map(struct hfs_volinfo *v, const struct hfs_extent *ext,
                        uint64_t offset, sector_t *dev_sec, uint32_t *off_in_sec)
{
    uint32_t fork_block = offset / v->block_size;
    uint32_t in_block   = offset % v->block_size;
    uint32_t acc = 0, vblock = 0xFFFFFFFFu;
    for (int i = 0; i < 8; i++)
    {
        if (ext[i].block_count == 0)
            break;
        if (fork_block < acc + ext[i].block_count)
        {
            vblock = ext[i].start_block + (fork_block - acc);
            break;
        }
        acc += ext[i].block_count;
    }
    if (vblock == 0xFFFFFFFFu)
        return -1;
    *dev_sec = v->part_start + (sector_t)vblock * v->sectors_per_block
               + in_block / v->log_sector_size;
    *off_in_sec = in_block % v->log_sector_size;
    return 0;
}

/* read-modify-write 'len' bytes into a fork at byte 'offset' */
static int hfs_fork_write(struct hfs_volinfo *v, const struct hfs_extent *ext,
                          uint64_t offset, const void *buf, uint32_t len)
{
    const uint8_t *in = buf;
    while (len > 0)
    {
        sector_t dev_sec; uint32_t off_in_sec;
        if (hfs_fork_map(v, ext, offset, &dev_sec, &off_in_sec) < 0)
            return -1;
        uint32_t chunk = v->log_sector_size - off_in_sec;
        if (chunk > len)
            chunk = len;
        if (chunk == v->log_sector_size)
        {
            if (hfs_write_sectors(v, dev_sec, 1, in) < 0)
                return -1;
        }
        else
        {
            if (hfs_read_sectors(v, dev_sec, 1, hfs_sec_buf) < 0)
                return -1;
            memcpy(hfs_sec_buf + off_in_sec, in, chunk);
            if (hfs_write_sectors(v, dev_sec, 1, hfs_sec_buf) < 0)
                return -1;
        }
        in += chunk; offset += chunk; len -= chunk;
    }
    return 0;
}

/* write a catalog B-tree node from buffer 'nb' back to disk */
static int hfs_write_node(struct hfs_volinfo *v, uint32_t node, const uint8_t *nb)
{
    if (hfs_fork_write(v, v->cat_extents, (uint64_t)node * v->node_size, nb,
                       v->node_size) < 0)
        return -1;
    if (nb == hfs_node_buf)
    {
        node_cache_v = v;
        node_cache_node = node;
    }
    else if (node_cache_v == v && node_cache_node == node)
    {
        node_cache_v = NULL; /* a different buffer now defines this node */
    }
    return 0;
}

/** Volume header & catalog header persistence **/

static int hfs_vh_flush(struct hfs_volinfo *v)
{
    uint64_t vh_byte = HFSP_VH_OFFSET;
    sector_t vh_sec = v->part_start + vh_byte / v->log_sector_size;
    if (hfs_read_sectors(v, vh_sec, 1, hfs_sec_buf) < 0)
        return -1;
    uint8_t *vh = hfs_sec_buf + vh_byte % v->log_sector_size;
    put_be32(vh + 32, v->file_count);
    put_be32(vh + 36, v->folder_count);
    put_be32(vh + 48, v->free_blocks);
    put_be32(vh + 64, v->next_cnid);
    put_be32(vh + 68, ++v->write_count);
    if (hfs_write_sectors(v, vh_sec, 1, hfs_sec_buf) < 0)
        return -1;
    /* keep the alternate (backup) volume header in sync (1024 bytes before the
       end of the embedded volume) so the result passes fsck */
    uint64_t alt_byte = (uint64_t)v->total_blocks * v->block_size - 1024;
    sector_t alt_sec = v->part_start + alt_byte / v->log_sector_size;
    if ((alt_byte % v->log_sector_size) == (vh_byte % v->log_sector_size))
        return hfs_write_sectors(v, alt_sec, 1, hfs_sec_buf);
    return 0;
}

static int hfs_cat_header_flush(struct hfs_volinfo *v)
{
    if (hfs_fork_read(v, v->cat_extents, 0, hfs_node_buf2, v->node_size) < 0)
        return -1;
    uint8_t *h = hfs_node_buf2 + 14;
    put_be16(h + 0,  v->cat_tree_depth);
    put_be32(h + 2,  v->cat_root);
    put_be32(h + 6,  v->cat_leaf_records);
    put_be32(h + 10, v->first_leaf);
    put_be32(h + 14, v->cat_last_leaf);
    put_be32(h + 22, v->cat_total_nodes);
    put_be32(h + 26, v->cat_free_nodes);
    return hfs_write_node(v, 0, hfs_node_buf2);
}

/** Allocation bitmap **/

static bool hfs_bmp_test(struct hfs_volinfo *v, uint32_t blk)
{
    uint8_t byte = 0;
    if (hfs_fork_read(v, v->alloc_extents, blk / 8, &byte, 1) < 0)
        return true; /* treat I/O error as "used" so we never hand it out */
    return (byte & (0x80 >> (blk & 7))) != 0;
}

static int hfs_bmp_set(struct hfs_volinfo *v, uint32_t blk, bool used)
{
    uint8_t byte = 0;
    if (hfs_fork_read(v, v->alloc_extents, blk / 8, &byte, 1) < 0)
        return -1;
    if (used) byte |=  (0x80 >> (blk & 7));
    else      byte &= ~(0x80 >> (blk & 7));
    return hfs_fork_write(v, v->alloc_extents, blk / 8, &byte, 1);
}

static int hfs_bmp_mark(struct hfs_volinfo *v, uint32_t start, uint32_t count,
                        bool used)
{
    for (uint32_t i = 0; i < count; i++)
        if (hfs_bmp_set(v, start + i, used) < 0)
            return -1;
    return 0;
}

/* find the first run of free blocks (length capped at 'want'); returns the run
   length (0 if none) and sets *start. Scans the bitmap a sector at a time. */
static uint32_t hfs_bmp_find_free(struct hfs_volinfo *v, uint32_t want,
                                  uint32_t *start)
{
    uint32_t total = v->total_blocks;
    uint32_t nbytes = (total + 7) / 8;
    uint32_t run_start = 0, run_len = 0;
    sector_t cached = (sector_t)-1;
    for (uint32_t by = 0; by < nbytes; by++)
    {
        sector_t dev_sec; uint32_t off;
        if (hfs_fork_map(v, v->alloc_extents, by, &dev_sec, &off) < 0)
            break;
        if (dev_sec != cached)
        {
            if (hfs_read_sectors(v, dev_sec, 1, hfs_sec_buf) < 0)
                break;
            cached = dev_sec;
        }
        uint8_t byte = hfs_sec_buf[off];
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t blk = by * 8 + bit;
            if (blk >= total)
                break;
            if (!(byte & (0x80 >> bit)))
            {
                if (run_len == 0)
                    run_start = blk;
                if (++run_len >= want)
                {
                    *start = run_start;
                    return want;
                }
            }
            else if (run_len > 0)
            {
                *start = run_start;
                return run_len;     /* first free run, shorter than wanted */
            }
        }
    }
    if (run_len > 0)
    {
        *start = run_start;
        return run_len;
    }
    return 0;
}

/* allocate up to 'want' contiguous free blocks; returns count, sets *start */
static uint32_t hfs_alloc_find(struct hfs_volinfo *v, uint32_t want,
                               uint32_t *start)
{
    uint32_t got = hfs_bmp_find_free(v, want, start);
    if (got == 0)
        return 0;
    if (hfs_bmp_mark(v, *start, got, true) < 0)
        return 0;
    v->free_blocks -= got;
    return got;
}

/* try to allocate up to 'want' blocks starting exactly at 'at' */
static uint32_t hfs_alloc_at(struct hfs_volinfo *v, uint32_t at, uint32_t want)
{
    uint32_t cnt = 0;
    while (cnt < want && (at + cnt) < v->total_blocks && !hfs_bmp_test(v, at + cnt))
        cnt++;
    if (cnt == 0)
        return 0;
    if (hfs_bmp_mark(v, at, cnt, true) < 0)
        return 0;
    v->free_blocks -= cnt;
    return cnt;
}

/* free 'count' blocks starting at 'start' */
static void hfs_alloc_free(struct hfs_volinfo *v, uint32_t start, uint32_t count)
{
    if (count == 0)
        return;
    if (hfs_bmp_mark(v, start, count, false) == 0)
        v->free_blocks += count;
}

/* grow a file's inline extents to cover at least 'need_blocks' blocks */
static int hfs_grow(struct hfs_volinfo *v, struct hfs_file *f,
                    uint32_t need_blocks)
{
    while (f->total_blocks < need_blocks)
    {
        uint32_t want = need_blocks - f->total_blocks;

        /* extend the last extent in place if the next blocks are free */
        int li = -1;
        for (int i = 0; i < 8; i++)
            if (f->extents[i].block_count)
                li = i;
        if (li >= 0)
        {
            uint32_t hint = f->extents[li].start_block + f->extents[li].block_count;
            uint32_t got = hfs_alloc_at(v, hint, want);
            if (got)
            {
                f->extents[li].block_count += got;
                f->total_blocks += got;
                continue;
            }
        }

        /* otherwise start a new extent */
        int slot = -1;
        for (int i = 0; i < 8; i++)
            if (f->extents[i].block_count == 0) { slot = i; break; }
        if (slot < 0)
            return HFS_RC_ENOSPC;   /* would need the extents-overflow B-tree */

        uint32_t start, got = hfs_alloc_find(v, want, &start);
        if (got == 0)
            return HFS_RC_ENOSPC;
        f->extents[slot].start_block = start;
        f->extents[slot].block_count = got;
        f->total_blocks += got;
    }
    return 0;
}

/* shrink a file's data fork to 'keep_blocks' blocks, freeing the rest */
static void hfs_shrink(struct hfs_volinfo *v, struct hfs_file *f,
                       uint32_t keep_blocks)
{
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++)
    {
        uint32_t bc = f->extents[i].block_count;
        if (bc == 0)
            continue;
        if (acc >= keep_blocks)
        {
            /* entire extent is beyond the kept range */
            hfs_alloc_free(v, f->extents[i].start_block, bc);
            f->extents[i].start_block = 0;
            f->extents[i].block_count = 0;
        }
        else if (acc + bc > keep_blocks)
        {
            /* split this extent: keep the head, free the tail */
            uint32_t keep = keep_blocks - acc;
            hfs_alloc_free(v, f->extents[i].start_block + keep, bc - keep);
            f->extents[i].block_count = keep;
        }
        acc += bc;
    }
    f->total_blocks = keep_blocks < f->total_blocks ? keep_blocks : f->total_blocks;
}

/** Catalog B-tree node record helpers **/

static inline uint16_t bt_numrec(const uint8_t *nb)
    { return load_be16(nb + 10); }
static inline uint16_t bt_recoff(const uint8_t *nb, uint16_t ns, int i)
    { return load_be16(nb + ns - 2 * (i + 1)); }
static inline void bt_set_recoff(uint8_t *nb, uint16_t ns, int i, uint16_t off)
    { put_be16(nb + ns - 2 * (i + 1), off); }
static inline uint16_t bt_freeoff(const uint8_t *nb, uint16_t ns)
    { return bt_recoff(nb, ns, bt_numrec(nb)); }
/* free space available for one more record's payload (excludes its offset slot) */
static inline int bt_freespace(const uint8_t *nb, uint16_t ns)
    { return (int)ns - 2 * (bt_numrec(nb) + 2) - bt_freeoff(nb, ns); }

/* insert 'len' bytes of record 'rec' at record index 'idx' */
static void bt_insert_rec(uint8_t *nb, uint16_t ns, int idx,
                          const uint8_t *rec, uint16_t len)
{
    int n = bt_numrec(nb);
    uint16_t at  = bt_recoff(nb, ns, idx);
    uint16_t end = bt_freeoff(nb, ns);
    memmove(nb + at + len, nb + at, end - at);
    memcpy(nb + at, rec, len);
    for (int i = n; i >= idx; i--)
        bt_set_recoff(nb, ns, i + 1, bt_recoff(nb, ns, i) + len);
    bt_set_recoff(nb, ns, idx, at);
    put_be16(nb + 10, n + 1);
}

/* delete record at index 'idx' */
static void bt_delete_rec(uint8_t *nb, uint16_t ns, int idx)
{
    int n = bt_numrec(nb);
    uint16_t start = bt_recoff(nb, ns, idx);
    uint16_t next  = bt_recoff(nb, ns, idx + 1);
    uint16_t len   = next - start;
    uint16_t end   = bt_freeoff(nb, ns);
    memmove(nb + start, nb + next, end - next);
    for (int i = idx + 1; i <= n; i++)
        bt_set_recoff(nb, ns, i - 1, bt_recoff(nb, ns, i) - len);
    put_be16(nb + 10, n - 1);
}

/** Catalog B-tree node allocation (free-node bitmap in the header node) **/

static int hfs_bt_alloc_node(struct hfs_volinfo *v, uint32_t *out)
{
    if (v->cat_free_nodes == 0)
        return -1; /* would need to grow the catalog fork (unsupported) */
    if (hfs_fork_read(v, v->cat_extents, 0, hfs_node_buf2, v->node_size) < 0)
        return -1;
    uint16_t maprec = bt_recoff(hfs_node_buf2, v->node_size, 2);
    uint8_t *map = hfs_node_buf2 + maprec;
    for (uint32_t n = 0; n < v->cat_total_nodes; n++)
    {
        if (!(map[n / 8] & (0x80 >> (n & 7))))
        {
            map[n / 8] |= (0x80 >> (n & 7));
            if (hfs_write_node(v, 0, hfs_node_buf2) < 0)
                return -1;
            v->cat_free_nodes--;
            *out = n;
            return 0;
        }
    }
    return -1;
}

static int hfs_bt_free_node(struct hfs_volinfo *v, uint32_t node)
{
    if (hfs_fork_read(v, v->cat_extents, 0, hfs_node_buf2, v->node_size) < 0)
        return -1;
    uint16_t maprec = bt_recoff(hfs_node_buf2, v->node_size, 2);
    uint8_t *map = hfs_node_buf2 + maprec;
    map[node / 8] &= ~(0x80 >> (node & 7));
    if (hfs_write_node(v, 0, hfs_node_buf2) < 0)
        return -1;
    v->cat_free_nodes++;
    return 0;
}

/** Catalog key comparison **/

static uint16_t hfs_fold(uint16_t c)
{
    uint16_t t = hfs_case_fold_table[c >> 8];
    return t ? hfs_case_fold_table[t + (c & 0xff)] : c;
}

/* compare search key (p1,name1[len1] in host UTF-16) against the catalog key of
   record 'idx' in node 'nb'. Returns <0, 0 or >0. */
static int hfs_cat_cmp(struct hfs_volinfo *v, uint32_t p1,
                       const uint16_t *name1, int len1,
                       const uint8_t *nb, uint16_t ns, int idx)
{
    const uint8_t *rec = nb + bt_recoff(nb, ns, idx);
    uint32_t p2 = load_be32(rec + 2);
    if (p1 != p2)
        return p1 < p2 ? -1 : 1;

    int len2 = load_be16(rec + 6);
    const uint8_t *name2 = rec + 8;
    bool fold = (v->key_compare != 0xBC);   /* 0xBC = kHFSBinaryCompare */
    int i = 0, j = 0;
    for (;;)
    {
        uint16_t c1 = 0, c2 = 0;
        if (fold)
        {
            while (i < len1) { c1 = hfs_fold(name1[i]); i++; if (c1) break; c1 = 0; }
            while (j < len2) { c2 = hfs_fold(load_be16(name2 + j*2)); j++; if (c2) break; c2 = 0; }
        }
        else
        {
            if (i < len1) c1 = name1[i++];
            if (j < len2) c2 = load_be16(name2 + j*2), j++;
        }
        if (c1 != c2)
            return c1 < c2 ? -1 : 1;
        if (c1 == 0)
            return 0;
    }
}

/* convert a UTF-8 string to host-order UTF-16; returns code-unit count */
static int hfs_str_to_utf16(const char *utf8, uint16_t *out, int maxchars)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p && n < maxchars)
    {
        ucschar_t ucs;
        p = utf8decode(p, &ucs);
        out[n++] = (uint16_t)ucs;
    }
    return n;
}

/** Catalog B-tree search & mutation **/

/* descend to the leaf for (parent, name). On return *found tells whether an
   exact match exists; leaf_out and idx_out give the leaf node and the record
   index (of the match, or where it would be inserted); path[0..*npath-1]
   record the index-node descent for split/merge propagation. */
static int hfs_cat_search(struct hfs_volinfo *v, uint32_t parent,
                          const uint16_t *name16, int namelen,
                          struct hfs_bt_path *path, int *npath,
                          uint32_t *leaf_out, int *idx_out, bool *found)
{
    uint32_t node = v->cat_root;
    int depth = 0;
    *found = false;
    for (;;)
    {
        if (hfs_read_node(v, node) < 0)
            return -1;
        int kind = (int8_t)hfs_node_buf[8];
        int n = bt_numrec(hfs_node_buf);
        if (kind == HFS_BT_INDEX)
        {
            int child = 0;
            for (int i = 0; i < n; i++)
            {
                if (hfs_cat_cmp(v, parent, name16, namelen,
                                hfs_node_buf, v->node_size, i) >= 0)
                    child = i;
                else
                    break;
            }
            if (depth < HFS_MAX_BTREE_DEPTH)
            {
                path[depth].node = node;
                path[depth].idx  = child;
            }
            depth++;
            const uint8_t *rec = hfs_node_buf +
                                 bt_recoff(hfs_node_buf, v->node_size, child);
            uint16_t kl = load_be16(rec);
            node = load_be32(rec + 2 + kl);
        }
        else /* leaf */
        {
            int i;
            for (i = 0; i < n; i++)
            {
                int c = hfs_cat_cmp(v, parent, name16, namelen,
                                    hfs_node_buf, v->node_size, i);
                if (c == 0) { *found = true; break; }
                if (c < 0) break;
            }
            *leaf_out = node;
            *idx_out  = i;
            *npath    = depth;
            return 0;
        }
    }
}

/* fetch logical record m of 'src' as if 'rec'(len) were inserted at 'pos' */
static void bt_logical_rec(const uint8_t *src, uint16_t ns, int pos,
                           const uint8_t *rec, uint16_t len, int m,
                           const uint8_t **p, uint16_t *l)
{
    if (m < pos)
    {
        uint16_t o = bt_recoff(src, ns, m);
        *p = src + o; *l = bt_recoff(src, ns, m + 1) - o;
    }
    else if (m == pos)
    {
        *p = rec; *l = len;
    }
    else
    {
        uint16_t o = bt_recoff(src, ns, m - 1);
        *p = src + o; *l = bt_recoff(src, ns, m) - o;
    }
}

/* build node 'dst' from logical records [from,to) of (src + rec@pos) */
static void bt_build_node(uint8_t *dst, uint16_t ns, const uint8_t *src,
                          int pos, const uint8_t *rec, uint16_t len,
                          int from, int to)
{
    uint16_t off = 14;
    int cnt = 0;
    for (int m = from; m < to; m++)
    {
        const uint8_t *p; uint16_t l;
        bt_logical_rec(src, ns, pos, rec, len, m, &p, &l);
        memcpy(dst + off, p, l);
        bt_set_recoff(dst, ns, cnt, off);
        off += l;
        cnt++;
    }
    bt_set_recoff(dst, ns, cnt, off);
    put_be16(dst + 10, cnt);
}

/* insert record 'rec'(len) at index 'pos' into 'node'. If it fits, write it and
   set *promoted=false. Otherwise split, writing both halves, and return in
   promo/plen the index record (separator key + child pointer) to insert into
   the parent. */
static int hfs_node_insert(struct hfs_volinfo *v, uint32_t node, int pos,
                           const uint8_t *rec, uint16_t len,
                           bool *promoted, uint8_t *promo, uint16_t *plen)
{
    uint16_t ns = v->node_size;
    if (hfs_read_node(v, node) < 0)
        return -1;
    if (bt_freespace(hfs_node_buf, ns) >= (int)len)
    {
        bt_insert_rec(hfs_node_buf, ns, pos, rec, len);
        *promoted = false;
        return hfs_write_node(v, node, hfs_node_buf);
    }

    /* split required */
    uint32_t sib;
    if (hfs_bt_alloc_node(v, &sib) < 0)
        return HFS_RC_ENOSPC;
    if (hfs_read_node(v, node) < 0)
        return -1;
    memcpy(hfs_node_buf2, hfs_node_buf, ns);   /* source snapshot */

    int kind = (int8_t)hfs_node_buf2[8];
    uint8_t height = hfs_node_buf2[9];
    uint32_t old_fl = load_be32(hfs_node_buf2);
    uint32_t old_bl = load_be32(hfs_node_buf2 + 4);
    int n = bt_numrec(hfs_node_buf2);
    int total = n + 1;

    /* choose split point by byte balance */
    uint16_t used = bt_freeoff(hfs_node_buf2, ns) - 14 + len;
    uint16_t half = used / 2, acc = 0;
    int k = 1;
    for (int m = 0; m < total - 1; m++)
    {
        const uint8_t *p; uint16_t l;
        bt_logical_rec(hfs_node_buf2, ns, pos, rec, len, m, &p, &l);
        acc += l;
        if (acc >= half) { k = m + 1; break; }
        k = m + 1;
    }
    if (k < 1) k = 1;
    if (k > total - 1) k = total - 1;

    bt_build_node(hfs_node_buf,  ns, hfs_node_buf2, pos, rec, len, 0, k);
    bt_build_node(hfs_node_buf3, ns, hfs_node_buf2, pos, rec, len, k, total);

    hfs_node_buf[8] = (uint8_t)kind;  hfs_node_buf[9] = height;
    put_be32(hfs_node_buf, sib);          /* left.fLink  = sib   */
    put_be32(hfs_node_buf + 4, old_bl);   /* left.bLink  = old   */
    hfs_node_buf3[8] = (uint8_t)kind; hfs_node_buf3[9] = height;
    put_be32(hfs_node_buf3, old_fl);      /* right.fLink = old fl */
    put_be32(hfs_node_buf3 + 4, node);    /* right.bLink = left  */

    if (hfs_write_node(v, node, hfs_node_buf) < 0)
        return -1;
    if (hfs_write_node(v, sib, hfs_node_buf3) < 0)
        return -1;
    if (old_fl)
    {
        if (hfs_read_node(v, old_fl) < 0)
            return -1;
        put_be32(hfs_node_buf + 4, sib);
        if (hfs_write_node(v, old_fl, hfs_node_buf) < 0)
            return -1;
    }
    if (kind != HFS_BT_INDEX && v->cat_last_leaf == node)
        v->cat_last_leaf = sib;

    /* promote a copy of the sibling's first key, pointing at 'sib' */
    if (hfs_read_node(v, sib) < 0)
        return -1;
    const uint8_t *r0 = hfs_node_buf + bt_recoff(hfs_node_buf, ns, 0);
    uint16_t kl = load_be16(r0);
    memcpy(promo, r0, 2 + kl);
    put_be32(promo + 2 + kl, sib);
    *plen = 2 + kl + 4;
    *promoted = true;
    return 0;
}

/* insert a full catalog record (key + body) into the catalog B-tree */
static int hfs_cat_insert(struct hfs_volinfo *v, const uint8_t *fullrec,
                          uint16_t reclen)
{
    uint32_t parent = load_be32(fullrec + 2);
    int namelen = load_be16(fullrec + 6);
    if (namelen > HFS_KEY_NAME_MAX) namelen = HFS_KEY_NAME_MAX;
    for (int i = 0; i < namelen; i++)
        hfs_name16[i] = load_be16(fullrec + 8 + i * 2);

    struct hfs_bt_path path[HFS_MAX_BTREE_DEPTH];
    int npath; uint32_t leaf; int pos; bool found;
    if (hfs_cat_search(v, parent, hfs_name16, namelen, path, &npath,
                       &leaf, &pos, &found) < 0)
        return -1;
    if (found)
        return -1;  /* already exists */

    bool promoted; uint16_t plen;
    int rc = hfs_node_insert(v, leaf, pos, fullrec, reclen,
                             &promoted, hfs_promo_buf, &plen);
    if (rc < 0)
        return rc;
    v->cat_leaf_records++;

    for (int level = npath - 1; promoted && level >= 0; level--)
    {
        bool p2; uint16_t plen2;
        rc = hfs_node_insert(v, path[level].node, path[level].idx + 1,
                             hfs_promo_buf, plen, &p2, hfs_promo_buf2, &plen2);
        if (rc < 0)
            return rc;
        promoted = p2;
        if (promoted) { memcpy(hfs_promo_buf, hfs_promo_buf2, plen2); plen = plen2; }
    }

    if (promoted)
    {
        /* the root split: build a new root pointing at the old root and the
           promoted sibling */
        uint32_t newroot;
        if (hfs_bt_alloc_node(v, &newroot) < 0)
            return HFS_RC_ENOSPC;
        if (hfs_read_node(v, v->cat_root) < 0)
            return -1;
        uint16_t kl = load_be16(hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, 0));
        const uint8_t *r0 = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, 0);
        memcpy(hfs_promo_buf2, r0, 2 + kl);
        put_be32(hfs_promo_buf2 + 2 + kl, v->cat_root);

        memset(hfs_node_buf2, 0, v->node_size);
        hfs_node_buf2[8] = HFS_BT_INDEX;
        hfs_node_buf2[9] = (uint8_t)(v->cat_tree_depth + 1);
        put_be16(hfs_node_buf2 + 10, 0);
        bt_set_recoff(hfs_node_buf2, v->node_size, 0, 14);
        bt_insert_rec(hfs_node_buf2, v->node_size, 0, hfs_promo_buf2, 2 + kl + 4);
        bt_insert_rec(hfs_node_buf2, v->node_size, 1, hfs_promo_buf, plen);
        if (hfs_write_node(v, newroot, hfs_node_buf2) < 0)
            return -1;
        v->cat_root = newroot;
        v->cat_tree_depth++;
    }

    return hfs_cat_header_flush(v);
}

/* delete the catalog record with key (parent,name16/len) from the B-tree */
static int hfs_cat_delete(struct hfs_volinfo *v, uint32_t parent,
                          const uint16_t *name16, int namelen)
{
    struct hfs_bt_path path[HFS_MAX_BTREE_DEPTH];
    int npath; uint32_t leaf; int pos; bool found;
    if (hfs_cat_search(v, parent, name16, namelen, path, &npath,
                       &leaf, &pos, &found) < 0)
        return -1;
    if (!found)
        return -1;

    if (hfs_read_node(v, leaf) < 0)
        return -1;
    bt_delete_rec(hfs_node_buf, v->node_size, pos);
    v->cat_leaf_records--;
    if (bt_numrec(hfs_node_buf) > 0)
    {
        if (hfs_write_node(v, leaf, hfs_node_buf) < 0)
            return -1;
        return hfs_cat_header_flush(v);
    }

    /* leaf emptied: unlink it from the chain, free it, drop parent pointers */
    uint32_t child = leaf;
    int level = npath - 1;
    for (;;)
    {
        if (hfs_read_node(v, child) < 0)
            return -1;
        uint32_t fl = load_be32(hfs_node_buf);
        uint32_t bl = load_be32(hfs_node_buf + 4);
        bool is_leaf = ((int8_t)hfs_node_buf[8] == (int8_t)HFS_BT_LEAF);
        if (bl)
        {
            if (hfs_read_node(v, bl) < 0) return -1;
            put_be32(hfs_node_buf, fl);
            if (hfs_write_node(v, bl, hfs_node_buf) < 0) return -1;
        }
        if (fl)
        {
            if (hfs_read_node(v, fl) < 0) return -1;
            put_be32(hfs_node_buf + 4, bl);
            if (hfs_write_node(v, fl, hfs_node_buf) < 0) return -1;
        }
        if (is_leaf)
        {
            if (v->first_leaf == child)    v->first_leaf = fl;
            if (v->cat_last_leaf == child) v->cat_last_leaf = bl;
        }
        if (hfs_bt_free_node(v, child) < 0)
            return -1;

        if (level < 0)
            break;
        /* remove the index entry that pointed at 'child' from its parent */
        uint32_t idxnode = path[level].node;
        if (hfs_read_node(v, idxnode) < 0)
            return -1;
        int n = bt_numrec(hfs_node_buf), del = -1;
        for (int i = 0; i < n; i++)
        {
            const uint8_t *rec = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, i);
            uint16_t kl = load_be16(rec);
            if (load_be32(rec + 2 + kl) == child) { del = i; break; }
        }
        if (del < 0)
            break;
        bt_delete_rec(hfs_node_buf, v->node_size, del);
        if (bt_numrec(hfs_node_buf) > 0 || idxnode == v->cat_root)
        {
            if (hfs_write_node(v, idxnode, hfs_node_buf) < 0)
                return -1;
            break;
        }
        child = idxnode;
        level--;
    }

    return hfs_cat_header_flush(v);
}

/* find a file/folder record by its CNID via a linear leaf scan; leaves the
   containing node in hfs_node_buf and returns its node number and record idx */
static int hfs_cat_find_cnid(struct hfs_volinfo *v, uint32_t cnid,
                             uint32_t *node_out, int *idx_out)
{
    uint32_t node = v->first_leaf;
    while (node)
    {
        if (hfs_read_node(v, node) < 0)
            return -1;
        int n = bt_numrec(hfs_node_buf);
        for (int r = 0; r < n; r++)
        {
            const uint8_t *rec = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, r);
            uint16_t kl = load_be16(rec);
            const uint8_t *data = rec + 2 + kl;
            int16_t t = (int16_t)load_be16(data);
            if ((t == HFSP_FOLDER || t == HFSP_FILE) &&
                load_be32(data + 8) == cnid)
            {
                *node_out = node; *idx_out = r;
                return 0;
            }
        }
        node = load_be32(hfs_node_buf);
    }
    return -1;
}

/* read an object's thread record to learn its parent and name */
static int hfs_cat_read_thread(struct hfs_volinfo *v, uint32_t cnid,
                               uint32_t *parent_out, uint16_t *name16,
                               int *namelen_out)
{
    struct hfs_bt_path path[HFS_MAX_BTREE_DEPTH];
    int npath, pos; uint32_t leaf; bool found;
    if (hfs_cat_search(v, cnid, NULL, 0, path, &npath, &leaf, &pos, &found) < 0)
        return -1;
    if (!found)
        return -1;
    if (hfs_read_node(v, leaf) < 0)
        return -1;
    const uint8_t *rec = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, pos);
    uint16_t kl = load_be16(rec);
    const uint8_t *data = rec + 2 + kl;
    *parent_out = load_be32(data + 4);
    int nl = load_be16(data + 8);
    if (nl > HFS_KEY_NAME_MAX) nl = HFS_KEY_NAME_MAX;
    for (int i = 0; i < nl; i++)
        name16[i] = load_be16(data + 10 + i * 2);
    *namelen_out = nl;
    return 0;
}

/* write the data-fork (size, totalBlocks, inline extents) into a file record */
static int hfs_cat_set_fork(struct hfs_volinfo *v, struct hfs_file *f)
{
    uint32_t node; int idx;
    if (hfs_cat_find_cnid(v, f->cnid, &node, &idx) < 0)
        return -1;
    uint8_t *rec = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, idx);
    uint16_t kl = load_be16(rec);
    uint8_t *data = rec + 2 + kl;
    put_be64(data + 88, f->size);
    put_be32(data + 88 + 12, f->total_blocks);
    for (int i = 0; i < 8; i++)
    {
        put_be32(data + 88 + 16 + i * 8,     f->extents[i].start_block);
        put_be32(data + 88 + 16 + i * 8 + 4, f->extents[i].block_count);
    }
    return hfs_write_node(v, node, hfs_node_buf);
}

/* adjust a folder's valence (child count) and bump its modification date */
static int hfs_cat_adjust_valence(struct hfs_volinfo *v, uint32_t folder,
                                  int delta, uint32_t now)
{
    if (folder == 1) /* parent of the root has no on-disk record */
        return 0;
    uint32_t node; int idx;
    if (hfs_cat_find_cnid(v, folder, &node, &idx) < 0)
        return -1;
    uint8_t *rec = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, idx);
    uint16_t kl = load_be16(rec);
    uint8_t *data = rec + 2 + kl;
    if ((int16_t)load_be16(data) != HFSP_FOLDER)
        return -1;
    put_be32(data + 4, load_be32(data + 4) + (uint32_t)delta);
    put_be32(data + 16, now); /* contentModDate */
    return hfs_write_node(v, node, hfs_node_buf);
}

/* assemble a file/folder catalog record into hfs_rec_buf; returns total length */
static uint16_t hfs_build_node_record(uint32_t parent, const uint16_t *name16,
                                      int namelen, bool is_dir, uint32_t cnid,
                                      uint32_t now)
{
    uint8_t *r = hfs_rec_buf;
    uint16_t keylen = 6 + 2 * namelen;
    put_be16(r, keylen);
    put_be32(r + 2, parent);
    put_be16(r + 6, namelen);
    for (int i = 0; i < namelen; i++)
        put_be16(r + 8 + i * 2, name16[i]);

    uint8_t *b = r + 2 + keylen;
    int blen = is_dir ? 88 : 248;
    memset(b, 0, blen);
    put_be16(b + 0, is_dir ? HFSP_FOLDER : HFSP_FILE);
    put_be16(b + 2, 0x0002);            /* kHFSThreadExistsMask */
    put_be32(b + 8, cnid);              /* folderID / fileID */
    put_be32(b + 12, now);              /* createDate */
    put_be32(b + 16, now);              /* contentModDate */
    put_be32(b + 20, now);              /* attributeModDate */
    put_be32(b + 24, now);              /* accessDate */
    put_be16(b + 32 + 10, is_dir ? 0040755 : 0100644); /* BSD fileMode */
    return 2 + keylen + blen;
}

/* assemble a thread record into hfs_rec_buf; returns total length */
static uint16_t hfs_build_thread_record(uint32_t cnid, uint32_t parent,
                                        const uint16_t *name16, int namelen,
                                        bool is_dir)
{
    uint8_t *r = hfs_rec_buf;
    put_be16(r, 6);                     /* keyLength: parentID + empty name */
    put_be32(r + 2, cnid);
    put_be16(r + 6, 0);
    uint8_t *b = r + 8;
    put_be16(b + 0, is_dir ? HFSP_FOLDER_THREAD : HFSP_FILE_THREAD);
    put_be16(b + 2, 0);
    put_be32(b + 4, parent);
    put_be16(b + 8, namelen);
    for (int i = 0; i < namelen; i++)
        put_be16(b + 10 + i * 2, name16[i]);
    return 8 + 10 + 2 * namelen;
}

/** Public write entry points **/

void hfs_seek_to_stream(struct hfs_filestr *filestr,
                        const struct hfs_filestr *seekto)
{
    filestr->cur_sector = seekto->cur_sector;
    filestr->eof = seekto->eof;
}

static long hfs_write_data(struct hfs_filestr *filestr,
                           unsigned long sectorcount, void *buf)
{
    struct hfs_file *f = filestr->filep;
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(f)];
    if (!v->mounted)
        return -1;
    if (!v->writable)
        return HFS_RC_READONLY;
    if (sectorcount == 0)
        return 0;

    uint32_t spb = v->sectors_per_block;
    uint64_t end_sector = (uint64_t)filestr->cur_sector + sectorcount;
    uint32_t need_blocks = (uint32_t)((end_sector + spb - 1) / spb);
    int rc = hfs_grow(v, f, need_blocks);
    if (rc < 0)
        return rc;

    const uint8_t *in = buf;
    long done = 0;
    while (sectorcount > 0)
    {
        uint32_t fork_block   = filestr->cur_sector / spb;
        uint32_t sec_in_block = filestr->cur_sector % spb;
        uint32_t acc = 0, vblock = 0xFFFFFFFFu, run_blocks = 0;
        for (int i = 0; i < 8; i++)
        {
            if (f->extents[i].block_count == 0)
                break;
            if (fork_block < acc + f->extents[i].block_count)
            {
                uint32_t o = fork_block - acc;
                vblock = f->extents[i].start_block + o;
                run_blocks = f->extents[i].block_count - o;
                break;
            }
            acc += f->extents[i].block_count;
        }
        if (vblock == 0xFFFFFFFFu)
            return done > 0 ? done : -1;
        sector_t dev_sec = v->part_start + (sector_t)vblock * spb + sec_in_block;
        uint64_t run = (uint64_t)run_blocks * spb - sec_in_block;
        if (run > sectorcount)
            run = sectorcount;
        if (hfs_write_sectors(v, dev_sec, (int)run, in) < 0)
            return done > 0 ? done : -1;
        in += run * v->log_sector_size;
        filestr->cur_sector += run;
        sectorcount -= run;
        done += run;
    }
    return done;
}

int hfs_closewrite(struct hfs_filestr *filestr, uint32_t size,
                   struct fat_direntry *fatentp)
{
    struct hfs_file *f = filestr->filep;
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(f)];
    (void)fatentp;
    if (!v->mounted)
        return -1;
    if (!v->writable)
        return HFS_RC_READONLY;

    f->size = size;
    uint32_t need = (uint32_t)((size + v->block_size - 1) / v->block_size);
    if (need < f->total_blocks)
        hfs_shrink(v, f, need);
    if (hfs_cat_set_fork(v, f) < 0)
        return -1;
    return hfs_vh_flush(v);
}

int hfs_truncate(struct hfs_filestr *filestr)
{
    struct hfs_file *f = filestr->filep;
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(f)];
    if (!v->mounted)
        return -1;
    if (!v->writable)
        return HFS_RC_READONLY;

    uint32_t spb = v->sectors_per_block;
    uint32_t keep = (uint32_t)((filestr->cur_sector + spb - 1) / spb);
    hfs_shrink(v, f, keep);
    if (hfs_cat_set_fork(v, f) < 0)
        return -1;
    return hfs_vh_flush(v);
}

int hfs_create(struct hfs_file *parent, const char *name, unsigned int attr,
               struct hfs_file *file, struct fat_direntry *fatentp)
{
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(parent)];
    if (!v->mounted)
        return -1;
    if (!v->writable)
        return HFS_RC_READONLY;

    bool is_dir = (attr & ATTR_DIRECTORY) != 0;
    uint32_t pcnid = parent->cnid;
    uint32_t cnid  = v->next_cnid;
    uint32_t now   = hfs_now();
    int namelen = hfs_str_to_utf16(name, hfs_name16, HFS_KEY_NAME_MAX);

    uint16_t rl = hfs_build_node_record(pcnid, hfs_name16, namelen, is_dir,
                                        cnid, now);
    int rc = hfs_cat_insert(v, hfs_rec_buf, rl);
    if (rc < 0)
        return rc;

    rl = hfs_build_thread_record(cnid, pcnid, hfs_name16, namelen, is_dir);
    rc = hfs_cat_insert(v, hfs_rec_buf, rl);
    if (rc < 0)
        return rc;

    hfs_cat_adjust_valence(v, pcnid, +1, now);

    v->next_cnid = cnid + 1;
    if (is_dir) v->folder_count++; else v->file_count++;
    if (hfs_vh_flush(v) < 0)
        return -1;

    memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
    file->volume = parent->volume;
#endif
    file->cnid = cnid;
    file->parent = pcnid;
    file->is_dir = is_dir ? 1 : 0;
    file->size = 0;
    file->total_blocks = 0;
    file->e.node = HFSP_SCAN_REWOUND;

    if (fatentp)
    {
        memset(fatentp, 0, sizeof(*fatentp));
        fatentp->attr = (uint8_t)attr;
        fatentp->firstcluster = (int32_t)cnid;
    }
    return 0;
}

int hfs_remove(struct hfs_file *file, int what)
{
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(file)];
    if (!v->mounted)
        return -1;
    if (!v->writable)
        return HFS_RC_READONLY;

    if (what & FAT_RM_DIRENTRIES)
    {
        uint32_t parent; int namelen;
        if (hfs_cat_read_thread(v, file->cnid, &parent, hfs_name16, &namelen) < 0)
            return -1;
        if (hfs_cat_delete(v, parent, hfs_name16, namelen) < 0) /* file record */
            return -1;
        if (hfs_cat_delete(v, file->cnid, NULL, 0) < 0)         /* thread */
            return -1;
        hfs_cat_adjust_valence(v, parent,
                               -1, hfs_now());
        if (file->is_dir) v->folder_count--; else v->file_count--;
        if (hfs_vh_flush(v) < 0)
            return -1;
    }

    if (what & FAT_RM_DATA)
    {
        for (int i = 0; i < 8; i++)
        {
            if (file->extents[i].block_count)
                hfs_alloc_free(v, file->extents[i].start_block,
                               file->extents[i].block_count);
            file->extents[i].start_block = 0;
            file->extents[i].block_count = 0;
        }
        file->total_blocks = 0;
        file->size = 0;
        if (hfs_vh_flush(v) < 0)
            return -1;
    }
    return 0;
}

int hfs_rename(struct hfs_file *parent, struct hfs_file *file,
               const unsigned char *newname)
{
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(file)];
    if (!v->mounted)
        return -1;
    if (!v->writable)
        return HFS_RC_READONLY;

    uint32_t newparent = parent->cnid;
    uint32_t now = hfs_now();

    /* current location/name from the thread record */
    uint32_t oldparent; int oldlen;
    static uint16_t oldname[HFS_KEY_NAME_MAX];
    if (hfs_cat_read_thread(v, file->cnid, &oldparent, oldname, &oldlen) < 0)
        return -1;

    /* save the existing file/folder record body */
    uint32_t node; int idx;
    if (hfs_cat_find_cnid(v, file->cnid, &node, &idx) < 0)
        return -1;
    uint8_t *rec = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, idx);
    uint16_t kl = load_be16(rec);
    uint16_t reclen = bt_recoff(hfs_node_buf, v->node_size, idx + 1)
                      - bt_recoff(hfs_node_buf, v->node_size, idx);
    uint16_t bodylen = reclen - (2 + kl);
    memcpy(hfs_body_buf, rec + 2 + kl, bodylen);
    bool is_dir = ((int16_t)load_be16(hfs_body_buf) == HFSP_FOLDER);

    int newlen = hfs_str_to_utf16((const char *)newname, hfs_name16,
                                  HFS_KEY_NAME_MAX);

    /* remove the old file/folder record, insert the new-keyed one */
    if (hfs_cat_delete(v, oldparent, oldname, oldlen) < 0)
        return -1;
    uint16_t newkeylen = 6 + 2 * newlen;
    put_be16(hfs_rec_buf, newkeylen);
    put_be32(hfs_rec_buf + 2, newparent);
    put_be16(hfs_rec_buf + 6, newlen);
    for (int i = 0; i < newlen; i++)
        put_be16(hfs_rec_buf + 8 + i * 2, hfs_name16[i]);
    memcpy(hfs_rec_buf + 2 + newkeylen, hfs_body_buf, bodylen);
    if (hfs_cat_insert(v, hfs_rec_buf, 2 + newkeylen + bodylen) < 0)
        return -1;

    /* rebuild the thread record (same key, new parent/name) */
    if (hfs_cat_delete(v, file->cnid, NULL, 0) < 0)
        return -1;
    uint16_t tl = hfs_build_thread_record(file->cnid, newparent, hfs_name16,
                                          newlen, is_dir);
    if (hfs_cat_insert(v, hfs_rec_buf, tl) < 0)
        return -1;

    if (oldparent != newparent)
    {
        hfs_cat_adjust_valence(v, oldparent, -1, now);
        hfs_cat_adjust_valence(v, newparent, +1, now);
    }
    file->parent = newparent;
    return hfs_vh_flush(v);
}

int hfs_modtime(struct hfs_file *parent, struct hfs_file *file, time_t modtime)
{
    struct hfs_volinfo *v = &hfs_vols[hfs_volidx(file)];
    (void)parent;
    if (!v->mounted)
        return -1;
    if (!v->writable)
        return HFS_RC_READONLY;

    uint32_t node; int idx;
    if (hfs_cat_find_cnid(v, file->cnid, &node, &idx) < 0)
        return -1;
    uint8_t *rec = hfs_node_buf + bt_recoff(hfs_node_buf, v->node_size, idx);
    uint16_t kl = load_be16(rec);
    uint8_t *data = rec + 2 + kl;
    uint32_t mac = (uint32_t)modtime + HFS_MAC_EPOCH_OFFSET;
    put_be32(data + 16, mac); /* contentModDate */
    put_be32(data + 20, mac); /* attributeModDate */
    if (hfs_write_node(v, node, hfs_node_buf) < 0)
        return -1;
    return hfs_vh_flush(v);
}

#else /* BOOTLOADER: the boot loader only reads, so provide refusing stubs to
         satisfy the VFS write wrappers without pulling in the writer. */

void hfs_seek_to_stream(struct hfs_filestr *filestr,
                        const struct hfs_filestr *seekto)
    { (void)filestr; (void)seekto; }
int hfs_truncate(struct hfs_filestr *filestr)
    { (void)filestr; return HFS_RC_READONLY; }
int hfs_closewrite(struct hfs_filestr *filestr, uint32_t size,
                   struct fat_direntry *fatentp)
    { (void)filestr; (void)size; (void)fatentp; return 0; }
int hfs_create(struct hfs_file *parent, const char *name, unsigned int attr,
               struct hfs_file *file, struct fat_direntry *fatentp)
    { (void)parent; (void)name; (void)attr; (void)file; (void)fatentp;
      return HFS_RC_READONLY; }
int hfs_remove(struct hfs_file *file, int what)
    { (void)file; (void)what; return HFS_RC_READONLY; }
int hfs_rename(struct hfs_file *parent, struct hfs_file *file,
               const unsigned char *newname)
    { (void)parent; (void)file; (void)newname; return HFS_RC_READONLY; }
int hfs_modtime(struct hfs_file *parent, struct hfs_file *file, time_t modtime)
    { (void)parent; (void)file; (void)modtime; return HFS_RC_READONLY; }

#endif /* !BOOTLOADER */

#if defined(MAX_VARIABLE_LOG_SECTOR)
int hfs_file_sector_size(IF_MV_NONVOID(const struct hfs_file *file))
{
#ifdef HAVE_MULTIVOLUME
    return hfs_vols[file->volume].log_sector_size;
#else
    return hfs_vols[0].log_sector_size;
#endif
}
#endif
