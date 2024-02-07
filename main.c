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

// #define SERIAL_BAUD PICO_DEFAULT_UART_BAUD_RATE
#define SERIAL_BAUD 10400

#define PIO_RX_PIN 3
#define PIO_TX_PIN 2

void delay_start() {
    int ch;

    while ((ch = getchar()) != 's') {
        continue;
    }

    // for(short i=0;i<4;i++) {
    //     printf("Starting in %d\n", 4-i);
    //     sleep_ms(1000);
    // }

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

void wakeup_slow() {
    printf("Bit toggle\n");
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(400);

    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(400);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(400);

    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(400);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(227);
}


uint32_t init_comm_protocol() {
    // initial state, line is high
    gpio_put(PIO_TX_PIN, 1);
    delay_start();

    // 0xC1 0x33 0xF1 0x81 0x66

    // 0 -> 200ms, 1 -> 400ms, 0 -> 400ms, 1 -> 400ms, 0 -> 400ms, 1 -> 227ms
    wakeup_slow();
    // set tx to low and wait for ecu transmission
    gpio_put(PIO_TX_PIN, 0);

    init_pio_tx();

    uint32_t first_read = uart_rx_program_getc(pio0, 0);
    printf("First %x\n", first_read);

    if(first_read == 0xf7) return first_read;

    uart_tx_program_putc(pio_tx, sm_tx, 0x01);

    uint32_t second_read = uart_rx_program_getc(pio0, 0);
    printf("Second %x\n", second_read);

    uint32_t third_read = uart_rx_program_getc(pio0, 0);
    printf("Third %x\n", third_read);

    uint32_t fourth_read = uart_rx_program_getc(pio0, 0);
    printf("fourth %x\n", fourth_read);

    if(fourth_read == 0x8f) {        
        uint32_t complement = 0xff - fourth_read;

        uart_tx_program_putc(pio_tx, sm_tx, complement);

    }

    return fourth_read;
}

int main() {
    stdio_init_all();
    setup_default_uart();

    init_pio_rx();

    gpio_init(PIO_TX_PIN);
    gpio_set_dir(PIO_TX_PIN, GPIO_OUT);

    while (init_comm_protocol() != 0xef) {
        printf("Failed to init, waiting to try again\n");
    }

    printf("Comms initialization success!\n");

    while (true) {
        uint32_t c = uart_rx_program_getc(pio0, 0);
        printf("%x\n", c);
    }
}
