/* Host twin internals shared by the fake layers. SPDX-License-Identifier: Apache-2.0 */
#ifndef HOST_TWIN_H
#define HOST_TWIN_H
#include <stdint.h>
void host_rtos_init(void);
void host_isr_enter(void);
void host_isr_exit(void);
void host_kernel_lock(void);
void host_kernel_unlock(void);
void host_rail_start(void);
extern int host_node_index;
extern int host_node_count;
extern const char *host_medium_dir;
#endif
