#pragma once

#include <stdint.h>

// Initialize K-Line connection: 5-baud wakeup, sync, key bytes, complement exchange.
// Returns the address byte on success (0xee = programming mode, 0xcc = KWP2000),
// or UART_TIMEOUT on failure.
uint32_t kline_init_connection(void);
