#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ring_buffer.h"
#include "messages.h"

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
