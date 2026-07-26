#pragma once

#include <stdint.h>

// Pin configuration
#define PIO_RX_PIN 18
#define PIO_TX_PIN 15

// K-Line baud rate as per KWP2000
#define SERIAL_BAUD 10400

// Timeout indicator — returned when read_byte times out
#define UART_TIMEOUT UINT32_MAX

// Initialize PIO-based UART TX
void uart_pio_init_tx(void);

// Initialize PIO-based UART RX
void uart_pio_init_rx(void);

// Read a byte with specified timeout in microseconds.
// Returns UART_TIMEOUT on timeout.
uint32_t uart_read_byte_timeout(uint32_t timeout_us);

// Read a byte with default 500ms timeout.
// Returns UART_TIMEOUT on timeout.
uint32_t uart_read_byte(void);

// Send a single byte
void uart_send_byte(uint32_t byte);

// Change the K-line bit rate at runtime. The ECU can be asked to switch to a
// faster rate mid-session (StartDiagnosticSession with a baud identifier), and
// both PIO state machines have to follow it.
void uart_set_baud(uint32_t baud);

// Currently configured K-line bit rate.
uint32_t uart_get_baud(void);
