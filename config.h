#pragma once

#include <stdio.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"

// K-Line serial stuff
// #define SERIAL_BAUD PICO_DEFAULT_UART_BAUD_RATE
#define SERIAL_BAUD 10400 // as per KWP2000

#define PIO_RX_PIN 18
#define PIO_TX_PIN 15
// #define PIO_RX_PIN 3
// #define PIO_TX_PIN 2

uint32_t read_byte();
void send_byte(uint32_t byte);
void init_pio_rx();
void init_pio_tx();
uint32_t init_comm_protocol();


// KWP2000 stuff
#define MAX_DATA_SIZE 255
#define MAX_RESPONSE_SIZE 80
#define MAX_DTC_COUNT 255

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
    uint8_t highByte;
    uint8_t lowByte;
    uint8_t status;
} DTCData;

void read_ecu_id();
void read_dtcs();
ResponseStatus read_response(size_t commandLength, KWP2000Response* response);
