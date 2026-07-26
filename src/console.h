#pragma once

#include "ring_buffer.h"

// USB serial text console: parses typed commands into Command values, hands
// them to the command layer, and prints whatever core 0 sends back. It is one
// frontend among the (eventually) two - the RPi 5 host link is the other - and
// holds no ECU knowledge of its own.
typedef struct {
    RingBuffer *rx_buffer;  // messages from core 0
    RingBuffer *tx_buffer;  // commands to core 0
} Console;

void console_init(Console *console, RingBuffer *tx, RingBuffer *rx);
void console_update(Console *console);
