#include "proto.h"

#include <stdio.h>
#include <string.h>

uint16_t proto_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// Standard COBS. `out` needs length + length/254 + 1 bytes.
static size_t cobs_encode(const uint8_t *in, size_t length, uint8_t *out) {
    uint8_t *encode = out;
    uint8_t *code_position = encode++;
    uint8_t code = 1;

    for (size_t i = 0; i < length; i++) {
        if (in[i] != 0) {
            *encode++ = in[i];
            code++;
        }
        if (in[i] == 0 || code == 0xFF) {
            *code_position = code;
            code = 1;
            code_position = encode;
            if (in[i] == 0 || i + 1 < length) {
                encode++;
            }
        }
    }
    *code_position = code;
    return (size_t)(encode - out);
}

// Returns decoded length, or 0 if the block is malformed.
static size_t cobs_decode(const uint8_t *in, size_t length, uint8_t *out, size_t out_size) {
    size_t read = 0;
    size_t written = 0;
    uint8_t code = 0xFF;
    uint8_t block = 0;

    while (read < length) {
        if (block != 0) {
            if (written >= out_size) return 0;
            out[written++] = in[read++];
        } else {
            block = in[read++];
            if (block == 0) return 0;  // a zero code byte cannot occur in COBS
            if (code != 0xFF) {
                if (written >= out_size) return 0;
                out[written++] = 0;
            }
            code = block;
        }
        block--;
    }

    return written;
}

size_t proto_encode(uint8_t type, uint8_t seq, const uint8_t *data, size_t length,
                    uint8_t *out, size_t out_size) {
    const size_t payload_length = 2 + length + 2;
    if (payload_length > PROTO_MAX_PAYLOAD) return 0;

    uint8_t payload[PROTO_MAX_PAYLOAD];
    payload[0] = type;
    payload[1] = seq;
    if (length > 0) memcpy(&payload[2], data, length);

    const uint16_t crc = proto_crc16(payload, 2 + length);
    payload[2 + length] = (crc >> 8) & 0xFF;
    payload[2 + length + 1] = crc & 0xFF;

    // Worst case COBS growth plus the delimiter.
    if (out_size < payload_length + (payload_length / 254) + 2) return 0;

    const size_t encoded = cobs_encode(payload, payload_length, out);
    out[encoded] = 0x00;
    return encoded + 1;
}

void proto_decoder_reset(ProtoDecoder *decoder) {
    decoder->length = 0;
    decoder->overflow = false;
}

bool proto_decode_byte(ProtoDecoder *decoder, uint8_t byte, ProtoFrame *frame) {
    if (byte != 0x00) {
        if (decoder->length < sizeof(decoder->buffer)) {
            decoder->buffer[decoder->length++] = byte;
        } else if (!decoder->overflow) {
            // Runaway frame: stop storing but keep consuming until the next
            // delimiter, which is where synchronisation is regained.
            decoder->overflow = true;
            decoder->overflow_errors++;
        }
        return false;
    }

    // Delimiter: whatever we have is one frame's worth, good or bad.
    const size_t raw_length = decoder->length;
    const bool was_overflow = decoder->overflow;
    proto_decoder_reset(decoder);

    if (was_overflow || raw_length == 0) return false;

    const size_t payload_length =
        cobs_decode(decoder->buffer, raw_length, decoder->decoded, sizeof(decoder->decoded));

    // Need at least type, seq and the CRC.
    if (payload_length < 4) {
        decoder->short_frames++;
        return false;
    }

    const size_t body_length = payload_length - 2;
    const uint16_t received_crc = ((uint16_t)decoder->decoded[body_length] << 8) |
                                  decoder->decoded[body_length + 1];
    if (received_crc != proto_crc16(decoder->decoded, body_length)) {
        decoder->crc_errors++;
        return false;
    }

    frame->type = decoder->decoded[0];
    frame->seq = decoder->decoded[1];
    frame->data = &decoder->decoded[2];
    frame->length = body_length - 2;
    return true;
}

// --- self-test ------------------------------------------------------------
// The link has no loopback wired, so this is what stands between the framing
// being right and it being merely plausible. It exercises the cases that
// actually bite: payloads full of the delimiter byte, the 254-byte COBS code
// boundary, single-bit corruption, and joining a stream mid-frame.

static unsigned failures = 0;
static unsigned checks = 0;

// Counted as well as verified: a self-test that silently stopped running its
// cases would otherwise report exactly the same "passed" as a real pass.
static void check(bool condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

// Encode then feed back byte by byte; verifies the payload survives intact.
static void check_roundtrip(const uint8_t *data, size_t length, const char *what) {
    uint8_t wire[PROTO_MAX_FRAME];
    const size_t wire_length = proto_encode(PROTO_SAMPLE, 0x42, data, length,
                                            wire, sizeof(wire));
    checks++;
    if (wire_length == 0) {
        failures++;
        printf("  FAIL: %s (encode refused)\n", what);
        return;
    }

    for (size_t i = 0; i + 1 < wire_length; i++) {
        if (wire[i] == 0x00) {
            failures++;
            checks++;
            printf("  FAIL: %s (zero byte inside frame at %u)\n", what, (unsigned)i);
            return;
        }
    }

    ProtoDecoder decoder;
    proto_decoder_reset(&decoder);
    decoder.crc_errors = decoder.overflow_errors = decoder.short_frames = 0;

    ProtoFrame frame;
    bool got = false;
    for (size_t i = 0; i < wire_length; i++) {
        if (proto_decode_byte(&decoder, wire[i], &frame)) {
            got = true;
        }
    }

    checks++;
    if (!got) {
        failures++;
        printf("  FAIL: %s (no frame decoded)\n", what);
        return;
    }
    check(frame.type == PROTO_SAMPLE, what);
    check(frame.seq == 0x42, what);
    check(frame.length == length, what);
    check(length == 0 || memcmp(frame.data, data, length) == 0, what);
}

unsigned proto_selftest(void) {
    failures = 0;
    checks = 0;
    printf("\n=== proto self-test ===\n");

    // Round trips, including the payloads most likely to break COBS.
    const uint8_t empty[1] = {0};
    check_roundtrip(empty, 0, "empty payload");

    const uint8_t simple[] = {0x01, 0x02, 0x03};
    check_roundtrip(simple, sizeof(simple), "simple payload");

    const uint8_t zeros[16] = {0};
    check_roundtrip(zeros, sizeof(zeros), "all zeros");

    uint8_t mixed[32];
    for (size_t i = 0; i < sizeof(mixed); i++) mixed[i] = (i % 3 == 0) ? 0x00 : (uint8_t)i;
    check_roundtrip(mixed, sizeof(mixed), "zeros interleaved");

    uint8_t high[64];
    memset(high, 0xFF, sizeof(high));
    check_roundtrip(high, sizeof(high), "no zeros at all");

    uint8_t big[PROTO_MAX_PAYLOAD - 4];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    check_roundtrip(big, sizeof(big), "maximum payload");

    // A real sample: seq, timestamp, five variable bytes.
    const uint8_t sample[] = {0x00, 0x7B, 0x00, 0x01, 0xE2, 0x40, 0x00, 0xB6, 0x00, 0x5E, 0x5B};
    check_roundtrip(sample, sizeof(sample), "realistic sample");

    // Oversized payloads must be refused rather than truncated.
    uint8_t oversized[PROTO_MAX_PAYLOAD + 8];
    memset(oversized, 0xAA, sizeof(oversized));
    uint8_t wire[PROTO_MAX_FRAME];
    check(proto_encode(PROTO_SAMPLE, 0, oversized, sizeof(oversized), wire, sizeof(wire)) == 0,
          "oversized payload refused");

    // Corruption must be caught, not delivered.
    {
        const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
        size_t n = proto_encode(PROTO_LOG, 7, payload, sizeof(payload), wire, sizeof(wire));
        wire[2] ^= 0x01;  // flip one bit in the body

        ProtoDecoder decoder;
        proto_decoder_reset(&decoder);
        decoder.crc_errors = decoder.overflow_errors = decoder.short_frames = 0;

        ProtoFrame frame;
        bool got = false;
        for (size_t i = 0; i < n; i++) {
            if (proto_decode_byte(&decoder, wire[i], &frame)) got = true;
        }
        check(!got, "single-bit corruption rejected");
        check(decoder.crc_errors == 1, "corruption counted as a CRC error");
    }

    // Joining mid-stream: garbage, a truncated frame, then a good one.
    {
        const uint8_t payload[] = {0x11, 0x22, 0x33};
        size_t n = proto_encode(PROTO_ACK, 9, payload, sizeof(payload), wire, sizeof(wire));

        ProtoDecoder decoder;
        proto_decoder_reset(&decoder);
        decoder.crc_errors = decoder.overflow_errors = decoder.short_frames = 0;

        ProtoFrame frame;
        bool got = false;

        const uint8_t junk[] = {0x7F, 0x03, 0x91, 0xC4};  // mid-frame join
        for (size_t i = 0; i < sizeof(junk); i++) {
            if (proto_decode_byte(&decoder, junk[i], &frame)) got = true;
        }
        // Half of a real frame, then the delimiter that ends the garbage.
        for (size_t i = 0; i < n / 2; i++) {
            if (proto_decode_byte(&decoder, wire[i], &frame)) got = true;
        }
        if (proto_decode_byte(&decoder, 0x00, &frame)) got = true;
        check(!got, "garbage before resync produced no frame");

        for (size_t i = 0; i < n; i++) {
            if (proto_decode_byte(&decoder, wire[i], &frame)) got = true;
        }
        check(got, "resynchronised on the next good frame");
        check(got && frame.type == PROTO_ACK && frame.length == sizeof(payload),
              "resynchronised frame intact");
    }

    // A frame longer than the buffer must not wedge the decoder.
    {
        ProtoDecoder decoder;
        proto_decoder_reset(&decoder);
        decoder.crc_errors = decoder.overflow_errors = decoder.short_frames = 0;

        ProtoFrame frame;
        for (size_t i = 0; i < PROTO_MAX_FRAME * 2; i++) {
            proto_decode_byte(&decoder, 0xA5, &frame);
        }
        check(decoder.overflow_errors == 1, "runaway frame counted once");

        const uint8_t payload[] = {0x55};
        size_t n = proto_encode(PROTO_EVENT, 3, payload, sizeof(payload), wire, sizeof(wire));
        bool got = false;
        proto_decode_byte(&decoder, 0x00, &frame);  // delimiter ends the runaway
        for (size_t i = 0; i < n; i++) {
            if (proto_decode_byte(&decoder, wire[i], &frame)) got = true;
        }
        check(got, "decoder recovers after an oversized frame");
    }

    // Back-to-back frames must not need a gap between them.
    {
        const uint8_t a[] = {0x01};
        const uint8_t b[] = {0x02, 0x03};
        uint8_t stream[PROTO_MAX_FRAME * 2];
        size_t n = proto_encode(PROTO_SAMPLE, 1, a, sizeof(a), stream, sizeof(stream));
        n += proto_encode(PROTO_SAMPLE, 2, b, sizeof(b), stream + n, sizeof(stream) - n);

        ProtoDecoder decoder;
        proto_decoder_reset(&decoder);
        decoder.crc_errors = decoder.overflow_errors = decoder.short_frames = 0;

        ProtoFrame frame;
        unsigned count = 0;
        uint8_t seqs[4] = {0};
        for (size_t i = 0; i < n; i++) {
            if (proto_decode_byte(&decoder, stream[i], &frame)) {
                if (count < 4) seqs[count] = frame.seq;
                count++;
            }
        }
        check(count == 2, "two back-to-back frames decoded");
        check(seqs[0] == 1 && seqs[1] == 2, "back-to-back frames in order");
    }

    if (failures == 0) {
        printf("=== proto self-test: %u checks passed ===\n\n", checks);
    } else {
        printf("=== proto self-test: %u FAILURE(S) in %u checks ===\n\n", failures, checks);
    }
    return failures;
}
