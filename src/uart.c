#include "uart.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "uart_rx.pio.h"

static PIO pio_tx = pio1;
static uint sm_tx = 1;
static PIO pio_rx = pio0;
static uint sm_rx = 0;

static uint32_t current_baud = SERIAL_BAUD;

// pio_add_program panics once the PIO's 32 instruction slots fill up, and TX is
// re-initialised on every connect. Load each program once and only re-init the
// state machine afterwards.
static bool tx_loaded = false;
static uint tx_offset = 0;
static bool rx_loaded = false;
static uint rx_offset = 0;

void uart_pio_init_tx(void) {
    if (!tx_loaded) {
        tx_offset = pio_add_program(pio_tx, &uart_tx_program);
        tx_loaded = true;
    }
    uart_tx_program_init(pio_tx, sm_tx, tx_offset, PIO_TX_PIN, current_baud);
}

void uart_pio_init_rx(void) {
    if (!rx_loaded) {
        rx_offset = pio_add_program(pio_rx, &uart_rx_program);
        rx_loaded = true;
    }
    uart_rx_program_init(pio_rx, sm_rx, rx_offset, PIO_RX_PIN, current_baud);
}

uint32_t uart_get_baud(void) {
    return current_baud;
}

// Re-run the full init path at the new rate rather than only nudging the clock
// divider - this reuses the exact, working configuration each SM starts with and
// leaves no half-updated state on a mid-session switch.
void uart_set_baud(uint32_t baud) {
    current_baud = baud;
    if (rx_loaded) uart_pio_init_rx();
    if (tx_loaded) uart_pio_init_tx();
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
