/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Read-only Apple HFS+ / HFSX filesystem driver (for Mac-formatted iPods).
 *
 * Only the operations the Rockbox VFS needs for browsing and reading are
 * implemented: mounting, opening the root directory, reading directory
 * entries, opening a file/dir by its catalog node id (CNID) and reading file
 * data. All writing returns HFS_RC_READONLY.
 *
 * The catalog is read by walking the B-tree leaf nodes linearly (the leaves
 * are chained and sorted by parent CNID), so no key comparison or HFS+ Unicode
 * case-folding is needed: directory listing filters by parent CNID and opening
 * matches the object's own CNID. Path resolution is handled by the generic VFS
 * (readdir + strcasecmp), exactly as for FAT.
 *
 * Limitations (phase 1): data fork only, first eight (inline) extents only
 * (files fragmented into the extents-overflow B-tree are not fully readable),
 * and a journaled volume is read as-is (replay is not performed).
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

    v->block_size   = load_be32(vh + 40);
    v->total_blocks = load_be32(vh + 44);

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
    v->node_size  = load_be16(hfs_node_buf + 14 + 18);
    v->first_leaf = load_be32(hfs_node_buf + 14 + 10);

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
        *free = 0; /* unknown / not tracked for read-only */
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

long hfs_readwrite(struct hfs_filestr *filestr, unsigned long sectorcount,
                   void *buf, bool write)
{
    if (write)
        return HFS_RC_READONLY;

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
