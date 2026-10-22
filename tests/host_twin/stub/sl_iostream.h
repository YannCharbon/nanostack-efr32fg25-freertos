#ifndef HOST_SL_IOSTREAM_H
#define HOST_SL_IOSTREAM_H
#include <stddef.h>
#include "sl_status.h"   /* real SDK header (platform/common/inc) */
typedef struct { int fd; } sl_iostream_t;
sl_status_t sl_iostream_set_default(sl_iostream_t *s);
sl_status_t sl_iostream_write(sl_iostream_t *s, const void *buf, size_t len);
sl_status_t sl_iostream_read(sl_iostream_t *s, void *buf, size_t len, size_t *got);
#endif
