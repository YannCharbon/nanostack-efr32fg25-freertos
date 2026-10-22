/*
 * Compatibility header: the stack includes "common/utils.h" (ws_regulation.c)
 * for GNU min/max (D3). GCC/Clang only.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_COMMON_UTILS_H
#define NS_PORT_COMMON_UTILS_H

#include <stdint.h>
#include <stddef.h>

#ifndef min
#define min(x, y) ({ __typeof__(x) _x = (x); __typeof__(y) _y = (y); _x < _y ? _x : _y; })
#endif
#ifndef max
#define max(x, y) ({ __typeof__(x) _x = (x); __typeof__(y) _y = (y); _x > _y ? _x : _y; })
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((uintptr_t)(ptr) - offsetof(type, member)))
#endif

#endif
