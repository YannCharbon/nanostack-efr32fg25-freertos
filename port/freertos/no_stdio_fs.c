/*
 * No file system on target (GENERICITY_PLAN.md D9/D10, "PoC step 1").
 *
 * ns_file_system.c falls back to stdio fopen()/fread()/... when no
 * ns_file_system_callbacks_set() callbacks are installed. The firmware never
 * sets a root path (ns_file_system_set_root_path() stays NULL), so storage of
 * PAE keys / frame counters / BBR info is disabled and these paths are never
 * taken at run time. To keep newlib's FILE machinery (and its _open/_unlink
 * stubs) out of the image, the link wraps the few stdio calls the stack makes
 * (-Wl,--wrap=fopen,...) onto these failing stubs.
 *
 * Step 2 (TODO): NVM3-backed ns_file_* callbacks, one NVM3 object per file
 * name hash, chunked above the NVM3 object size limit.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stddef.h>
#include <errno.h>

FILE *__wrap_fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

int __wrap_fclose(FILE *f)
{
    (void)f;
    return EOF;
}

size_t __wrap_fread(void *ptr, size_t size, size_t n, FILE *f)
{
    (void)ptr;
    (void)size;
    (void)n;
    (void)f;
    return 0;
}

size_t __wrap_fwrite(const void *ptr, size_t size, size_t n, FILE *f)
{
    (void)ptr;
    (void)size;
    (void)n;
    (void)f;
    return 0;
}

int __wrap_fseek(FILE *f, long off, int whence)
{
    (void)f;
    (void)off;
    (void)whence;
    return -1;
}

long __wrap_ftell(FILE *f)
{
    (void)f;
    return -1;
}

int __wrap_remove(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}
