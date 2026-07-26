#pragma once

#include "ring_buffer.h"

// USB serial text console: parses typed commands into Command values, hands
// them to the command layer, and prints whatever core 0 sends back. It is one
// frontend of two - the RPi 5 host link is the other - and holds no ECU
// knowledge of its own.

void console_init(void);

// Poll USB for typed input; executes a command once a line is complete.
void console_poll_input(void);

// Print one message from core 0. The drain loop lives in host.c, because the
// message queue has exactly one consumer by design.
void console_on_message(const BufferMessage *msg);
