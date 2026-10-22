#ifndef HOST_SL_IOSTREAM_UART_H
#define HOST_SL_IOSTREAM_UART_H
#include <stdbool.h>
#include "sl_iostream.h"
typedef struct { sl_iostream_t stream; } sl_iostream_uart_t;
void sl_iostream_uart_set_read_block(sl_iostream_uart_t *u, bool on);
#endif
