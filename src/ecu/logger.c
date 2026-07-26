#include "logger.h"
#include "me7_handler.h"
#include "kwp2000.h"
#include "messages.h"
#include "log.h"

#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"

// A handler that has lost its redirect answers 0xB7 with SNS. That is the
// signature of "the ECU rebooted or the table went away", not of a bad request.
#define NRC_SERVICE_NOT_SUPPORTED 0x11

// Reinstalls are bounded: if the handler will not come back, say so and stop
// rather than hammering the ECU forever.
#define MAX_RECOVERY_ATTEMPTS 3

// Consecutive dead samples before treating it as a lost handler rather than a
// one-off glitch.
#define FAILURES_BEFORE_RECOVERY 3

#define STATS_EVERY_SAMPLES 100

static RingBuffer *host_queue = NULL;

static bool streaming = false;
static uint32_t interval_ms = 0;
static uint32_t last_sample_ms = 0;
static uint16_t sequence = 0;

static uint32_t consecutive_failures = 0;
static uint32_t recovery_attempts = 0;

static uint32_t stats_samples = 0;
static uint32_t stats_started_ms = 0;

void logger_init(RingBuffer *to_host) {
    host_queue = to_host;
}

bool logger_is_streaming(void) {
    return streaming;
}

void logger_start(uint32_t interval) {
    interval_ms = interval;
    streaming = true;
    consecutive_failures = 0;
    recovery_attempts = 0;
    stats_samples = 0;
    stats_started_ms = to_ms_since_boot(get_absolute_time());
    last_sample_ms = 0;  // sample immediately

    if (interval_ms == 0) {
        klog("logger: streaming at full rate (%u byte sample)",
             (unsigned)me7_handler_sample_size());
    } else {
        klog("logger: streaming every %u ms (%u byte sample)",
             (unsigned)interval_ms, (unsigned)me7_handler_sample_size());
    }
}

void logger_stop(const char *reason) {
    if (!streaming) return;
    streaming = false;

    const uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - stats_started_ms;
    const uint32_t rate_x10 = elapsed ? (stats_samples * 10000u) / elapsed : 0;
    klog("logger: stopped (%s) - %u samples in %u ms, %u.%u/s",
         reason, (unsigned)stats_samples, (unsigned)elapsed,
         (unsigned)(rate_x10 / 10), (unsigned)(rate_x10 % 10));
}

// MSG_SAMPLE payload: [seq:2 BE][timestamp_ms:4 BE][raw variable bytes]. The
// values are passed through exactly as the ECU packed them - scaling is the
// host's job, and the firmware has no .ecu file to do it with anyway.
static void push_sample(const KWP2000Response *response, uint32_t now) {
    BufferMessage msg = { .messageType = MSG_SAMPLE };

    msg.data[0] = (sequence >> 8) & 0xFF;
    msg.data[1] = sequence & 0xFF;
    msg.data[2] = (now >> 24) & 0xFF;
    msg.data[3] = (now >> 16) & 0xFF;
    msg.data[4] = (now >> 8) & 0xFF;
    msg.data[5] = now & 0xFF;

    size_t size = response->dataSize;
    if (size > MAX_MESSAGE_SIZE - 6) size = MAX_MESSAGE_SIZE - 6;
    memcpy(&msg.data[6], response->data, size);
    msg.length = (uint8_t)(6 + size);

    // Dropping a sample when the host is behind is correct: samples are
    // perishable, and blocking core 0 to preserve a stale one would cost the
    // next one too.
    ringbuffer_push(host_queue, &msg);
    sequence++;
}

static bool handler_is_gone(ResponseStatus status, const KWP2000Response *response) {
    return status == RESPONSE_NEGATIVE &&
           response->dataSize >= 2 &&
           response->data[0] == 0xB7 &&
           response->data[1] == NRC_SERVICE_NOT_SUPPORTED;
}

// Try to put the handler back. Safe in exactly this situation: 0xB7 answering
// SNS means the redirect is gone, so the write chunks travel through the ECU's
// own dispatcher. Reinstalling while the redirect is still live is what breaks
// 0x23/0x3D - see CLAUDE.md.
static bool attempt_reinstall(void) {
    if (recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
        return false;
    }
    recovery_attempts++;

    klog("logger: handler not responding, reinstalling (attempt %u/%u)",
         (unsigned)recovery_attempts, (unsigned)MAX_RECOVERY_ATTEMPTS);

    if (!me7_handler_install()) {
        klog("logger: reinstall failed");
        return false;
    }

    consecutive_failures = 0;
    klog("logger: handler restored, resuming");
    return true;
}

LoggerTick logger_update(bool connected) {
    if (!streaming) return LOGGER_TICK_IDLE;

    if (!connected) {
        logger_stop("link lost");
        return LOGGER_TICK_IDLE;
    }

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (interval_ms > 0 && (now - last_sample_ms) < interval_ms) {
        return LOGGER_TICK_IDLE;
    }
    last_sample_ms = now;

    KWP2000Service request = { .serviceId = 0xB7, .dataLength = 0 };
    KWP2000Response response;
    const ResponseStatus status = kwp2000_execute(&request, &response, true);

    if (status == RESPONSE_OK) {
        consecutive_failures = 0;
        push_sample(&response, now);

        if (++stats_samples % STATS_EVERY_SAMPLES == 0) {
            const uint32_t elapsed = now - stats_started_ms;
            const uint32_t rate_x10 = elapsed ? (stats_samples * 10000u) / elapsed : 0;
            klog("logger: %u samples, %u.%u/s",
                 (unsigned)stats_samples, (unsigned)(rate_x10 / 10),
                 (unsigned)(rate_x10 % 10));
        }
        return LOGGER_TICK_SAMPLED;
    }

    consecutive_failures++;
    const bool gone = handler_is_gone(status, &response);

    if (gone || consecutive_failures >= FAILURES_BEFORE_RECOVERY) {
        if (attempt_reinstall()) {
            return LOGGER_TICK_SAMPLED;
        }

        // Out of reinstall attempts, or the reinstall itself failed. A fresh
        // session fixes both, but this layer does not own the link.
        if (recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
            logger_stop("handler unrecoverable");
            return LOGGER_TICK_IDLE;
        }
        return LOGGER_TICK_NEEDS_RECONNECT;
    }

    klog("logger: sample %u -> %s", (unsigned)sequence, kwp2000_status_name(status));

    // A rejection still proves the ECU is listening.
    return (status == RESPONSE_NEGATIVE) ? LOGGER_TICK_SAMPLED : LOGGER_TICK_IDLE;
}
