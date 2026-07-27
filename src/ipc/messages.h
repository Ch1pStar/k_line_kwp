#pragma once

// Inter-core message types.
//
// Two ring buffers carry these, each strictly single-producer/single-consumer
// (see ring_buffer.h):
//
//   dash_to_ecu : produced by core 1 (console), consumed by core 0
//   ecu_to_dash : produced by core 0 (ECU + klog), consumed by core 1
//
// The direction each type travels is noted below; nothing else may push to a
// buffer from the other core without adding real locking first.
typedef enum {
    MSG_ECU_DATA            = 0x01,  // core0 -> core1: response payload
    MSG_COMMAND             = 0x03,  // core1 -> core0: KWP2000 frame to send
    MSG_ACK                 = 0x04,  // core0 -> core1: command succeeded
    MSG_NACK                = 0x05,  // core0 -> core1: failed, data[0] = status
    MSG_CONNECT_ECU         = 0x06,  // core1 -> core0
    MSG_DISCONNECT_ECU      = 0x07,  // core1 -> core0
    MSG_ECU_DISCONNECTED    = 0x08,  // core0 -> core1: link lost (unsolicited)
    MSG_SET_HEARTBEAT       = 0x09,  // core1 -> core0: 4 bytes, big-endian ms
    MSG_RAW_COMMAND         = 0x0A,  // core1 -> core0: send, dump reply verbatim
    MSG_SET_BAUD            = 0x0B,  // core1 -> core0: 4 bytes, big-endian
    MSG_LOG                 = 0x0C,  // core0 -> core1: text line to print
    MSG_INSTALL_HANDLER     = 0x0D,  // core1 -> core0: run the full injection
    MSG_LOAD_HANDLER        = 0x0E,  // core1 -> core0: write the blob only

    // core0 -> core1: one sample from the free-running logger.
    // Payload: [seq:2 BE][timestamp_ms:4 BE][raw variable bytes]
    MSG_SAMPLE              = 0x0F,

    MSG_START_STREAM        = 0x10,  // core1 -> core0: 4 bytes BE interval ms
    MSG_STOP_STREAM         = 0x11,  // core1 -> core0
    MSG_SET_LOG_VARS        = 0x12,  // core1 -> core0: flat 3-byte addresses
    MSG_DUMP_MEMORY         = 0x13,  // core1 -> core0: [addr:3][length:2 BE]
} MessageType;
