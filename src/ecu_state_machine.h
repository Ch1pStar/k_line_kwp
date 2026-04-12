#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ring_buffer.h"

// Message types for inter-core communication
typedef enum {
    MSG_ECU_DATA            = 0x01,
    MSG_COMMAND             = 0x03,
    MSG_ACK                 = 0x04,
    MSG_NACK                = 0x05,
    MSG_CONNECT_ECU         = 0x06,
    MSG_DISCONNECT_ECU      = 0x07,
    MSG_ECU_DISCONNECTED    = 0x08,
    MSG_SET_HEARTBEAT       = 0x09,
} MessageType;

typedef enum {
    ECU_STATE_IDLE,
    ECU_STATE_CONNECTING,
    ECU_STATE_CONNECTED,
} ECUState;

typedef struct {
    ECUState state;
    uint32_t heartbeat_interval_ms;
    uint32_t last_activity;
    bool connected;
    RingBuffer *rx_buffer;  // Messages from Dashboard
    RingBuffer *tx_buffer;  // Messages to Dashboard
} ECUStateMachine;

void ecu_init(ECUStateMachine *sm, RingBuffer *rx, RingBuffer *tx);
void ecu_update(ECUStateMachine *sm);
