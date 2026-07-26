#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ring_buffer.h"

// Free-running sampler. Runs on core 0.
//
// Once started it keeps issuing bare 0xB7 reads at its own pace and pushes each
// result as MSG_SAMPLE, rather than waiting to be asked for every sample. That
// is what decouples the sample rate from host latency - the RPi 5 just consumes
// what arrives. The sample traffic also keeps the KWP session alive by itself,
// so the heartbeat only fires when sampling is slower than the heartbeat
// interval (or stopped).

typedef enum {
    LOGGER_TICK_IDLE,             // not streaming, or not due yet
    LOGGER_TICK_SAMPLED,          // a K-line transaction happened
    LOGGER_TICK_NEEDS_RECONNECT,  // handler is gone and reinstalling did not fix it
} LoggerTick;

void logger_init(RingBuffer *to_host);

// interval_ms 0 means "as fast as the K-line allows".
void logger_start(uint32_t interval_ms);
void logger_stop(const char *reason);
bool logger_is_streaming(void);

// Call from the core 0 loop. `connected` is the link state; the caller owns
// reconnection, which is why losing the handler is reported back rather than
// handled here.
LoggerTick logger_update(bool connected);
