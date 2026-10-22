#ifndef HOST_SL_SE_MANAGER_H
#define HOST_SL_SE_MANAGER_H
#include <stdint.h>
#include "sl_status.h"   /* real SDK header (platform/common/inc) */
typedef struct { int dummy; } sl_se_command_context_t;
sl_status_t sl_se_init_command_context(sl_se_command_context_t *c);
#endif
