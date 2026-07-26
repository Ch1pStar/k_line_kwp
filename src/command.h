#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ring_buffer.h"

// The one place that turns "do a thing to the ECU" into messages for core 0.
//
// Both frontends go through here: the USB text console today, and the RPi 5
// host link from phase 5. Nothing in this layer knows how a command arrived or
// where its output is shown - that is what CommandHost is for. Adding a
// frontend means implementing CommandHost, not duplicating any of this.

typedef enum {
    CMD_NONE = 0,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_ECU_ID,
    CMD_READ_DTCS,
    CMD_CLEAR_DTCS,
    CMD_DIAG_SESSION,
    CMD_LOAD_HANDLER,
    CMD_START_LOGGING,
    CMD_READ_LOG,
    CMD_FILL_DISTRIBUTOR_TABLE,  // PRJ handler path, unused - deleted in phase 3
    CMD_RAW_KWP,                 // payload = KWP frame, reply parsed normally
    CMD_RAW_DUMP,                // payload = KWP frame, reply dumped verbatim
    CMD_SET_HEARTBEAT,           // value = interval in ms
    CMD_SET_BAUD,                // value = K-line bit rate
} CommandId;

typedef struct {
    CommandId id;
    uint32_t value;
    uint8_t payload[MAX_MESSAGE_SIZE];
    uint8_t payload_length;
} Command;

typedef enum {
    COMMAND_OK,
    COMMAND_UNKNOWN,
    COMMAND_QUEUE_FULL,   // core 0 is not keeping up; the command was dropped
} CommandStatus;

// How the command layer talks back to whoever issued the command.
typedef struct {
    // One line of human-readable progress. No trailing newline.
    void (*report)(void *ctx, const char *line);

    // Called repeatedly while the command layer waits. Multi-step sequences
    // (the handler load is ~7s of pauses) would otherwise block the caller
    // from draining core 0's messages and overflow the queue. This disappears
    // in phase 3 when the sequencing moves to core 0 and becomes ack-driven.
    void (*pump)(void *ctx);

    void *ctx;
} CommandHost;

void command_init(RingBuffer *to_ecu, const CommandHost *host);
CommandStatus command_execute(const Command *cmd);
