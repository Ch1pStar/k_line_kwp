#include "dashboard_state_machine.h"
#include "config.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

#define DASH_HEARTBEAT_INTERVAL 500  // 500ms
#define DASH_CONNECTION_TIMEOUT 5000 // 5 seconds

void dashboard_init(DashboardStateMachine* sm, RingBuffer* tx, RingBuffer* rx) {
    sm->currentState = DASH_STATE_DISCONNECTED;
    sm->connected = false;
    sm->lastHeartbeat = 0;
    sm->txBuffer = tx;
    sm->rxBuffer = rx;
}

bool dashboard_handle_frame(DashboardStateMachine* sm, const SerialFrame* frame) {
    switch (frame->messageType) {
        case MSG_HEARTBEAT:
            // Update heartbeat timestamp
            sm->lastHeartbeat = to_ms_since_boot(get_absolute_time());
            sm->connected = true;
            
            if (sm->currentState == DASH_STATE_DISCONNECTED) {
                sm->currentState = DASH_STATE_CONNECTED;
                printf("Dashboard connected\n");
            }
            
            // Send ACK
            SerialFrame ack = {
                .messageType = MSG_ACK,
                .length = 0
            };
            sendFrameToRpi5(&ack);
            break;
            
        case MSG_CONNECT_ECU:
            // Forward connection request to ECU state machine
            BufferMessage connectMsg = {
                .messageType = MSG_CONNECT_ECU,
                .length = 0
            };
            ringbuffer_push(sm->txBuffer, &connectMsg);
            sm->currentState = DASH_STATE_WAITING_RESPONSE;
            break;
            
        case MSG_COMMAND:
            // Forward command to ECU state machine
            if (frame->length > 0 && frame->length <= MAX_MESSAGE_SIZE) {
                BufferMessage cmdMsg = {
                    .messageType = MSG_COMMAND,
                    .length = frame->length
                };
                memcpy(cmdMsg.data, frame->payload, frame->length);
                ringbuffer_push(sm->txBuffer, &cmdMsg);
                sm->currentState = DASH_STATE_WAITING_RESPONSE;
            } else {
                printf("Invalid command length: %d\n", frame->length);
                SerialFrame nack = {
                    .messageType = MSG_NACK,
                    .length = 1,
                    .payload = {0xFE} // Invalid length error
                };
                sendFrameToRpi5(&nack);
            }
            break;
            
        case MSG_DISCONNECT_ECU:
            // Forward disconnect request to ECU state machine
            BufferMessage disconnectMsg = {
                .messageType = MSG_DISCONNECT_ECU,
                .length = 0
            };
            ringbuffer_push(sm->txBuffer, &disconnectMsg);
            break;
            
        default:
            printf("Unknown message type: %d\n", frame->messageType);
            return false;
    }
    
    return true;
}

bool dashboard_handle_command(DashboardStateMachine* sm, const SerialFrame* frame) {
    // This handles debug commands from serial monitor
    return dashboard_handle_frame(sm, frame);
}

void dashboard_process_ecu_messages(DashboardStateMachine* sm) {
    BufferMessage msg;
    
    while (ringbuffer_pop(sm->rxBuffer, &msg)) {
        // Convert buffer message to serial frame for RPI5
        SerialFrame frame = {
            .messageType = msg.messageType,
            .length = msg.length
        };
        memcpy(frame.payload, msg.data, msg.length);
        
        // Calculate checksum
        frame.checksum = frame.messageType ^ frame.length;
        for (int i = 0; i < frame.length; i++) {
            frame.checksum ^= frame.payload[i];
        }
        
        // Send to RPI5
        // sendFrameToRpi5(&frame);
        // debugPrintFrame(&frame, "Dashboard");

        // Update state if we were waiting for a response
        if (sm->currentState == DASH_STATE_WAITING_RESPONSE) {
            sm->currentState = DASH_STATE_CONNECTED;
        }
    }
}

void dashboard_check_connection(DashboardStateMachine* sm) {
    uint32_t currentTime = to_ms_since_boot(get_absolute_time());
    
    // Check for dashboard timeout
    if (sm->connected && (currentTime - sm->lastHeartbeat > DASH_CONNECTION_TIMEOUT)) {
        printf("Dashboard connection timeout\n");
        sm->connected = false;
        sm->currentState = DASH_STATE_DISCONNECTED;
    }
}

void dashboard_update(DashboardStateMachine* sm) {
    // Check connection status
    dashboard_check_connection(sm);
    
    // Check for incoming messages from RPI5
    SerialFrame frame;
    if (receiveFrameFromRpi5(&frame)) {
        dashboard_handle_frame(sm, &frame);
    }
    
    // Check for debug commands
    if (receiveDebugCommand(&frame)) {
        dashboard_handle_command(sm, &frame);
    }
    
    // Process ECU responses
    dashboard_process_ecu_messages(sm);
    
    // Send periodic heartbeats to dashboard if connected
    static uint32_t lastSentHeartbeat = 0;
    uint32_t currentTime = to_ms_since_boot(get_absolute_time());
    
    if (sm->connected && (currentTime - lastSentHeartbeat > DASH_HEARTBEAT_INTERVAL)) {
        SerialFrame heartbeat = {
            .messageType = MSG_HEARTBEAT,
            .length = 0
        };
        sendFrameToRpi5(&heartbeat);
        lastSentHeartbeat = currentTime;
    }
} 