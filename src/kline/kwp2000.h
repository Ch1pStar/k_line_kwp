#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_DATA_SIZE 255
#define MAX_RESPONSE_SIZE 80
#define MAX_DTC_COUNT 255

// ReadDTCByStatus request service id (positive response is 0x58)
#define KWP_SID_READ_DTC_BY_STATUS 0x18

// A DTC response is [count] + 3 bytes per DTC, so a full response
// buffer caps how many can arrive in a single reply.
#define MAX_DTCS_PER_RESPONSE ((MAX_RESPONSE_SIZE - 1) / 3)

typedef enum {
    RESPONSE_OK,
    RESPONSE_ERROR,              // no/!malformed reply - link problem
    RESPONSE_NEGATIVE,           // ECU replied 0x7F - rejected, but alive
    RESPONSE_CHECKSUM_INVALID,
    RESPONSE_OVERFLOW
} ResponseStatus;

typedef struct {
    // Response service id (request SID + 0x40, or 0x7F when rejected). Not
    // included in data[], which holds only what follows it.
    uint8_t serviceId;
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

// Build and send a KWP2000 packet over UART. The half-duplex echo is read back
// and verified byte-by-byte during the send, so nothing is left on the wire for
// the caller to skip. Returns the number of bytes sent (for logging).
// If silent is true, suppresses debug output.
size_t kwp2000_send(const KWP2000Service *service, bool silent);

// Read a KWP2000 response, first discarding `echo_bytes` bytes. Pass 0 after
// kwp2000_send, which already consumed the echo.
// If silent is true, suppresses debug output.
ResponseStatus kwp2000_read_response(size_t echo_bytes, KWP2000Response *response, bool silent);

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
