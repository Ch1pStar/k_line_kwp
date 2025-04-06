#include "dashboard_state_machine.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/**
    This is not a state machine currently for simplicity purposes.
    Maybe in the future it will be a state machine if necessary, but for now it is just a simple interface.
 */

void dashboard_init(DashboardStateMachine* sm, RingBuffer* tx, RingBuffer* rx) {
    sm->rxBuffer = rx;
    sm->txBuffer = tx;
    printf("Dashboard interface ready. Type 'help' for commands.\n");
}

// Process commands from debug console
void dashboard_process_debug_command(DashboardStateMachine* sm, const char* cmd_buffer) {
    printf("Command received: \"%s\"\n", cmd_buffer);
    
    // Connect to ECU
    if (strcmp(cmd_buffer, "connect") == 0) {
        printf("Attempting to connect to ECU...\n");
        BufferMessage connectMsg = {
            .messageType = MSG_CONNECT_ECU,
            .length = 0
        };
        ringbuffer_push(sm->txBuffer, &connectMsg);
        return;
    }
    
    // Disconnect from ECU
    if (strcmp(cmd_buffer, "disconnect") == 0) {
        printf("Disconnecting from ECU...\n");
        BufferMessage disconnectMsg = {
            .messageType = MSG_DISCONNECT_ECU,
            .length = 0
        };
        ringbuffer_push(sm->txBuffer, &disconnectMsg);
        return;
    }
    
    // Help command
    if (strcmp(cmd_buffer, "help") == 0) {
        printf("\n--- Available Commands ---\n");
        printf("connect          - Connect to ECU\n");
        printf("disconnect       - Disconnect from ECU\n");
        printf("ecu-id           - Request ECU identification\n");
        printf("read-dtcs        - Read diagnostic trouble codes\n");
        printf("clear-dtcs       - Clear diagnostic trouble codes\n");
        printf("cmd:XXXX         - Send raw KWP2000 command (hex format)\n");
        printf("help             - Show this help\n");
        printf("------------------------\n\n");
        return;
    }
    
    // ECU ID command
    if (strcmp(cmd_buffer, "ecu-id") == 0) {
        printf("Requesting ECU ID...\n");
        BufferMessage cmdMsg = {
            .messageType = MSG_COMMAND,
            .length = 2,
            .data = {0x1A, 0x9B}  // ECU ID service ID and parameter
        };
        ringbuffer_push(sm->txBuffer, &cmdMsg);
        return;
    }
    
    // Read DTCs command
    if (strcmp(cmd_buffer, "read-dtcs") == 0) {
        printf("Reading DTCs...\n");
        BufferMessage cmdMsg = {
            .messageType = MSG_COMMAND,
            .length = 4,
            .data = {0x18, 0x00, 0xFF, 0x00}  // Read DTCs service 
        };
        ringbuffer_push(sm->txBuffer, &cmdMsg);
        return;
    }
    
    // Clear DTCs command
    if (strcmp(cmd_buffer, "clear-dtcs") == 0) {
        printf("Clearing DTCs...\n");
        BufferMessage cmdMsg = {
            .messageType = MSG_COMMAND,
            .length = 3,
            .data = {0x14, 0xFF, 0x00}  // Clear DTCs service
        };
        ringbuffer_push(sm->txBuffer, &cmdMsg);
        return;
    }
    
    // Raw KWP2000 command (hexadecimal) "cmd:1A9B"
    if (strncmp(cmd_buffer, "cmd:", 4) == 0) {
        const char* hex_str = cmd_buffer + 4;  // Skip "cmd:"
        size_t hex_len = strlen(hex_str);
        
        if (hex_len == 0 || hex_len % 2 != 0) {
            printf("Error: Hex string must have even number of characters\n");
            return;
        }
        
        BufferMessage cmdMsg = {
            .messageType = MSG_COMMAND,
            .length = hex_len / 2
        };
        
        // Convert each 2 characters to a byte
        for (size_t i = 0; i < hex_len; i += 2) {
            char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
            cmdMsg.data[i/2] = (uint8_t)strtol(hex_byte, NULL, 16);
        }
        
        printf("Sending custom command: ");
        for (size_t i = 0; i < cmdMsg.length; i++) {
            printf("%02X ", cmdMsg.data[i]);
        }
        printf("\n");
        
        ringbuffer_push(sm->txBuffer, &cmdMsg);
        return;
    }
    
    // Unknown command
    printf("Unknown command: \"%s\". Type 'help' for available commands.\n", cmd_buffer);
}

// Process responses from ECU
void dashboard_process_ecu_messages(DashboardStateMachine* sm) {
    BufferMessage msg;
    
    while (ringbuffer_pop(sm->rxBuffer, &msg)) {
        switch (msg.messageType) {
            case MSG_ACK:
                if (msg.length > 0 && msg.data[0] == 1) {
                    printf("ECU connected successfully\n");
                } else {
                    printf("Command acknowledged\n");
                }
                break;
                
            case MSG_NACK:
                printf("Command failed with error code: %d\n", 
                       msg.length > 0 ? msg.data[0] : 0);
                break;
                
            case MSG_ECU_DATA:
                printf("ECU Response Data (%d bytes): ", msg.length);
                // Print hex values
                for (size_t i = 0; i < msg.length; i++) {
                    printf("%02X ", msg.data[i]);
                }
                printf("\n");
                
                // Also try to print as ASCII if possible
                printf("ECU Response ASCII: \"");
                for (size_t i = 0; i < msg.length; i++) {
                    if (msg.data[i] >= 32 && msg.data[i] <= 126) {
                        putchar(msg.data[i]);
                    } else {
                        putchar('.');
                    }
                }
                printf("\"\n");
                break;
                
            case MSG_ECU_DISCONNECTED:
                printf("ECU disconnected\n");
                break;
                
            default:
                printf("Unknown response type: %d\n", msg.messageType);
                break;
        }
    }
}

// Read input from the console
bool dashboard_read_console_input(DashboardStateMachine* sm) {
    static char cmd_buffer[128];
    static int buf_pos = 0;
    int c;
    
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\n' || c == '\r') {
            if (buf_pos > 0) {
                cmd_buffer[buf_pos] = '\0';
                dashboard_process_debug_command(sm, cmd_buffer);
                buf_pos = 0;
                return true;
            }
        } else if (buf_pos < sizeof(cmd_buffer) - 1 && c >= 32 && c <= 126) {
            cmd_buffer[buf_pos++] = (char)c;
        }
    }
    
    return false;
}

void dashboard_update(DashboardStateMachine* sm) {
    // Process user input from console
    dashboard_read_console_input(sm);
    
    // Process responses from ECU
    dashboard_process_ecu_messages(sm);
}
