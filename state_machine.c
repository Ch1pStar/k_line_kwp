#include "state_machine.h"
#include <string.h>
#include <stdlib.h>

// Default timeout for heartbeat messages (ms)
#define HEARTBEAT_TIMEOUT 4000

void initStateMachine(StateMachine* sm) {
    sm->currentState = STATE_IDLE;
    sm->ecuConnected = false;
    sm->rpi5Connected = false;
    sm->lastEcuHeartbeat = 0;
    sm->lastRpi5Heartbeat = 0;
    sm->heartbeatTimeout = HEARTBEAT_TIMEOUT;
}

void updateState(StateMachine* sm) {
    uint32_t currentTime = to_ms_since_boot(get_absolute_time());
    
    // Check for timeouts
    if (sm->ecuConnected && (currentTime - sm->lastEcuHeartbeat > sm->heartbeatTimeout)) {
        sm->ecuConnected = false;
    }
   
    // if (sm->rpi5Connected && (currentTime - sm->lastRpi5Heartbeat > sm->heartbeatTimeout)) {
    //     sm->rpi5Connected = false;
    // }
    
    // Update state based on connections
    switch (sm->currentState) {
        case STATE_IDLE:
            if (sm->ecuConnected) {
                sm->currentState = STATE_ECU_CONNECTED;
            }
            break;
            
        case STATE_ECU_CONNECTING:
            if (sm->ecuConnected) {
                sm->currentState = STATE_ECU_CONNECTED;
            } else if (!sm->ecuConnected) {
                sm->currentState = STATE_IDLE;
            }
            break;
            
        case STATE_ECU_CONNECTED:
            if (!sm->ecuConnected) {
                sm->currentState = STATE_IDLE;
            } else if (sm->rpi5Connected) {
                sm->currentState = STATE_FULLY_OPERATIONAL;
            }
            break;
            
        case STATE_RPI5_CONNECTED:
            if (!sm->rpi5Connected) {
                sm->currentState = STATE_IDLE;
            } else if (sm->ecuConnected) {
                sm->currentState = STATE_FULLY_OPERATIONAL;
            }
            break;
            
        case STATE_FULLY_OPERATIONAL:
            if (!sm->ecuConnected) {
                sm->currentState = STATE_RPI5_CONNECTED;
            } else if (!sm->rpi5Connected) {
                sm->currentState = STATE_ECU_CONNECTED;
            }
            break;
    }
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

void handleEcuData(const KWP2000Response* response) {
    SerialFrame frame;
    frame.messageType = MSG_ECU_DATA;
    frame.length = response->dataSize;
    
    // Copy response data to frame payload
    for (size_t i = 0; i < response->dataSize && i < MAX_FRAME_SIZE; i++) {
        frame.payload[i] = response->data[i];
    }
    
    // Calculate checksum
    frame.checksum = frame.messageType ^ frame.length;
    for (size_t i = 0; i < frame.length; i++) {
        frame.checksum ^= frame.payload[i];
    }
    
    // Print debug information
    debugPrintFrame(&frame, "ECU Response");
    
    // Send frame to RPI5
    sendFrameToRpi5(&frame);
}

void handleRpi5Command(const SerialFrame* frame) {
    // Convert RPI5 command to KWP2000 service
    KWP2000Service service;
    service.serviceId = frame->payload[0];
    service.dataLength = frame->length - 1;
    
    // Copy command parameters
    for (size_t i = 1; i < frame->length && i - 1 < MAX_DATA_SIZE; i++) {
        service.dataBytes[i-1] = frame->payload[i];
    }
    
    // Send command to ECU
    size_t packet_len = build_packet(&service);
    
    // Read response
    KWP2000Response response;
    ResponseStatus status = read_response(packet_len, &response);
    
    // Send response back to RPI5
    if (status == RESPONSE_OK) {
        handleEcuData(&response);
    } else {
        SerialFrame errorFrame = {
            .messageType = MSG_NACK,
            .length = 1,
            .payload = {status}
        };
        sendFrameToRpi5(&errorFrame);
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