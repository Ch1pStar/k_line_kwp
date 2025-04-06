#pragma once
#include "config.h"
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



// Message types for RPI5 communication
typedef enum {
    MSG_HEARTBEAT = 0x00,
    MSG_ECU_DATA = 0x01,
    MSG_ECU_DTC = 0x02,
    MSG_COMMAND = 0x03,
    MSG_ACK = 0x04,
    MSG_NACK = 0x05,
    MSG_CONNECT_ECU = 0x06,
    MSG_DISCONNECT_ECU = 0x07,
    MSG_ECU_DISCONNECTED = 0x08
} MessageType; 

// Frame format constants
#define FRAME_START_BYTE 0x7E
#define FRAME_END_BYTE 0x7F
#define MAX_FRAME_SIZE 256

// Frame structure for RPI5 communication
typedef struct {
    uint8_t messageType;
    uint8_t length;
    uint8_t payload[MAX_FRAME_SIZE];
    uint8_t checksum;
} SerialFrame;

void dashboard_init(DashboardStateMachine* sm, RingBuffer* tx, RingBuffer* rx);
void dashboard_update(DashboardStateMachine* sm);
bool dashboard_handle_command(DashboardStateMachine* sm, const SerialFrame* frame); 

bool send_frame_to_dashboard(const SerialFrame* frame);
bool receive_frame_from_dashboard(SerialFrame* frame);

// Debug function to receive commands via serial monitor
// Format: "CMD:serviceId,param1,param2,...\r\n"
// Example: "CMD:1A9B\n" for ECU ID request
bool receive_debug_command(SerialFrame* frame); 
// Debug function to print frame contents in human-readable format
void debug_print_frame(const SerialFrame* frame, const char* prefix);

bool dashboard_read_console_input(DashboardStateMachine* sm);
