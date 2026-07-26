#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Wire framing for the RPi 5 link.
//
//   frame = COBS(payload) 0x00
//   payload = [type][seq][data...][crc16:2]
//
// COBS removes 0x00 from the encoded body, so a single zero byte delimits
// frames unambiguously. That is the whole point: a receiver that joins mid
// stream, or loses bytes to noise, only has to scan for the next zero to be
// back in sync - no length field to misread, no escape sequences to get lost
// in. Corruption is caught by the CRC and the frame is dropped.
//
// CRC16-CCITT (poly 0x1021, init 0xFFFF) over [type][seq][data].

#define PROTO_MAX_PAYLOAD 136
#define PROTO_MAX_FRAME   (PROTO_MAX_PAYLOAD + (PROTO_MAX_PAYLOAD / 254) + 2)

typedef enum {
    PROTO_SAMPLE   = 0x10,  // [seq:2 BE][t_ms:4 BE][raw variable bytes]
    PROTO_EVENT    = 0x11,  // link/ECU state change
    PROTO_LOG      = 0x12,  // ASCII log line
    PROTO_RESPONSE = 0x13,  // raw KWP2000 response payload
    PROTO_CMD      = 0x20,  // host -> pico: [CommandId][args]
    PROTO_ACK      = 0x21,  // [status]
} ProtoType;

// PROTO_EVENT payload is [code][detail].
//
// Operation results live here rather than in PROTO_ACK because that frame means
// "your command frame arrived" and carries [CommandId][CommandStatus]. Putting
// both on one type made them genuinely ambiguous - a NACK reads identically to a
// receipt for CommandId 1 - so a host could not sequence a startup reliably.
#define PROTO_EVENT_LINK_LOST        0x01  // detail unused
#define PROTO_EVENT_OPERATION_OK     0x02  // detail = first ACK data byte
#define PROTO_EVENT_OPERATION_FAILED 0x03  // detail = error/status code

typedef struct {
    uint8_t type;
    uint8_t seq;
    const uint8_t *data;
    size_t length;
} ProtoFrame;

typedef struct {
    uint8_t buffer[PROTO_MAX_FRAME];
    uint8_t decoded[PROTO_MAX_PAYLOAD];
    size_t length;
    bool overflow;          // current frame already too long; drop to next zero
    uint32_t crc_errors;
    uint32_t overflow_errors;
    uint32_t short_frames;
} ProtoDecoder;

uint16_t proto_crc16(const uint8_t *data, size_t length);

// Encode one frame into `out`. Returns bytes written, or 0 if it would not fit.
size_t proto_encode(uint8_t type, uint8_t seq, const uint8_t *data, size_t length,
                    uint8_t *out, size_t out_size);

void proto_decoder_reset(ProtoDecoder *decoder);

// Feed one received byte. Returns true when `frame` has been filled with a
// complete, CRC-checked frame; `frame->data` points into the decoder.
bool proto_decode_byte(ProtoDecoder *decoder, uint8_t byte, ProtoFrame *frame);

// Round-trip, corruption and resynchronisation checks. Returns the number of
// failures; 0 means everything passed. Results are logged line by line.
unsigned proto_selftest(void);
