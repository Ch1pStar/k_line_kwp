#pragma once
#include "state_machine.h"
#include "ring_buffer.h"

typedef enum {
    DASH_STATE_DISCONNECTED,
    DASH_STATE_CONNECTED,
    DASH_STATE_SENDING,
    DASH_STATE_WAITING_RESPONSE
} DashboardState;

typedef struct {
    DashboardState currentState;
    uint32_t lastHeartbeat;
    bool connected;
    RingBuffer* txBuffer;  // Messages to ECU
    RingBuffer* rxBuffer;  // Messages from ECU
} DashboardStateMachine;

void dashboard_init(DashboardStateMachine* sm, RingBuffer* tx, RingBuffer* rx);
void dashboard_update(DashboardStateMachine* sm);
bool dashboard_handle_command(DashboardStateMachine* sm, const SerialFrame* frame); 