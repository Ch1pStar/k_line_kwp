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

#define HARD_UART_RX_PIN 5
#define HARD_UART_TX_PIN 4

#define PIO_RX_PIN 3
#define PIO_TX_PIN 2


#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY    UART_PARITY_NONE

const char *text = "Hello, world from PIO! (Plus 2 UARTs and 2 cores, for complex reasons)\n";
static int chars_rxed = 0;

void delay_start() {
    // for(short i=0;i<15;i++) {
    //     printf("Starting in %d\n", 15-i);
    //     sleep_ms(1000);
    // }
    int ch;

    while ((ch = getchar()) != 's') {
        continue;
    }
}

PIO pio_tx = pio1;
uint sm_tx = 1;

void init_pio_tx() {
    uint offset_tx = pio_add_program(pio_tx, &uart_tx_program);

    uart_tx_program_init(pio_tx, sm_tx, offset_tx, PIO_TX_PIN, SERIAL_BAUD);
}

void init_pio_rx() {
    // Set up the state machine we're going to use to receive them.
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &uart_rx_program);

    uart_rx_program_init(pio, sm, offset, PIO_RX_PIN, SERIAL_BAUD);
}

// RX interrupt handler
void on_uart_rx() {
    while (uart_is_readable(HARD_UART_INST)) {
        uint8_t ch = uart_getc(HARD_UART_INST);
        // Can we send it back?
        // if (uart_is_writable(HARD_UART_INST)) {
        //     // Change it slightly first!
        //     ch++;
        //     uart_putc(HARD_UART_INST, ch);
        // }

        putchar(ch);
        chars_rxed++;
    }
}

void init_irq_rx() {
    // Set UART flow control CTS/RTS, we don't want these, so turn them off
    uart_set_hw_flow(HARD_UART_INST, false, false);

    // Set our data format
    uart_set_format(HARD_UART_INST, DATA_BITS, STOP_BITS, PARITY);

    // Turn off FIFO's - we want to do this character by character
    uart_set_fifo_enabled(HARD_UART_INST, false);

    // Set up a RX interrupt
    // We need to set up the handler first
    // Select correct interrupt for the UART we are using
    int UART_IRQ = HARD_UART_INST == uart0 ? UART0_IRQ : UART1_IRQ;

    // And set up and enable the interrupt handlers
    irq_set_exclusive_handler(UART_IRQ, on_uart_rx);
    irq_set_enabled(UART_IRQ, true);

    // Now enable the UART to send interrupts - RX only
    uart_set_irq_enables(HARD_UART_INST, true, false);
}

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
    gpio_set_function(HARD_UART_RX_PIN, GPIO_FUNC_UART);

    init_pio_rx();
    init_irq_rx();
    init_pio_tx();

    // Tell core 1 to print some text to uart1 as fast as it can
    multicore_launch_core1(core1_main);

    delay_start();

    const char *kek = "KEK\n";
    uart_tx_program_puts(pio_tx, sm_tx, kek);

    multicore_fifo_push_blocking((uint32_t) text);

    // Echo characters received from PIO to the console
    while (true) {
        char c = uart_rx_program_getc(pio0, 0);
        putchar(c);
    }
}
