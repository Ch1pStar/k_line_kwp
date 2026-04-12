#include "ecu_state_machine.h"
#include "uart.h"
#include "kline.h"
#include "kwp2000.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"

#define DEFAULT_HEARTBEAT_INTERVAL_MS 5000

void ecu_init(ECUStateMachine *sm, RingBuffer *rx, RingBuffer *tx) {
    sm->state = ECU_STATE_IDLE;
    sm->connected = false;
    sm->last_activity = 0;
    sm->heartbeat_interval_ms = DEFAULT_HEARTBEAT_INTERVAL_MS;
    sm->rx_buffer = rx;
    sm->tx_buffer = tx;

    // Initialize PIO RX and TX pin for K-Line
    uart_pio_init_rx();
    gpio_init(PIO_TX_PIN);
    gpio_set_dir(PIO_TX_PIN, GPIO_OUT);
    gpio_put(PIO_TX_PIN, 1); // Idle state is high

    printf("ECU state machine initialized\n");
}

static bool ecu_try_connect(ECUStateMachine *sm) {
    printf("Attempting to connect to ECU...\n");
    sm->state = ECU_STATE_CONNECTING;

    uint32_t result = kline_init_connection();
    if (result == 0xee) {
        sm->connected = true;
        sm->last_activity = to_ms_since_boot(get_absolute_time());
        sm->state = ECU_STATE_CONNECTED;

        BufferMessage msg = {
            .messageType = MSG_ACK,
            .length = 1,
            .data = {1}
        };
        ringbuffer_push(sm->tx_buffer, &msg);
        return true;
    }

    printf("ECU connection failed\n");
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
        printf("Cannot send command - ECU not connected\n");
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

    if (status == RESPONSE_OK) {
        sm->last_activity = to_ms_since_boot(get_absolute_time());
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

static void ecu_check_connection(ECUStateMachine *sm) {
    if (!sm->connected) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - sm->last_activity < sm->heartbeat_interval_ms) return;

    // Time for a heartbeat — send keep-alive to check the connection is still up
    // printf("Sending heartbeat\n");
    KWP2000Service keep_alive = {.serviceId = 0x3E, .dataLength = 0};
    KWP2000Response keep_alive_resp;
    ResponseStatus status = kwp2000_execute(&keep_alive, &keep_alive_resp, true);

    if (status == RESPONSE_OK) {
        sm->last_activity = now;
    } else {
        printf("ECU heartbeat failed - connection lost\n");
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
                    printf("Disconnecting from ECU\n");
                    sm->connected = false;
                    sm->state = ECU_STATE_IDLE;

                    BufferMessage ack = {
                        .messageType = MSG_ACK,
                        .length = 0
                    };
                    ringbuffer_push(sm->tx_buffer, &ack);
                }
                break;

            case MSG_SET_HEARTBEAT:
                if (msg.length >= 4) {
                    sm->heartbeat_interval_ms =
                        ((uint32_t)msg.data[0] << 24) |
                        ((uint32_t)msg.data[1] << 16) |
                        ((uint32_t)msg.data[2] << 8)  |
                        ((uint32_t)msg.data[3]);
                    printf("Heartbeat interval set to %lu ms\n", sm->heartbeat_interval_ms);
                }
                break;

            default:
                break;
        }
    }
}

void ecu_update(ECUStateMachine *sm) {
    ecu_check_connection(sm);
    ecu_process_messages(sm);
}
