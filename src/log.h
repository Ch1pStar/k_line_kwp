#pragma once

#include <stdint.h>
#include "ring_buffer.h"

// Logging for core 0.
//
// Core 0 must never call printf: stdio_usb is not multicore-safe, and a USB
// host that stops reading can make it block for milliseconds - straight through
// the middle of a K-line transaction, whose byte timing has no slack. klog
// instead queues the line for core 1 to print, and drops it if the queue is
// full, so the ECU path never waits on the console.
//
// Only core 0 may call klog: the sink is a single-producer queue.

// Point klog at the core0 -> core1 ring buffer. Call before launching core 1.
void klog_init(RingBuffer *sink);

// Queue one line (no trailing newline needed; core 1 adds it).
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Lines dropped because the queue was full, since boot.
uint32_t klog_dropped_count(void);
