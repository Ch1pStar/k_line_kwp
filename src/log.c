#include "log.h"
#include "messages.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static RingBuffer *log_sink = NULL;
static uint32_t dropped_total = 0;
static uint32_t dropped_pending = 0;

void klog_init(RingBuffer *sink) {
    log_sink = sink;
}

uint32_t klog_dropped_count(void) {
    return dropped_total;
}

// Returns false when the queue is full - the caller counts the loss rather
// than waiting, see the note in log.h.
static bool push_line(const char *text, size_t len) {
    if (log_sink == NULL) return false;
    if (len > MAX_MESSAGE_SIZE) len = MAX_MESSAGE_SIZE;

    BufferMessage msg = {
        .messageType = MSG_LOG,
        .length = (uint8_t)len,
    };
    memcpy(msg.data, text, len);

    return ringbuffer_push(log_sink, &msg);
}

void klog(const char *fmt, ...) {
    char line[MAX_MESSAGE_SIZE];

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (written < 0) return;

    size_t len = (size_t)written;
    if (len >= sizeof(line)) len = sizeof(line) - 1;  // truncated by vsnprintf

    // Own up to any gap in the output before printing the next line, so a burst
    // that overran the queue is visible instead of silently missing.
    if (dropped_pending > 0) {
        char note[48];
        int n = snprintf(note, sizeof(note), "[log] %u line(s) dropped",
                         (unsigned)dropped_pending);
        if (n > 0 && push_line(note, (size_t)n)) {
            dropped_pending = 0;
        }
    }

    if (!push_line(line, len)) {
        dropped_total++;
        dropped_pending++;
    }
}
