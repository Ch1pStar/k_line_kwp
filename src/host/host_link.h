#pragma once

#include "ring_buffer.h"

// Binary link to the RPi 5: over uart0 (GP0 TX, GP1 RX) at 921600 baud, or
// over the USB CDC port that also carries the text console.
//
// The second frontend: it speaks the COBS+CRC framing in proto.h instead of
// text, but goes through the same command layer as the console. GP0/GP1 are the
// SDK's default UART pins and are otherwise unused - the K-line is on GP15/GP18
// via PIO, and the USB console uses the RP2040's dedicated USB peripheral, not
// a UART.
//
// USB mode exists because during development the Pico is on the Pi's USB and
// nothing is wired to GP0/GP1. Only one transport is live at a time: text and
// frames on the same pipe would interleave, so entering USB mode mutes the
// console. See console.c for how the mode is entered and left.

#include <stdbool.h>

void host_link_init(void);

// Drain the receive FIFO, decoding and executing any complete commands.
void host_link_poll(void);

// Frame one message from core 0 and write it to the link.
void host_link_on_message(const BufferMessage *msg);

// Route frames over USB instead of uart0. The console falls silent while this
// is set, since it shares the pipe.
void host_link_set_usb(bool on);
bool host_link_usb_active(void);
