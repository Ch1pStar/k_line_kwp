#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"

#include "host.h"
#include "ecu_state_machine.h"
#include "ring_buffer.h"
#include "log.h"

void core1_entry(void);

// Shared ring buffers for inter-core communication
static RingBuffer host_to_ecu_buffer;
static RingBuffer ecu_to_host_buffer;

static ECUStateMachine ecu_sm;

int main() {
    stdio_init_all();
    // No setup_default_uart(): with stdio-over-UART disabled it only did a
    // legacy uart0 init at 115200 on GP0/GP1. host_link_init() configures that
    // same peripheral for the RPi 5 link instead.

    ringbuffer_init(&host_to_ecu_buffer);
    ringbuffer_init(&ecu_to_host_buffer);

    // Core 0 logs through the same queue it sends responses on, so anything it
    // prints before core 1 starts is simply queued and shown once core 1 runs.
    klog_init(&ecu_to_host_buffer);

    ecu_init(&ecu_sm, &host_to_ecu_buffer, &ecu_to_host_buffer);

    multicore_launch_core1(core1_entry);

    // ECU communication runs on core 0
    while (true) {
        ecu_update(&ecu_sm);
        sleep_ms(1);
    }
}

void core1_entry(void) {
    host_init(&host_to_ecu_buffer, &ecu_to_host_buffer);

    while (true) {
        host_update();
        sleep_ms(1);
    }
}
