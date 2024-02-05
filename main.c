/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"

#include "uart_rx.pio.h"

#define SERIAL_BAUD PICO_DEFAULT_UART_BAUD_RATE
#define HARD_UART_INST uart1

#define HARD_UART_TX_PIN 4
#define PIO_RX_PIN 3

const char *text = "Hello, world from PIO! (Plus 2 UARTs and 2 cores, for complex reasons)\n";

// Ask core 1 to print a string, to make things easier on core 0
void core1_main() {
    const char *s = (const char *) multicore_fifo_pop_blocking();

    uart_puts(HARD_UART_INST, s);
}

int main() {
    stdio_init_all();

    setup_default_uart();

    uart_init(HARD_UART_INST, SERIAL_BAUD);
    gpio_set_function(HARD_UART_TX_PIN, GPIO_FUNC_UART);

    // Set up the state machine we're going to use to receive them.
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &uart_rx_program);
    uart_rx_program_init(pio, sm, offset, PIO_RX_PIN, SERIAL_BAUD);

    for(short i=0;i<8;i++) {
        printf("Starting in %d\n", 8-i);
        sleep_ms(1000);
    }

    // Tell core 1 to print some text to uart1 as fast as it can
    multicore_launch_core1(core1_main);
    multicore_fifo_push_blocking((uint32_t) text);

    // Echo characters received from PIO to the console
    while (true) {
        char c = uart_rx_program_getc(pio, sm);
        putchar(c);
    }
}
