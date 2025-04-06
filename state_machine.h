// State machine definitions for K-LINE intermediary
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// System states
typedef enum {
    STATE_IDLE = 0,
    STATE_ECU_CONNECTING,
    STATE_ECU_CONNECTED,
    STATE_RPI5_CONNECTED,
    STATE_FULLY_OPERATIONAL
} SystemState;

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

// State machine context
typedef struct {
    SystemState currentState;
    bool ecuConnected;
    bool rpi5Connected;
    uint32_t lastEcuHeartbeat;
    uint32_t lastRpi5Heartbeat;
    uint32_t heartbeatTimeout;
} StateMachine;

// Function declarations
void initStateMachine(StateMachine* sm);
void updateState(StateMachine* sm);
bool sendFrameToRpi5(const SerialFrame* frame);
bool receiveFrameFromRpi5(SerialFrame* frame);
void handleEcuData(const KWP2000Response* response);
void handleRpi5Command(const SerialFrame* frame);

// Debug function to print frame contents in human-readable format
void debugPrintFrame(const SerialFrame* frame, const char* prefix);

// Debug function to receive commands via serial monitor
// Format: "CMD:serviceId,param1,param2,...\r\n"
// Example: "CMD:1A,9B\r\n" for ECU ID request
bool receiveDebugCommand(SerialFrame* frame); 