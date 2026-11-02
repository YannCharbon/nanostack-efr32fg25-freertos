/*
 * A small file store in internal flash for the stack's ns_file_system API
 * (FW_FLASH_FS): the Wi-SUN security keys, the frame counters and the network
 * information (ws_pae_nvm_store.c, ws_pae_key_storage.c) survive a reset, so
 * a node rejoins without a full EAP-TLS authentication.
 *
 * The stack writes and reads whole files (open "w", one or more writes,
 * close; open "r", read, close). Layout: two flash pages at the end of the
 * main flash, used in turn as an append-only log of records
 *     [magic 4][name 24][length 2][flags 2][data, padded to 4 bytes]
 * A file's content is its latest record; a removal is a record with the
 * deleted flag. When the active page is full, the live records are copied
 * to the other page, which then becomes the active one, and the old page is
 * erased. Writes are buffered in RAM until close. Not power-fail safe in the
 * middle of a compaction (the newer page wins at start-up only if complete;
 * test use).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "em_device.h"
#include "em_msc.h"
#include "nanostack/ns_file_system.h"
#include "mbed-client-libservice/ns_trace.h"

#define TRACE_GROUP "ffs"

#define FFS_PAGES       2
#define FFS_PAGE_SIZE   FLASH_PAGE_SIZE
#define FFS_BASE        (FLASH_BASE + FLASH_SIZE - FFS_PAGES * FFS_PAGE_SIZE)
#define FFS_MAGIC       0x53464e57u     /* "WNFS" */
#define FFS_NAME_LEN    24
#define FFS_FLAG_DELETED 0x0001
#define FFS_HDR_LEN     (4 + FFS_NAME_LEN + 4)
#define FFS_MAX_FILE    2048

typedef struct {
    uint32_t magic;
    char name[FFS_NAME_LEN];
    uint16_t len;
    uint16_t flags;
} ffs_hdr_t;

typedef struct {
    char name[FFS_NAME_LEN];
    bool write;
    uint16_t len;
    uint16_t pos;
    uint8_t *data;
} ffs_file_t;

static uint8_t active;      // active page
static uint32_t wr_off;     // first free byte in the active page

static uint32_t page_addr(uint8_t p)
{
    return FFS_BASE + (uint32_t)p * FFS_PAGE_SIZE;
}

static uint32_t rec_size(uint16_t len)
{
    return FFS_HDR_LEN + ((len + 3u) & ~3u);
}

/* The end of the records of a page, or 0 if it holds none */
static uint32_t page_end(uint8_t p)
{
    uint32_t off = 0;
    while (off + FFS_HDR_LEN <= FFS_PAGE_SIZE) {
        const ffs_hdr_t *h = (const ffs_hdr_t *)(page_addr(p) + off);
        if (h->magic != FFS_MAGIC || h->len > FFS_MAX_FILE || off + rec_size(h->len) > FFS_PAGE_SIZE) {
            break;
        }
        off += rec_size(h->len);
    }
    return off;
}

/* The latest record of a file in a page, or NULL */
static const ffs_hdr_t *find(uint8_t p, uint32_t end, const char *name)
{
    const ffs_hdr_t *found = NULL;
    for (uint32_t off = 0; off < end;) {
        const ffs_hdr_t *h = (const ffs_hdr_t *)(page_addr(p) + off);
        if (!strncmp(h->name, name, FFS_NAME_LEN)) {
            found = h;
        }
        off += rec_size(h->len);
    }
    return (found && !(found->flags & FFS_FLAG_DELETED)) ? found : NULL;
}

static int program(uint32_t addr, const void *data, uint32_t len)
{
    return MSC_WriteWord((uint32_t *)addr, data, len) == mscReturnOk ? 0 : -1;
}

static int append(const char *name, const uint8_t *data, uint16_t len, uint16_t flags);

/* Copy the live records to the other page, which becomes the active one */
static int compact(void)
{
    uint8_t from = active, to = active ^ 1;
    uint32_t end = wr_off;

    if (MSC_ErasePage((uint32_t *)page_addr(to)) != mscReturnOk) {
        return -1;
    }
    active = to;
    wr_off = 0;
    for (uint32_t off = 0; off < end;) {
        const ffs_hdr_t *h = (const ffs_hdr_t *)(page_addr(from) + off);
        off += rec_size(h->len);
        if (find(from, end, h->name) == h) {
            if (append(h->name, (const uint8_t *)(h + 1), h->len, 0) < 0) {
                return -1;
            }
        }
    }
    return MSC_ErasePage((uint32_t *)page_addr(from)) == mscReturnOk ? 0 : -1;
}

static int append(const char *name, const uint8_t *data, uint16_t len, uint16_t flags)
{
    uint8_t pad[4] = { 0xff, 0xff, 0xff, 0xff };
    ffs_hdr_t h;

    if (len > FFS_MAX_FILE) {
        return -1;
    }
    if (wr_off + rec_size(len) > FFS_PAGE_SIZE) {
        if (compact() < 0 || wr_off + rec_size(len) > FFS_PAGE_SIZE) {
            tr_error("flash file store full");
            return -1;
        }
    }
    memset(&h, 0, sizeof(h));
    h.magic = FFS_MAGIC;
    strncpy(h.name, name, FFS_NAME_LEN - 1);
    h.len = len;
    h.flags = flags;
    uint32_t a = page_addr(active) + wr_off;
    uint32_t body = len & ~3u;
    if (program(a + 4, h.name, FFS_HDR_LEN - 4) < 0 ||
            (body && program(a + FFS_HDR_LEN, data, body) < 0)) {
        return -1;
    }
    if (len & 3u) {
        memcpy(pad, data + body, len & 3u);
        if (program(a + FFS_HDR_LEN + body, pad, 4) < 0) {
            return -1;
        }
    }
    // The magic last: a record interrupted before it is ignored
    if (program(a, &h.magic, 4) < 0) {
        return -1;
    }
    wr_off += rec_size(len);
    return 0;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static NS_FILE ffs_open(const char *filename, const char *mode)
{
    ffs_file_t *f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    strncpy(f->name, base_name(filename), FFS_NAME_LEN - 1);
    f->write = (*mode == 'w');
    if (f->write) {
        f->data = malloc(FFS_MAX_FILE);
        if (!f->data) {
            free(f);
            return NULL;
        }
        return f;
    }
    const ffs_hdr_t *h = find(active, wr_off, f->name);
    if (!h) {
        free(f);
        return NULL;
    }
    f->data = (uint8_t *)(h + 1);   // read in place
    f->len = h->len;
    return f;
}

static int ffs_close(NS_FILE *handle)
{
    ffs_file_t *f = (ffs_file_t *)handle;
    int ret = 0;

    if (f->write) {
        const ffs_hdr_t *h = find(active, wr_off, f->name);
        // Unchanged content: no flash write
        if (!h || h->len != f->len || memcmp(h + 1, f->data, f->len)) {
            ret = append(f->name, f->data, f->len, 0);
        }
        free(f->data);
    }
    free(f);
    return ret;
}

static int ffs_remove(const char *filename)
{
    const char *name = base_name(filename);
    if (!find(active, wr_off, name)) {
        return -1;
    }
    return append(name, NULL, 0, FFS_FLAG_DELETED);
}

static size_t ffs_write(NS_FILE *handle, const void *buffer, size_t size)
{
    ffs_file_t *f = (ffs_file_t *)handle;
    if (!f->write || f->len + size > FFS_MAX_FILE) {
        return 0;
    }
    memcpy(f->data + f->len, buffer, size);
    f->len += size;
    return size;
}

static size_t ffs_read(NS_FILE *handle, void *buffer, size_t size)
{
    ffs_file_t *f = (ffs_file_t *)handle;
    if (f->write) {
        return 0;
    }
    if (size > (size_t)(f->len - f->pos)) {
        size = f->len - f->pos;
    }
    memcpy(buffer, f->data + f->pos, size);
    f->pos += size;
    return size;
}

static int ffs_size(NS_FILE *handle, size_t *size)
{
    *size = ((ffs_file_t *)handle)->len;
    return 0;
}

int flash_fs_init(bool erase)
{
    MSC_Init();
    uint32_t e0 = page_end(0), e1 = page_end(1);
    if (erase) {
        MSC_ErasePage((uint32_t *)page_addr(0));
        MSC_ErasePage((uint32_t *)page_addr(1));
        e0 = e1 = 0;
    }
    // The page with records is active; with both (a compaction cut short),
    // the one being compacted to (fewer bytes) is incomplete: erase it
    if (e0 && e1) {
        uint8_t drop = e0 < e1 ? 0 : 1;
        MSC_ErasePage((uint32_t *)page_addr(drop));
        active = drop ^ 1;
    } else {
        active = e1 ? 1 : 0;
    }
    wr_off = active ? e1 : e0;
    // The rest of each page must be erased (another program, e.g. NVM3 of
    // Silicon Labs' samples, may have used these pages)
    for (uint8_t p = 0; p < FFS_PAGES; p++) {
        uint32_t from = (p == active) ? wr_off : 0;
        for (uint32_t off = from; off < FFS_PAGE_SIZE; off += 4) {
            if (*(const uint32_t *)(page_addr(p) + off) != 0xffffffffu) {
                if (p == active && wr_off) {
                    compact();          // keep the records, on a clean page
                } else {
                    MSC_ErasePage((uint32_t *)page_addr(p));
                    if (p == active) {
                        wr_off = 0;
                    }
                }
                break;
            }
        }
    }
    ns_file_system_callbacks_set(ffs_open, ffs_close, ffs_remove, ffs_write, ffs_read, ffs_size);
    ns_file_system_set_root_path("/flash/");
    tr_info("flash file store: page %u, %lu bytes used", active, (unsigned long)wr_off);
    return 0;
}
