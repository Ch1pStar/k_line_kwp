#include "dashboard_state_machine.h"
#include "ecu_state_machine.h"
#include "config.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

#define ECU_HEARTBEAT_INTERVAL 2000  // 2 seconds
#define ECU_CONNECTION_TIMEOUT 5000  // 5 seconds

void ecu_init(ECUStateMachine* sm, RingBuffer* rx, RingBuffer* tx) {
    sm->currentState = ECU_STATE_IDLE;
    sm->connected = false;
    sm->lastHeartbeat = 0;
    sm->rxBuffer = rx;
    sm->txBuffer = tx;

    // Initialize PIO for K-LINE communication
    init_pio_rx();
    gpio_init(PIO_TX_PIN);
    gpio_set_dir(PIO_TX_PIN, GPIO_OUT);
    gpio_put(PIO_TX_PIN, 1); // Idle state is high

    printf("ECU state machine initialized\n");
}

bool ecu_try_connect(ECUStateMachine* sm) {
    printf("Attempting to connect to ECU...\n");
    sm->currentState = ECU_STATE_CONNECTING;
    
    uint32_t result = init_comm_protocol();
    if (result == 0xee) {
        printf("ECU connection successful\n");
        sm->connected = true;
        sm->lastHeartbeat = to_ms_since_boot(get_absolute_time());
        sm->currentState = ECU_STATE_CONNECTED;
        
        // Send success acknowledgment to dashboard
        BufferMessage msg = {
            .messageType = MSG_ACK,
            .length = 1,
            .data = {1} // 1 = success
        };
        ringbuffer_push(sm->txBuffer, &msg);
        
        return true;
    } else {
        printf("ECU connection failed\n");
        sm->connected = false;
        sm->currentState = ECU_STATE_IDLE;
        
        // Send failure acknowledgment to dashboard
        BufferMessage msg = {
            .messageType = MSG_NACK,
            .length = 1,
            .data = {0} // 0 = failure
        };
        ringbuffer_push(sm->txBuffer, &msg);
        
        return false;
    }
}

bool ecu_send_command(ECUStateMachine* sm, const BufferMessage* msg) {
    if (!sm->connected) {
        printf("Cannot send command - ECU not connected\n");
        return false;
    }
    
    // Convert buffer message to KWP2000 service
    KWP2000Service service;
    service.serviceId = msg->data[0];
    service.dataLength = msg->length - 1;
    
    // Copy command parameters
    for (size_t i = 1; i < msg->length && i - 1 < MAX_DATA_SIZE; i++) {
        service.dataBytes[i-1] = msg->data[i];
    }
    
    // Send command to ECU - silent mode to reduce console spam
    size_t packet_len = build_packet(&service);
    
    // Read response - silent mode to reduce console spam
    KWP2000Response response;
    ResponseStatus status = read_response(packet_len, &response);
    
    // Update heartbeat timestamp on successful communication
    if (status == RESPONSE_OK) {
        sm->lastHeartbeat = to_ms_since_boot(get_absolute_time());
    }
    
    // Forward response to dashboard
    BufferMessage responseMsg;
    if (status == RESPONSE_OK) {
        responseMsg.messageType = MSG_ECU_DATA;
        responseMsg.length = response.dataSize;
        memcpy(responseMsg.data, response.data, response.dataSize);
    } else {
        responseMsg.messageType = MSG_NACK;
        responseMsg.length = 1;
        responseMsg.data[0] = status;
    }
    
    ringbuffer_push(sm->txBuffer, &responseMsg);
    return (status == RESPONSE_OK);
}

void ecu_check_connection(ECUStateMachine* sm) {
    uint32_t currentTime = to_ms_since_boot(get_absolute_time());
    
    // If connected, check connection timeout
    if (sm->connected) {
        if (currentTime - sm->lastHeartbeat > ECU_CONNECTION_TIMEOUT) {
            printf("ECU connection timeout\n");
            sm->connected = false;
            sm->currentState = ECU_STATE_IDLE;
            
            // Notify dashboard of disconnection
            BufferMessage msg = {
                .messageType = MSG_ECU_DISCONNECTED,
                .length = 0
            };
            ringbuffer_push(sm->txBuffer, &msg);
        }
    }

    // Send periodic heartbeat if connected and enough time has passed
    if (sm->connected && currentTime - sm->lastHeartbeat > ECU_HEARTBEAT_INTERVAL) {
        // Send a silent ECU ID request as heartbeat
        KWP2000Service keep_alive_service = {0x3E, {0x00}, 0};
        KWP2000Response keep_alive_response;

        size_t packet_len = build_packet_silent(&keep_alive_service);
        ResponseStatus status = read_response_silent(packet_len, &keep_alive_response);

        if (status == RESPONSE_OK) {
            sm->lastHeartbeat = currentTime;

            // Forward ECU ID data to dashboard as heartbeat
            // BufferMessage heartbeatMsg = {
            //     .messageType = MSG_HEARTBEAT,
            //     .length = keep_alive_response.dataSize
            // };
            // memcpy(heartbeatMsg.data, keep_alive_response.data, keep_alive_response.dataSize);
            // ringbuffer_push(sm->txBuffer, &heartbeatMsg);
        } else {
            // Connection lost
            printf("ECU heartbeat failed - connection lost\n");
            sm->connected = false;
            sm->currentState = ECU_STATE_IDLE;
            
            // Notify dashboard of disconnection
            BufferMessage msg = {
                .messageType = MSG_ECU_DISCONNECTED,
                .length = 0
            };
            ringbuffer_push(sm->txBuffer, &msg);
        }
    }
}

void ecu_process_messages(ECUStateMachine* sm) {
    BufferMessage msg;

    while (ringbuffer_pop(sm->rxBuffer, &msg)) {
        switch (msg.messageType) {
            case MSG_CONNECT_ECU:
                if (!sm->connected) {
                    ecu_try_connect(sm);
                }
                break;

            case MSG_COMMAND:
                if (sm->connected) {
                    ecu_send_command(sm, &msg);
                } else {
                    // Send NACK - ECU not connected
                    BufferMessage nack = {
                        .messageType = MSG_NACK,
                        .length = 1,
                        .data = {0xFF} // Error code for ECU not connected
                    };
                    ringbuffer_push(sm->txBuffer, &nack);
                }
                break;

            case MSG_DISCONNECT_ECU:
                if (sm->connected) {
                    printf("Disconnecting from ECU\n");
                    sm->connected = false;
                    sm->currentState = ECU_STATE_IDLE;
                    
                    // Notify dashboard of disconnection
                    BufferMessage ack = {
                        .messageType = MSG_ACK,
                        .length = 0
                    };
                    ringbuffer_push(sm->txBuffer, &ack);
                }
                break;
        }
    }
}

void ecu_update(ECUStateMachine* sm) {
    // Check connection status
    ecu_check_connection(sm);
    
    // Process incoming messages from dashboard
    ecu_process_messages(sm);
    
    // Update state
    switch (sm->currentState) {
        case ECU_STATE_IDLE:
            // Nothing to do in idle state, waiting for connection command
            break;
            
        case ECU_STATE_CONNECTING:
            // Connection attempts handled in ecu_process_messages
            break;
            
        case ECU_STATE_CONNECTED:
            // Normal operation - heartbeats and command processing
            break;
            
        case ECU_STATE_PROCESSING:
            // Will return to CONNECTED when processing complete
            break;
    }
} 