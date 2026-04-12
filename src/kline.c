#include "kline.h"
#include "uart.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"

static void wakeup_programming_mode(void) {
    // Send 0x88 at 5 baud (200ms per bit), LSB first
    // Programming mode wakeup for Bosch ME7.5 or similar ECUs
    printf("Begin programming mode wakeup - sending 0x88 over 5 baud\n");

    // Start bit (low)
    gpio_put(PIO_TX_PIN, 0);
    sleep_ms(200);

    // 0x88 = 1000 1000, LSB first
    gpio_put(PIO_TX_PIN, 1); sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0); sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0); sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0); sleep_ms(200);

    gpio_put(PIO_TX_PIN, 1); sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0); sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0); sleep_ms(200);
    gpio_put(PIO_TX_PIN, 0); sleep_ms(200);

    // Stop bit (high)
    gpio_put(PIO_TX_PIN, 1);
    sleep_ms(200);
}

static void wakeup_slow(void) {
    // Line idle state is high
    gpio_put(PIO_TX_PIN, 1);
    // Per KWP2000 spec, wait 300ms+ while line is idle
    sleep_ms(1500); // Conservative wait

    printf("Begin 5 baud initialization address transmission\n");
    wakeup_programming_mode();
}

uint32_t kline_init_connection(void) {
    wakeup_slow();

    // Initialize PIO TX after wakeup (wakeup uses raw GPIO)
    uart_pio_init_tx();

    // Per KWP2000 spec, sync byte arrives 60-300ms after wakeup
    sleep_ms(60);

    // Wait for sync byte (0x55)
    uint32_t sync_byte = uart_read_byte_timeout(300000);
    if (sync_byte == UART_TIMEOUT) {
        printf("Timeout waiting for sync byte\n");
        return UART_TIMEOUT;
    }
    printf("Sync byte: %x\n", sync_byte);

    // Key bytes arrive 5-20ms after sync
    sleep_ms(5);

    uint32_t key_byte1 = uart_read_byte_timeout(20000);
    if (key_byte1 == UART_TIMEOUT) {
        printf("Timeout waiting for key byte 1\n");
        return UART_TIMEOUT;
    }
    printf("Key byte 1: %x\n", key_byte1);

    uint32_t key_byte2 = uart_read_byte_timeout(20000);
    if (key_byte2 == UART_TIMEOUT) {
        printf("Timeout waiting for key byte 2\n");
        return UART_TIMEOUT;
    }
    printf("Key byte 2: %x\n", key_byte2);

    // Complement response window is 25-50ms after key byte 2
    sleep_ms(25);

    uint32_t complement = 0xff - key_byte2;
    printf("Sending complement: %x\n", complement);
    uart_send_byte(complement);

    // Wait for complement readback
    uint32_t complement_readback = uart_read_byte_timeout(50000);
    if (complement_readback == UART_TIMEOUT) {
        printf("Timeout waiting for complement readback\n");
        return UART_TIMEOUT;
    }
    printf("Complement readback: %x\n", complement_readback);

    if (complement_readback != complement) {
        printf("Got %x response to complement, something went wrong\n", complement_readback);
        return UART_TIMEOUT;
    }

    // Final address byte: 0xee = programming mode, 0xcc = KWP2000
    uint32_t address = uart_read_byte_timeout(50000);
    if (address == UART_TIMEOUT) {
        printf("Timeout waiting for address byte\n");
        return UART_TIMEOUT;
    }
    printf("Read address: %x\n", address);

    return address;
}
