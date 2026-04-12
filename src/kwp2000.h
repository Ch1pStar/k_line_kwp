#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_DATA_SIZE 255
#define MAX_RESPONSE_SIZE 80
#define MAX_DTC_COUNT 255

typedef enum {
    RESPONSE_OK,
    RESPONSE_ERROR,
    RESPONSE_CHECKSUM_INVALID,
    RESPONSE_OVERFLOW
} ResponseStatus;

typedef struct {
    uint8_t data[MAX_RESPONSE_SIZE];
    size_t dataSize;
} KWP2000Response;

typedef struct {
    uint8_t serviceId;
    uint8_t dataBytes[MAX_DATA_SIZE];
    size_t dataLength;
} KWP2000Service;

typedef struct {
    uint8_t highByte;
    uint8_t lowByte;
    uint8_t status;
} DTCData;

// Build and send a KWP2000 packet over UART.
// If silent is true, suppresses debug output.
// Returns the total number of bytes sent (for use with kwp2000_read_response).
size_t kwp2000_send(const KWP2000Service *service, bool silent);

// Read a KWP2000 response, skipping `command_length` echoed bytes.
// If silent is true, suppresses debug output.
ResponseStatus kwp2000_read_response(size_t command_length, KWP2000Response *response, bool silent);

// Convenience: send + read response in one call.
ResponseStatus kwp2000_execute(const KWP2000Service *service, KWP2000Response *response, bool silent);

// Print response as hex dump
void kwp2000_print_response(const KWP2000Response *response);

// Print response as ASCII string
void kwp2000_print_str_response(const KWP2000Response *response);

// Parse DTC response into array, returns number of DTCs found
uint8_t kwp2000_parse_dtcs(const KWP2000Response *response, DTCData *dtc_array, size_t dtc_array_size);

// Print DTCs in human-readable format
void kwp2000_print_dtcs(DTCData *dtcs, size_t num_dtcs);
