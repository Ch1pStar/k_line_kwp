#include <pico/time.h>
#include <stdint.h>
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

void wakeup_boot_mode() {
    // Initializing with 0x88, lsb first, this is boot mode for the ECU
    // the confirmation byte is 0xcc
    printf("Begin boot mode wakeup - sending 0x88 over 5 baud\n");

    // put line low, start bit
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);

    //  1000 1000, lsb first
    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);

    // pull line high, stop bit
    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(200);
}

void wakeup_kwp2000() {
    // This is KWP2000 or VAG flavor of KWP2000
    printf("Begin KWP2000 wakeup - sending 0x33 over 5 baud\n");

    // put line low, start bit
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);

    //  1100 1100, 0x88, lsb first
    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(400);

    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(400);

    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(400);

    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(400);

    // pull line high, stop bit
    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(200);
}

void wakeup_slow() {
    printf("Waiting 3 seconds before slow wakeup\n");
    sleep_ms(3000);

    printf("Begin 5 baud initializaion\n");
    // line idle is high

    // with 5 baud, 1 bit time is 200ms
    // so we sleep 200ms between each bit
    wakeup_boot_mode();
}

void delay_start() {
    int ch;

    printf("Press 's' to start\n");
    while ((ch = getchar()) != 's') {
        printf("ch: %c\n", ch);
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

    // delay_start();
    wakeup_slow();

    init_pio_tx();

    // 0x55
    uint32_t sync_byte = read_byte();
    printf("Sync byte: %x\n", sync_byte);

    // prob not needed
    sleep_ms(5);

    // 0xef in boot mode, 0x08 in KWP2000
    uint32_t key_byte1 = read_byte();
    printf("Key byte 1: %x\n", key_byte1);

    // 0x8f in boot mode, 0x08 in KWP2000
    uint32_t key_byte2 = read_byte();
    printf("Key byte 2: %x\n", key_byte2);

    sleep_ms(50);

    uint32_t complement = 0xff - key_byte2;
    printf("Complement: %x\n", complement);
    send_byte(complement);

    uint32_t complement_readback = read_byte();
    printf("Complement readback: %x\n", complement_readback);

    if(complement_readback != complement) {
        printf("Got %x response to complement, something went wrong\n", complement_readback);
    }

    // 0xee in boot mode, 0xcc in KWP2000
    uint32_t readAddress = read_byte(); 
    printf("Read address: %x\n", readAddress);

    return readAddress;
}
