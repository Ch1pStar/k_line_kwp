#include "dashboard_state_machine.h"
#include "ecu_state_machine.h"
#include "config.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DASH_HEARTBEAT_INTERVAL 500  // 500ms
#define DASH_CONNECTION_TIMEOUT 5000 // 5 seconds

void dashboard_init(DashboardStateMachine* sm, RingBuffer* tx, RingBuffer* rx);
void dashboard_update(DashboardStateMachine* sm);
bool dashboard_handle_frame(DashboardStateMachine* sm, const SerialFrame* frame);
bool dashboard_handle_command(DashboardStateMachine* sm, const SerialFrame* frame);
void dashboard_process_ecu_messages(DashboardStateMachine* sm);
void dashboard_check_connection(DashboardStateMachine* sm);

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

bool receiveDebugCommand(SerialFrame* frame) {
    char cmd_buffer[128];
    
    // Read a line from input using fgets
    if (fgets(cmd_buffer, sizeof(cmd_buffer), stdin) != NULL) {
        size_t buf_pos = strlen(cmd_buffer);

        // Remove newline characters if present
        if (buf_pos > 0 && (cmd_buffer[buf_pos - 1] == '\n' || cmd_buffer[buf_pos - 1] == '\r')) {
            cmd_buffer[--buf_pos] = '\0';
        }

        printf("Complete command received: \"%s\" (length: %zu)\n", cmd_buffer, buf_pos);

        // Check for connect command
        if (strcmp(cmd_buffer, "CONNECT") == 0) {
            frame->messageType = MSG_CONNECT_ECU;
            frame->length = 0;
            return true;
        }
        // Check for normal KWP commands
        else if (strncmp(cmd_buffer, "CMD:", 4) == 0) {
            const char* hex_str = cmd_buffer + 4;  // Skip "CMD:"
            size_t hex_len = strlen(hex_str);
            
            if (hex_len % 2 != 0) {
                printf("Error: Hex string length must be even\n");
                return false;
            }

            // First byte is service ID
            frame->messageType = MSG_COMMAND;
            frame->length = 0;

            // Convert hex pairs to bytes
            for (size_t i = 0; i < hex_len; i += 2) {
                char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
                uint8_t byte = (uint8_t)strtol(hex_byte, NULL, 16);
                
                if (frame->length < MAX_FRAME_SIZE) {
                    frame->payload[frame->length++] = byte;
                } else {
                    printf("Error: Command too long\n");
                    return false;
                }
            }

            // Calculate checksum
            frame->checksum = frame->messageType ^ frame->length;
            for (int i = 0; i < frame->length; i++) {
                frame->checksum ^= frame->payload[i];
            }

            printf("\nDebug command received...");
            debugPrintFrame(frame, "Debug Input");
            return true;
        }
    }

    return false;
}

// TODO: not used for now
bool receiveFrameFromRpi5(SerialFrame* frame) {
    // Wait for start byte with non-blocking read
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT || c != FRAME_START_BYTE) {
        return false;
    }
    
    // Read message type
    c = getchar_timeout_us(100000); // 100ms timeout
    if (c == PICO_ERROR_TIMEOUT) return false;
    frame->messageType = (uint8_t)c;
    
    // Read length
    c = getchar_timeout_us(100000);
    if (c == PICO_ERROR_TIMEOUT) return false;
    frame->length = (uint8_t)c;
    
    // Read payload
    uint8_t checksum = frame->messageType ^ frame->length;
    for (int i = 0; i < frame->length; i++) {
        c = getchar_timeout_us(100000);
        if (c == PICO_ERROR_TIMEOUT) return false;
        frame->payload[i] = (uint8_t)c;
        checksum ^= frame->payload[i];
    }
    
    // Read and verify checksum
    c = getchar_timeout_us(100000);
    if (c == PICO_ERROR_TIMEOUT) return false;
    if (checksum != (uint8_t)c) return false;
    
    // Read end byte
    c = getchar_timeout_us(100000);
    if (c == PICO_ERROR_TIMEOUT || c != FRAME_END_BYTE) return false;
    
    return true;
}

void debugPrintFrame(const SerialFrame* frame, const char* prefix) {
    const char* messageTypeStr;
    switch (frame->messageType) {
        case MSG_HEARTBEAT:
            messageTypeStr = "HEARTBEAT";
            break;
        case MSG_ECU_DATA:
            messageTypeStr = "ECU_DATA";
            break;
        case MSG_ECU_DTC:
            messageTypeStr = "ECU_DTC";
            break;
        case MSG_COMMAND:
            messageTypeStr = "COMMAND";
            break;
        case MSG_ACK:
            messageTypeStr = "ACK";
            break;
        case MSG_NACK:
            messageTypeStr = "NACK";
            break;
        default:
            messageTypeStr = "UNKNOWN";
    }

    // Calculate checksum for verification
    uint8_t checksum = frame->messageType ^ frame->length;
    for (int i = 0; i < frame->length; i++) {
        checksum ^= frame->payload[i];
    }

    // Print frame header
    printf("\n%s Frame:\n", prefix);
    printf("├─ Type: %s (0x%02X)\n", messageTypeStr, frame->messageType);
    printf("├─ Length: %d\n", frame->length);
    
    // Print payload in both hex and ASCII format
    printf("├─ Payload:\n");
    printf("│  ├─ Hex: ");
    for (int i = 0; i < frame->length; i++) {
        printf("%02X ", frame->payload[i]);
        if ((i + 1) % 16 == 0 && i + 1 < frame->length) {
            printf("\n│  │     ");
        }
    }
    printf("\n│  └─ ASCII: ");
    for (int i = 0; i < frame->length; i++) {
        if (frame->payload[i] >= 32 && frame->payload[i] <= 126) {
            printf("%c", frame->payload[i]);
        } else {
            printf(".");
        }
    }
    printf("\n");
    
    // Print checksum information
    printf("└─ Checksum: 0x%02X (%s)\n", frame->checksum,
           (checksum == frame->checksum) ? "Valid" : "Invalid");
    printf("\n");
}

bool sendFrameToRpi5(const SerialFrame* frame) {
    // Calculate checksum (simple XOR of all bytes)
    uint8_t checksum = frame->messageType ^ frame->length;
    for (int i = 0; i < frame->length; i++) {
        checksum ^= frame->payload[i];
    }
    
    // Send frame
    putchar(FRAME_START_BYTE);
    putchar(frame->messageType);
    putchar(frame->length);
    
    for (int i = 0; i < frame->length; i++) {
        putchar(frame->payload[i]);
    }
    
    putchar(checksum);
    putchar(FRAME_END_BYTE);
    
    return true;
}

void dashboard_init(DashboardStateMachine* sm, RingBuffer* tx, RingBuffer* rx) {
    sm->currentState = DASH_STATE_DISCONNECTED;
    sm->connected = false;
    sm->lastHeartbeat = 0;
    sm->txBuffer = tx;
    sm->rxBuffer = rx;
}
