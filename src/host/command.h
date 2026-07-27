#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ring_buffer.h"

// The one place that turns "do a thing to the ECU" into messages for core 0.
//
// Both frontends go through here: the USB text console today, and the RPi 5
// host link from phase 5. This layer is pure translation - it queues a message
// and returns immediately, with no waiting, no sequencing and no output of its
// own. Anything multi-step (handler injection) belongs on core 0 where it can
// be paced by the ECU's replies, and anything worth printing is logged there.

typedef enum {
    CMD_NONE = 0,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_ECU_ID,
    CMD_READ_DTCS,
    CMD_CLEAR_DTCS,
    CMD_DIAG_SESSION,
    CMD_LOAD_HANDLER,            // write the handler blob to ECU RAM
    CMD_START_LOGGING,           // full injection sequence
    CMD_READ_LOG,                // bare 0xB7 sample
    CMD_STREAM_START,            // value = sample interval in ms (0 = full rate)
    CMD_STREAM_STOP,
    CMD_SET_LOG_VARS,            // payload = flat list of 3-byte ECU addresses
    CMD_DUMP_MEMORY,             // payload = [addr:3][length:2 BE]
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

void command_init(RingBuffer *to_ecu);
CommandStatus command_execute(const Command *cmd);
