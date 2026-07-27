#include "ecu_state_machine.h"
#include "uart_pio.h"
#include "kline.h"
#include "kwp2000.h"
#include "me7_handler.h"
#include "logger.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"

#define DEFAULT_HEARTBEAT_INTERVAL_MS 5000

// Once the handler is installed, keep the session short-lived enough that it
// never lapses. Recovering from a drop costs a reconnect and a reinstall, which
// the logger can now do by itself, but sampling stops while it happens.
#define LOGGING_HEARTBEAT_INTERVAL_MS 2000

void ecu_init(ECUStateMachine *sm, RingBuffer *rx, RingBuffer *tx) {
    sm->state = ECU_STATE_IDLE;
    sm->connected = false;
    sm->last_activity = 0;
    sm->heartbeat_interval_ms = DEFAULT_HEARTBEAT_INTERVAL_MS;
    sm->rx_buffer = rx;
    sm->tx_buffer = tx;

    logger_init(tx);

    // Initialize PIO RX and TX pin for K-Line
    uart_pio_init_rx();
    uart_pio_release_tx_pin();  // TX starts as plain GPIO, idle high

    klog("ECU state machine initialized");
}

static bool ecu_try_connect(ECUStateMachine *sm) {
    klog("Attempting to connect to ECU...");
    sm->state = ECU_STATE_CONNECTING;

    uint32_t result = kline_init_connection();
    if (result == 0xee) {
        sm->connected = true;
        sm->last_activity = to_ms_since_boot(get_absolute_time());
        sm->state = ECU_STATE_CONNECTED;

        // Immediately, while the ECU still accepts it: open the session and
        // move the K-line to the logging rate. Any delay here and it times out.
        me7_handler_open_fast_session();
        sm->last_activity = to_ms_since_boot(get_absolute_time());

        BufferMessage msg = {
            .messageType = MSG_ACK,
            .length = 1,
            .data = {1}
        };
        ringbuffer_push(sm->tx_buffer, &msg);
        return true;
    }

    klog("ECU connection failed");
    sm->connected = false;
    sm->state = ECU_STATE_IDLE;

    BufferMessage msg = {
        .messageType = MSG_NACK,
        .length = 1,
        .data = {0}
    };
    ringbuffer_push(sm->tx_buffer, &msg);
    return false;
}

static bool ecu_send_command(ECUStateMachine *sm, const BufferMessage *msg) {
    if (!sm->connected) {
        klog("Cannot send command - ECU not connected");
        return false;
    }

    KWP2000Service service;
    service.serviceId = msg->data[0];
    service.dataLength = msg->length - 1;
    for (size_t i = 1; i < msg->length && i - 1 < MAX_DATA_SIZE; i++) {
        service.dataBytes[i - 1] = msg->data[i];
    }

    KWP2000Response response;
    ResponseStatus status = kwp2000_execute(&service, &response, false);

    // Any reply at all - including a rejection - counts as activity.
    if (status == RESPONSE_OK || status == RESPONSE_NEGATIVE) {
        sm->last_activity = to_ms_since_boot(get_absolute_time());
    }

    if (status == RESPONSE_OK) {

        // Decode fault codes straight to the USB console. The raw bytes still go
        // to core 1 below, so the hex dump stays available for reverse engineering.
        if (service.serviceId == KWP_SID_READ_DTC_BY_STATUS) {
            DTCData dtcs[MAX_DTCS_PER_RESPONSE];
            uint8_t num_dtcs = kwp2000_parse_dtcs(&response, dtcs, MAX_DTCS_PER_RESPONSE);
            kwp2000_print_dtcs(dtcs, num_dtcs);
        }
    }

    BufferMessage response_msg;
    if (status == RESPONSE_OK) {
        response_msg.messageType = MSG_ECU_DATA;
        response_msg.length = response.dataSize;
        memcpy(response_msg.data, response.data, response.dataSize);
    } else {
        response_msg.messageType = MSG_NACK;
        response_msg.length = 1;
        response_msg.data[0] = status;
    }

    ringbuffer_push(sm->tx_buffer, &response_msg);
    return (status == RESPONSE_OK);
}

// Send a frame and dump every byte that comes back, making no assumptions about
// framing. The normal parser skips exactly the bytes it expects to be echoed;
// when that assumption is wrong every subsequent field is misread, so this is
// the tool for finding out what the ECU actually put on the wire.
static void ecu_send_raw(ECUStateMachine *sm, const BufferMessage *msg) {
    if (!sm->connected) {
        klog("Cannot send raw command - ECU not connected");
        return;
    }

    KWP2000Service service;
    service.serviceId = msg->data[0];
    service.dataLength = msg->length - 1;
    for (size_t i = 1; i < msg->length && i - 1 < MAX_DATA_SIZE; i++) {
        service.dataBytes[i - 1] = msg->data[i];
    }

    size_t sent = kwp2000_send(&service, false);
    klog("RAW: sent %u bytes (echo consumed inline); dumping the reply verbatim",
         (unsigned)sent);

    // Build one log line per 16 bytes instead of a byte at a time - the log
    // queue carries whole lines, and a per-byte printf would be 256 messages.
    size_t count = 0;
    char line[16 * 3 + 1];
    size_t line_len = 0;
    while (count < 256) {
        uint32_t byte = uart_read_byte_timeout(300000);
        if (byte == UART_TIMEOUT) break;

        line_len += snprintf(line + line_len, sizeof(line) - line_len,
                             "%02X ", (unsigned)byte);
        count++;

        if (count % 16 == 0) {
            klog("  [%3u] %s", (unsigned)(count - 16), line);
            line_len = 0;
        }
    }
    if (line_len > 0) {
        klog("  [%3u] %s", (unsigned)(count - (count % 16)), line);
    }

    klog("RAW: %u reply bytes.", (unsigned)count);

    sm->last_activity = to_ms_since_boot(get_absolute_time());

    BufferMessage ack = { .messageType = MSG_ACK, .length = 0 };
    ringbuffer_push(sm->tx_buffer, &ack);
}

static void ecu_check_connection(ECUStateMachine *sm) {
    if (!sm->connected) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - sm->last_activity < sm->heartbeat_interval_ms) return;

    // Time for a heartbeat — send keep-alive to check the connection is still up
    // klog("Sending heartbeat");
    KWP2000Service keep_alive = {.serviceId = 0x3E, .dataLength = 0};
    KWP2000Response keep_alive_resp;
    ResponseStatus status = kwp2000_execute(&keep_alive, &keep_alive_resp, true);

    // A negative response still proves the ECU is listening. This matters right
    // after the handler redirect: every service returns SNS until the handler
    // copies the service table, and treating that as a dead link tore down a
    // perfectly good connection mid-install.
    if (status == RESPONSE_OK || status == RESPONSE_NEGATIVE) {
        sm->last_activity = now;
    } else {
        klog("ECU heartbeat failed - connection lost");
        sm->connected = false;
        sm->state = ECU_STATE_IDLE;

        BufferMessage msg = {
            .messageType = MSG_ECU_DISCONNECTED,
            .length = 0
        };
        ringbuffer_push(sm->tx_buffer, &msg);
    }
}

static void ecu_process_messages(ECUStateMachine *sm) {
    BufferMessage msg;

    while (ringbuffer_pop(sm->rx_buffer, &msg)) {
        switch (msg.messageType) {
            case MSG_CONNECT_ECU:
                if (!sm->connected) {
                    ecu_try_connect(sm);
                }
                break;

            case MSG_COMMAND:
                if (sm->connected) {
                    ecu_send_command(sm, &msg);
                } else {
                    BufferMessage nack = {
                        .messageType = MSG_NACK,
                        .length = 1,
                        .data = {0xFF}
                    };
                    ringbuffer_push(sm->tx_buffer, &nack);
                }
                break;

            case MSG_DISCONNECT_ECU:
                if (sm->connected) {
                    klog("Disconnecting from ECU");
                    sm->connected = false;
                    sm->state = ECU_STATE_IDLE;

                    BufferMessage ack = {
                        .messageType = MSG_ACK,
                        .length = 0
                    };
                    ringbuffer_push(sm->tx_buffer, &ack);
                }
                break;

            case MSG_RAW_COMMAND:
                ecu_send_raw(sm, &msg);
                break;

            // Handler injection runs here, on core 0, so each step can wait for
            // the ECU's own reply before the next goes out. It used to be
            // driven from core 1 through fixed sleeps around a queue, which
            // meant guessing the pacing and blocking the console for ~8s.
            case MSG_INSTALL_HANDLER:
            case MSG_LOAD_HANDLER:
                if (!sm->connected) {
                    klog("Cannot install handler - ECU not connected");
                    BufferMessage nack = {
                        .messageType = MSG_NACK, .length = 1, .data = {0xFF}
                    };
                    ringbuffer_push(sm->tx_buffer, &nack);
                    break;
                }

                if (msg.messageType == MSG_INSTALL_HANDLER) {
                    sm->heartbeat_interval_ms = LOGGING_HEARTBEAT_INTERVAL_MS;
                    klog("Heartbeat interval set to %u ms",
                         (unsigned)sm->heartbeat_interval_ms);
                }

                {
                    const bool ok = (msg.messageType == MSG_INSTALL_HANDLER)
                                        ? me7_handler_install()
                                        : me7_handler_load();

                    // The sequence is a solid stream of requests, so the link
                    // was live throughout - don't let the heartbeat fire the
                    // moment it finishes.
                    sm->last_activity = to_ms_since_boot(get_absolute_time());

                    BufferMessage reply = {
                        .messageType = ok ? MSG_ACK : MSG_NACK,
                        .length = ok ? 0 : 1,
                        .data = {0},
                    };
                    ringbuffer_push(sm->tx_buffer, &reply);
                }
                break;

            case MSG_START_STREAM:
                if (!sm->connected) {
                    klog("Cannot start streaming - ECU not connected");
                    BufferMessage nack = {
                        .messageType = MSG_NACK, .length = 1, .data = {0xFF}
                    };
                    ringbuffer_push(sm->tx_buffer, &nack);
                    break;
                }
                logger_start(msg.length >= 4
                                 ? ((uint32_t)msg.data[0] << 24) |
                                   ((uint32_t)msg.data[1] << 16) |
                                   ((uint32_t)msg.data[2] << 8)  |
                                   ((uint32_t)msg.data[3])
                                 : 0);
                break;

            case MSG_STOP_STREAM:
                logger_stop("requested");
                break;

            case MSG_SET_LOG_VARS:
                if (!sm->connected) {
                    klog("Cannot set variables - ECU not connected");
                    break;
                }
                if (me7_handler_set_vars(msg.data, msg.length)) {
                    sm->last_activity = to_ms_since_boot(get_absolute_time());
                }
                break;

            case MSG_SET_BAUD:
                if (msg.length >= 4) {
                    uint32_t baud =
                        ((uint32_t)msg.data[0] << 24) |
                        ((uint32_t)msg.data[1] << 16) |
                        ((uint32_t)msg.data[2] << 8)  |
                        ((uint32_t)msg.data[3]);
                    klog("K-line baud: %u -> %u",
                         (unsigned)uart_get_baud(), (unsigned)baud);
                    uart_set_baud(baud);
                }
                break;

            case MSG_SET_HEARTBEAT:
                if (msg.length >= 4) {
                    sm->heartbeat_interval_ms =
                        ((uint32_t)msg.data[0] << 24) |
                        ((uint32_t)msg.data[1] << 16) |
                        ((uint32_t)msg.data[2] << 8)  |
                        ((uint32_t)msg.data[3]);
                    klog("Heartbeat interval set to %u ms", (unsigned)sm->heartbeat_interval_ms);
                }
                break;

            default:
                break;
        }
    }
}

// The logger owns sampling but not the link, so when the handler cannot be
// reinstalled it asks for a reconnect and this is where that happens.
static void ecu_run_logger(ECUStateMachine *sm) {
    switch (logger_update(sm->connected)) {
        case LOGGER_TICK_SAMPLED:
            // Sample traffic is what keeps the session alive while streaming.
            sm->last_activity = to_ms_since_boot(get_absolute_time());
            break;

        case LOGGER_TICK_NEEDS_RECONNECT: {
            klog("logger: recovering the handler through a fresh session");
            sm->connected = false;
            sm->state = ECU_STATE_IDLE;

            // The ECU ignores a 5-baud init while it still believes a session
            // is open, so let its P3max (~3-5s) lapse first.
            sleep_ms(4000);

            if (ecu_try_connect(sm) && me7_handler_install()) {
                sm->last_activity = to_ms_since_boot(get_absolute_time());
                klog("logger: recovered, resuming");
            } else {
                logger_stop("reconnect recovery failed");
            }
            break;
        }

        case LOGGER_TICK_IDLE:
            break;
    }
}

void ecu_update(ECUStateMachine *sm) {
    ecu_check_connection(sm);
    ecu_process_messages(sm);
    ecu_run_logger(sm);
}
