#include <stdio.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"

#include "uart_rx.pio.h"
#include "config.h"

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

uint32_t read_byte() {
    uint32_t c = uart_rx_program_getc(pio0, 0);
    // printf("Read: %x\n", c);

    return c;
}

void send_byte(uint32_t byte) {
    uart_tx_program_putc(pio_tx, sm_tx, byte);
    // printf("Sent: %x\n", byte);
}

void wakeup_slow() {
    printf("Begin slow initializaion\n");
    // break on - 0
    // break off - 1

    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(200);

    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(600);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(200);

    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(600);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(227);
}

void empty_reads() {
    while(read_byte() != 0xef) {
        continue;
    }
}

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

uint32_t init_comm_protocol() {
    // initial state, line is high
    gpio_put(PIO_TX_PIN, 1);
    delay_start();

    wakeup_slow();
    // set tx to low and wait for ecu transmission
    gpio_put(PIO_TX_PIN, 0);

    init_pio_tx();

    empty_reads();

    // this should be 0x8F
    uint32_t confirmationByte = read_byte();

    // this should be 0x70
    // this is the ECU address, idk what its used for
    uint32_t complement = 0xff - confirmationByte;

    printf("Sending complement %x, to %x\n", complement, confirmationByte);
    sleep_ms(50);
    send_byte(complement);

    uint32_t complement_readback = read_byte();

    if(complement_readback != complement) {
        printf("Got %x response to complement, something went wrong\n", complement_readback);
    }

    uint32_t ecu_address = read_byte();

    if(ecu_address != 0xee) {
        printf("Got %x ECU address, expecting 0xEE\n", ecu_address);
    }

    return ecu_address;
}
