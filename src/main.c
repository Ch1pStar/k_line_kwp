#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"

#include "dashboard.h"
#include "ecu_state_machine.h"
#include "ring_buffer.h"

void core1_entry(void);

// Shared ring buffers for inter-core communication
static RingBuffer dash_to_ecu_buffer;
static RingBuffer ecu_to_dash_buffer;

// State machines
static Dashboard dashboard;
static ECUStateMachine ecu_sm;

int main() {
    stdio_init_all();
    setup_default_uart();

    ringbuffer_init(&dash_to_ecu_buffer);
    ringbuffer_init(&ecu_to_dash_buffer);

    ecu_init(&ecu_sm, &dash_to_ecu_buffer, &ecu_to_dash_buffer);

    multicore_launch_core1(core1_entry);

    // ECU communication runs on core 0
    while (true) {
        ecu_update(&ecu_sm);
        sleep_ms(1);
    }
}

void core1_entry(void) {
    dashboard_init(&dashboard, &dash_to_ecu_buffer, &ecu_to_dash_buffer);

    while (true) {
        dashboard_update(&dashboard);
        sleep_ms(1);
    }
}
