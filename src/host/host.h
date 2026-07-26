#pragma once

#include "ring_buffer.h"

// Core 1: owns both frontends and the single drain of core 0's message queue.
//
// The queue is single-producer/single-consumer, so exactly one place may pop
// it. That place is here, and each message is then handed to every frontend -
// the console prints it, the host link frames it for the RPi 5.

void host_init(RingBuffer *to_ecu, RingBuffer *from_ecu);
void host_update(void);
