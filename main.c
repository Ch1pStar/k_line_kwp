#include "config.h"
#include <pico/time.h>
#include "dashboard_state_machine.h"
#include "ecu_state_machine.h"
#include "ring_buffer.h"

// Function prototypes
void core1_entry();

// Shared ring buffers for inter-core communication
static RingBuffer dash_to_ecu_buffer;
static RingBuffer ecu_to_dash_buffer;

// State machines
static DashboardStateMachine dash_sm;
static ECUStateMachine ecu_sm;

int main() {
    // Initialize stdio and hardware
    stdio_init_all();
    setup_default_uart();
    
    // Initialize ring buffers
    ringbuffer_init(&dash_to_ecu_buffer);
    ringbuffer_init(&ecu_to_dash_buffer);
    
    // Initialize ECU state machine
    ecu_init(&ecu_sm, &dash_to_ecu_buffer, &ecu_to_dash_buffer);
    
    // Launch dashboard core
    multicore_launch_core1(core1_entry);
    
    // ECU communication runs on core 0
    while (true) {
        ecu_update(&ecu_sm);
        sleep_ms(1);  // Prevent tight loop
    }
}

// Core 1 entry point - handles dashboard communication
void core1_entry() {
    // Dashboard communication runs on core 1
    dashboard_init(&dash_sm, &dash_to_ecu_buffer, &ecu_to_dash_buffer);
    
    while (true) {
        dashboard_update(&dash_sm);
        sleep_ms(1);  // Prevent tight loop
    }
}
