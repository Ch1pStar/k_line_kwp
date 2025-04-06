#pragma once
#include "ring_buffer.h"

typedef enum {
    ECU_STATE_IDLE,
    ECU_STATE_CONNECTING,
    ECU_STATE_CONNECTED,
    ECU_STATE_PROCESSING
} ECUState;

typedef struct {
    ECUState currentState;
    uint32_t lastHeartbeat;
    bool connected;
    RingBuffer* rxBuffer;  // Messages from Dashboard
    RingBuffer* txBuffer;  // Messages to Dashboard
} ECUStateMachine;

void ecu_init(ECUStateMachine* sm, RingBuffer* rx, RingBuffer* tx);
void ecu_update(ECUStateMachine* sm);
bool ecu_process_message(ECUStateMachine* sm); 