#include "uart.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "uart_rx.pio.h"

static PIO pio_tx = pio1;
static uint sm_tx = 1;

void uart_pio_init_tx(void) {
    uint offset_tx = pio_add_program(pio_tx, &uart_tx_program);
    uart_tx_program_init(pio_tx, sm_tx, offset_tx, PIO_TX_PIN, SERIAL_BAUD);
}

void uart_pio_init_rx(void) {
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &uart_rx_program);
    uart_rx_program_init(pio, sm, offset, PIO_RX_PIN, SERIAL_BAUD);
}

uint32_t uart_read_byte_timeout(uint32_t timeout_us) {
    absolute_time_t timeout_time = make_timeout_time_us(timeout_us);

    while (pio_sm_is_rx_fifo_empty(pio0, 0)) {
        if (time_reached(timeout_time)) {
            return UART_TIMEOUT;
        }
        tight_loop_contents();
    }

    return uart_rx_program_getc(pio0, 0);
}

uint32_t uart_read_byte(void) {
    return uart_read_byte_timeout(500000); // 500ms default
}

void uart_send_byte(uint32_t byte) {
    uart_tx_program_putc(pio_tx, sm_tx, byte);
}
