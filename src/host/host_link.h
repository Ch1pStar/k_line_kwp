#pragma once

#include "ring_buffer.h"

// Binary link to the RPi 5 over uart0 (GP0 TX, GP1 RX) at 921600 baud.
//
// The second frontend: it speaks the COBS+CRC framing in proto.h instead of
// text, but goes through the same command layer as the console. GP0/GP1 are the
// SDK's default UART pins and are otherwise unused - the K-line is on GP15/GP18
// via PIO, and the USB console uses the RP2040's dedicated USB peripheral, not
// a UART.

void host_link_init(void);

// Drain the UART receive FIFO, decoding and executing any complete commands.
void host_link_poll(void);

// Frame one message from core 0 and write it to the link.
void host_link_on_message(const BufferMessage *msg);
