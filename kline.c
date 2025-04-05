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

void wakeup_programming_mode() {
    // Initializing with 0x88, lsb first, this is a special programming mode for the ECU
    // most likely a variant of KWP2000 specific for Bosch ME7.5 or similar ECUs
    // the confirmation byte is 0xcc
    printf("Begin programming mode wakeup - sending 0x88 over 5 baud\n");

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
    printf("Begin KWP2000 wakeup - sending 0x33 at 5 baud\n");

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
    // initial state, line is high
    gpio_put(PIO_TX_PIN, 1);
    // per kawp2000 spec, wait 300ms while the line is idle(high)
    uint16_t w5 = 1500; // to be on the safe side as sometimes 300ms is not enough
    sleep_ms(w5);

    printf("Begin 5 baud initializaion address transmission\n");

    // with 5 baud, 1 bit time is 200ms
    // so we sleep 200ms between each bit
    wakeup_programming_mode();
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

    // delay_start();
    wakeup_slow();

    init_pio_tx();

    uint8_t w1 = 60;

    // per kwp2000 spec, sync byte should be sent between 60 and 300ms after wakeup
    sleep_ms(w1);

    // 0x55
    uint32_t sync_byte = read_byte();
    printf("Sync byte: %x\n", sync_byte);

    uint8_t w2 = 5;
    // key byte 1 should be sent by the ECU between 5 and 20ms after sync byte
    // key byte 2 should be sent instantly after key byte 1
    sleep_ms(w2);

    // key bytes
    // - 0xef 0x8f in programming mode
    // - 0x08 0x08 in KWP2000
    uint32_t key_byte1 = read_byte();
    printf("Key byte 1: %x\n", key_byte1);
    uint32_t key_byte2 = read_byte();
    printf("Key byte 2: %x\n", key_byte2);

    uint8_t w4 = 25;
    // per kwp2000 spec, complement byte response window is 25-50ms after key byte 2
    // but it doesn't seem to be very strict
    sleep_ms(w4);

    uint32_t complement = 0xff - key_byte2;
    printf("Complement: %x\n", complement);
    send_byte(complement);

    // complement readback is instantly after complement byte
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
