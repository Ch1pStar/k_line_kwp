#include "config.h"

int main() {
    stdio_init_all();
    setup_default_uart();

    init_pio_rx();

    gpio_init(PIO_TX_PIN);
    gpio_set_dir(PIO_TX_PIN, GPIO_OUT);

    while (init_comm_protocol() != 0xee) {
        printf("Failed to init, waiting to try again\n");
    }
    printf("Comms initialization success!\n");

    read_ecu_id();
    // clear_dtcs();
    sleep_ms(500);
    read_dtcs();

    // KWP2000Service init_diag_service = {0x10, {0x86, 0x64}, 2}; // Init diag session
    // build_packet(&init_diag_service);
    // printf("------------------------\n");
}
