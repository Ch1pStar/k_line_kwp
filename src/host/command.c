#include "command.h"
#include "messages.h"

#include <string.h>

static RingBuffer *ecu_queue = NULL;

void command_init(RingBuffer *to_ecu) {
    ecu_queue = to_ecu;
}

static CommandStatus queue(const BufferMessage *msg) {
    return ringbuffer_push(ecu_queue, msg) ? COMMAND_OK : COMMAND_QUEUE_FULL;
}

static CommandStatus queue_bare(uint8_t message_type) {
    BufferMessage msg = { .messageType = message_type, .length = 0 };
    return queue(&msg);
}

// Queue a KWP2000 frame for core 0 to send.
static CommandStatus queue_kwp(const uint8_t *data, uint8_t length) {
    BufferMessage msg = {
        .messageType = MSG_COMMAND,
        .length = length,
    };
    memcpy(msg.data, data, length);
    return queue(&msg);
}

static CommandStatus queue_u32(uint8_t message_type, uint32_t value) {
    BufferMessage msg = {
        .messageType = message_type,
        .length = 4,
    };
    msg.data[0] = (value >> 24) & 0xFF;
    msg.data[1] = (value >> 16) & 0xFF;
    msg.data[2] = (value >> 8) & 0xFF;
    msg.data[3] = value & 0xFF;
    return queue(&msg);
}

CommandStatus command_execute(const Command *cmd) {
    switch (cmd->id) {
        case CMD_CONNECT:
            return queue_bare(MSG_CONNECT_ECU);

        case CMD_DISCONNECT:
            return queue_bare(MSG_DISCONNECT_ECU);

        case CMD_ECU_ID: {
            const uint8_t d[] = {0x1A, 0x9B};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_READ_DTCS: {
            const uint8_t d[] = {0x18, 0x00, 0xFF, 0x00};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_CLEAR_DTCS: {
            const uint8_t d[] = {0x14, 0xFF, 0x00};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_DIAG_SESSION: {
            const uint8_t d[] = {0x10, 0x86};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_LOAD_HANDLER:
            return queue_bare(MSG_LOAD_HANDLER);

        case CMD_START_LOGGING:
            return queue_bare(MSG_INSTALL_HANDLER);

        case CMD_READ_LOG: {
            // Bare 0xB7: the handler returns the packed variable values.
            const uint8_t d[] = {0xB7};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_STREAM_START:
            return queue_u32(MSG_START_STREAM, cmd->value);

        case CMD_STREAM_STOP:
            return queue_bare(MSG_STOP_STREAM);

        case CMD_SET_LOG_VARS: {
            BufferMessage msg = {
                .messageType = MSG_SET_LOG_VARS,
                .length = cmd->payload_length,
            };
            memcpy(msg.data, cmd->payload, cmd->payload_length);
            return queue(&msg);
        }

        case CMD_RAW_KWP:
            return queue_kwp(cmd->payload, cmd->payload_length);

        case CMD_RAW_DUMP: {
            BufferMessage msg = {
                .messageType = MSG_RAW_COMMAND,
                .length = cmd->payload_length,
            };
            memcpy(msg.data, cmd->payload, cmd->payload_length);
            return queue(&msg);
        }

        case CMD_SET_HEARTBEAT:
            return queue_u32(MSG_SET_HEARTBEAT, cmd->value);

        case CMD_SET_BAUD:
            return queue_u32(MSG_SET_BAUD, cmd->value);

        default:
            return COMMAND_UNKNOWN;
    }
}
