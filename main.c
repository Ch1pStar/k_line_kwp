/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"

#include "uart_rx.pio.h"

// #define SERIAL_BAUD PICO_DEFAULT_UART_BAUD_RATE
#define SERIAL_BAUD 10400 // as per KWP2000

#define PIO_RX_PIN 3
#define PIO_TX_PIN 2

PIO pio_tx = pio1;
uint sm_tx = 1;

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

#define MAX_DATA_SIZE 255
#define MAX_RESPONSE_SIZE 80

typedef struct {
    uint8_t serviceId;
    uint8_t dataBytes[MAX_DATA_SIZE];
    size_t dataLength;
} KWP2000Service;

typedef struct {
    uint8_t length;
    uint8_t serviceId;
    uint8_t dataBytes[MAX_DATA_SIZE];
    uint8_t checksum;
} KWP2000Packet;


typedef struct {
    uint8_t data[MAX_RESPONSE_SIZE];
    size_t dataSize;
} KWP2000Response;

typedef enum {
    RESPONSE_OK,
    RESPONSE_ERROR,
    RESPONSE_CHECKSUM_INVALID,
    RESPONSE_OVERFLOW // When received data exceeds MAX_RESPONSE_SIZE
} ResponseStatus;


uint8_t calculate_checksum(const KWP2000Service* service) {
    uint8_t csum = 0;
    csum += (1 + service->dataLength);
    csum += service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        csum += service->dataBytes[i];
    }

    return csum;
}

ResponseStatus read_response(size_t commandLength, KWP2000Response* response) {
    response->dataSize = 0;

    // Skip echoed command bytes
    for (size_t i = 0; i < commandLength; ++i) {
        read_byte();
    }

    uint8_t checksum = 0;
    uint8_t responseLength = read_byte();
    checksum += responseLength;

    uint8_t responseStatus = read_byte();
    checksum += responseStatus;

    if (responseStatus == 0x7f) {
        printf("Error in response\n");
        return RESPONSE_ERROR;
    }

    // Read the data bytes
    for (size_t i = 0; i < responseLength - 1; ++i) {
        if (i < MAX_RESPONSE_SIZE) {
            uint8_t dataByte = read_byte();
            checksum += dataByte;
            response->data[i] = dataByte;
            response->dataSize++;
        } else {
            // Exceeds buffer, read remaining to maintain protocol but do not store
            read_byte();
            return RESPONSE_OVERFLOW;
        }
    }

    //read and validate the checksum byte
    uint8_t receivedChecksum = read_byte();
    if (checksum == receivedChecksum) {
        printf("Response checksum valid.\n");
        return RESPONSE_OK;
    } else {
        printf("Response checksum invalid. Calculated: %02x, Received: %02x\n", checksum, receivedChecksum);
        return RESPONSE_CHECKSUM_INVALID;
    }
}

void send_packet(const KWP2000Packet* packet) {
    send_byte(packet->length);
    send_byte(packet->serviceId);

    for (size_t i = 0; i < packet->length - 1; ++i) {
        send_byte(packet->dataBytes[i]);
    }

    send_byte(packet->checksum);
}

void print_response(const KWP2000Response* response) {
    printf("Response Data (size: %zu): ", response->dataSize);
    for (size_t i = 0; i < response->dataSize; ++i) {
        printf("%02X ", response->data[i]);
    }
    printf("\n");
}

void print_str_response(const KWP2000Response* response) {
    printf("Response String: \"");
    for (size_t i = 0; i < response->dataSize; ++i) {
        if (isprint(response->data[i])) {
            putchar(response->data[i]);
        } else {
            putchar('.'); // Replace non-printable characters with '.'
        }
    }
    printf("\"\n");
}

void build_packet(const KWP2000Service* service) {
    KWP2000Packet packet;

    packet.length = 1 + service->dataLength; // Only service ID + data bytes, without checksum
    packet.serviceId = service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        packet.dataBytes[i] = service->dataBytes[i];
    }
    packet.checksum = calculate_checksum(service);

    // Display the packet
    printf("KWP2000 Packet: Length=0x%02X, Service ID=0x%02X, Data=", packet.length, packet.serviceId);
    for (size_t i = 0; i < service->dataLength; ++i) {
        printf("0x%02X ", packet.dataBytes[i]);
    }
    printf("Checksum=0x%02X\n", packet.checksum);

    send_packet(&packet);

    KWP2000Response response;

    read_response(packet.length+2, &response);
    print_response(&response);
    print_str_response(&response);
}


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

    KWP2000Service ecu_id_service = {0x1A, {0x9B}, 1}; // ECU ID
    build_packet(&ecu_id_service);
    printf("------------------------\n");

    // KWP2000Service init_diag_service = {0x10, {0x86, 0x64}, 2}; // Init diag session
    // build_packet(&init_diag_service);
    // printf("------------------------\n");
}
