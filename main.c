#include "config.h"
#include "state_machine.h"
#include <pico/time.h>

// Function prototypes
void core1_entry();
bool try_connect_ecu();
void handle_ecu_communication();
void handle_rpi5_communication();
void start_ecu_communication_loop();

// Global state machine
StateMachine g_state_machine;

int main() {
    // Initialize stdio for USB communication with RPI5
    stdio_init_all();
    setup_default_uart();

    // Initialize state machine
    initStateMachine(&g_state_machine);
    // Launch core1 for handling RPI5 communication
    multicore_launch_core1(core1_entry);
    // Start ECU communication loop
    start_ecu_communication_loop();
}

void start_ecu_communication_loop() {
    // Initialize K-LINE communication
    init_pio_rx();
    gpio_init(PIO_TX_PIN);
    gpio_set_dir(PIO_TX_PIN, GPIO_OUT);

    // Main loop on core0 - handles ECU communication
    while (true) {
        switch (g_state_machine.currentState) {
            case STATE_ECU_CONNECTED:
            case STATE_FULLY_OPERATIONAL:
                handle_ecu_communication();
                break;

            default:
                sleep_ms(100);
                break;
        }

        updateState(&g_state_machine);
    }
}

// Core 1 entry point - handles RPI5 communication
void core1_entry() {
    while (true) {
        // Always try to handle RPI5 communication regardless of state
        handle_rpi5_communication();
        
        // Update state machine
        updateState(&g_state_machine);
        
        sleep_ms(10); // Small delay to prevent tight loop
    }
}

bool try_connect_ecu() {
    return (init_comm_protocol() == 0xee);
}

void handle_ecu_communication() {
    static uint32_t last_heartbeat = 0;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());

    // Send periodic heartbeat to ECU (read ECU ID)
    // if (current_time - last_heartbeat >= 2000) {
    //     KWP2000Service ecu_id_service = {0x1A, {0x9B}, 1};
    //     KWP2000Response response;
        
    //     size_t packet_len = build_packet(&ecu_id_service);
    //     ResponseStatus status = read_response(packet_len, &response);
        
    //     if (status == RESPONSE_OK) {
    //         g_state_machine.lastEcuHeartbeat = current_time;
    //         last_heartbeat = current_time;
            
    //         // Forward the data to RPI5 if it's connected
    //         if (g_state_machine.rpi5Connected) {
    //             handleEcuData(&response);
    //         }else{
    //             print_str_response(&response);
    //         }
    //     }
    // }
}

void handle_rpi5_communication() {
    SerialFrame frame;
    
    // Try to receive a frame from RPI5 or debug command
    if (receiveFrameFromRpi5(&frame) || receiveDebugCommand(&frame)) {
        // Update RPI5 connection status
        if (!g_state_machine.rpi5Connected) {
            printf("RPI5/Debug connected!\n");
            g_state_machine.currentState = STATE_RPI5_CONNECTED;
        }
        g_state_machine.rpi5Connected = true;
        g_state_machine.lastRpi5Heartbeat = to_ms_since_boot(get_absolute_time());
        
        switch (frame.messageType) {
            case MSG_HEARTBEAT:
                // Send ACK
                {
                    SerialFrame ack = {
                        .messageType = MSG_ACK,
                        .length = 0
                    };
                    sendFrameToRpi5(&ack);
                }
                break;
                
            case MSG_CONNECT_ECU:
                printf("Got ECU connect command\n");
                printf("Is ECU connected: %d\n", g_state_machine.ecuConnected);
                if (!g_state_machine.ecuConnected) {
                    printf("Attempting to connect to ECU\n");
                    if (try_connect_ecu()) {
                        g_state_machine.ecuConnected = true;
                        g_state_machine.lastEcuHeartbeat = to_ms_since_boot(get_absolute_time());
                        printf("ECU connected successfully!\n");
                        printf("ecuConnected: %d\n", g_state_machine.ecuConnected);
                        g_state_machine.currentState = STATE_FULLY_OPERATIONAL;
                        
                        SerialFrame ack = {
                            .messageType = MSG_ACK,
                            .length = 0
                        };
                        sendFrameToRpi5(&ack);

                        sleep_ms(500);
                        read_ecu_id();
                        sleep_ms(500);
                        read_dtcs();
                    } else {
                        printf("Failed to connect to ECU\n");
                        SerialFrame nack = {
                            .messageType = MSG_NACK,
                            .length = 1,
                            .payload = {0xFF}
                        };
                        sendFrameToRpi5(&nack);
                    }
                }
                break;
                
            case MSG_COMMAND:
                printf("ecuConnected: %d\n", g_state_machine.ecuConnected);
                if (g_state_machine.ecuConnected) {
                    handleRpi5Command(&frame);
                } else {
                    // Send NACK - ECU not connected
                    SerialFrame nack = {
                        .messageType = MSG_NACK,
                        .length = 1,
                        .payload = {0xFF} // Error code for ECU not connected
                    };
                    sendFrameToRpi5(&nack);
                    printf("Debug: ECU not connected, command rejected\n");
                }
                break;
        }
    }
}
