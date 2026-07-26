#include "command.h"
#include "messages.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

extern unsigned char _binary_handler_bin_start[];
extern unsigned char _binary_handler_bin_end[];

extern unsigned char _binary_handler_setzi_bin_start[];
extern unsigned char _binary_handler_setzi_bin_end[];

static RingBuffer *ecu_queue = NULL;
static CommandHost command_host = {0};

void command_init(RingBuffer *to_ecu, const CommandHost *host) {
    ecu_queue = to_ecu;
    command_host = *host;
}

static void report(const char *fmt, ...) {
    if (command_host.report == NULL) return;

    char line[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    command_host.report(command_host.ctx, line);
}

// Wait, letting the caller drain core 0's messages meanwhile - see the pump
// comment in command.h.
static void wait_ms(uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    do {
        if (command_host.pump != NULL) {
            command_host.pump(command_host.ctx);
        }
        sleep_ms(1);
    } while (!time_reached(deadline));
}

static bool push(const BufferMessage *msg) {
    return ringbuffer_push(ecu_queue, msg);
}

// Queue a KWP2000 frame for core 0 to send.
static bool send_kwp(const uint8_t *data, uint8_t length) {
    BufferMessage msg = {
        .messageType = MSG_COMMAND,
        .length = length,
    };
    memcpy(msg.data, data, length);
    return push(&msg);
}

static bool send_set_heartbeat(uint32_t interval_ms) {
    BufferMessage msg = {
        .messageType = MSG_SET_HEARTBEAT,
        .length = 4,
    };
    msg.data[0] = (interval_ms >> 24) & 0xFF;
    msg.data[1] = (interval_ms >> 16) & 0xFF;
    msg.data[2] = (interval_ms >> 8) & 0xFF;
    msg.data[3] = interval_ms & 0xFF;
    return push(&msg);
}

static bool send_set_baud(uint32_t baud) {
    BufferMessage msg = {
        .messageType = MSG_SET_BAUD,
        .length = 4,
    };
    msg.data[0] = (baud >> 24) & 0xFF;
    msg.data[1] = (baud >> 16) & 0xFF;
    msg.data[2] = (baud >> 8) & 0xFF;
    msg.data[3] = baud & 0xFF;
    return push(&msg);
}

// Queue one WriteMemoryByAddress (0x3D) chunk.
// data_offset is passed in rather than derived from a chunk index: computing it
// here as a uint8_t silently wrapped at 256 bytes and re-sent the start of the
// binary for every chunk past 31.
static void send_write_memory_chunk(uint32_t address, const unsigned char *data,
                                    size_t size, size_t data_offset) {
    uint8_t address_size = 3;
    uint8_t command_length = 1 + address_size + 1 + size;

    BufferMessage msg = {
        .messageType = MSG_COMMAND,
        .length = command_length,
    };

    msg.data[0] = 0x3d; // Write memory service id
    msg.data[1] = (address >> 16) & 0xFF;
    msg.data[2] = (address >> 8) & 0xFF;
    msg.data[3] = address & 0xFF;
    msg.data[4] = size;

    memcpy(&msg.data[5], &data[data_offset], size);

    report("Writing memory chunk to ECU. Address: 0x%06X, Size: %u, File offset: %u",
           (unsigned)address, (unsigned)size, (unsigned)data_offset);

    push(&msg);
}

static void load_handler_blob(const unsigned char *data, size_t size,
                              uint32_t start_address, const char *name) {
    const size_t chunk_size = 0x08;
    const size_t num_chunks = (size + chunk_size - 1) / chunk_size;

    report("Loading %s into ECU RAM. Size: %u, Number of chunks: %u",
           name, (unsigned)size, (unsigned)num_chunks);

    for (size_t i = 0; i < num_chunks; i++) {
        size_t offset = i * chunk_size;
        size_t this_chunk = chunk_size;
        if (offset + this_chunk > size) {
            this_chunk = size - offset;
        }

        send_write_memory_chunk(start_address + offset, data, this_chunk, offset);
        wait_ms(100);
    }
}

static void load_handler_setzi(void) {
    load_handler_blob(_binary_handler_setzi_bin_start,
                      _binary_handler_setzi_bin_end - _binary_handler_setzi_bin_start,
                      0x387a00, "handler_setzi");
}

static void fill_distributor_table(void) {
    uint8_t data_size = 4;
    uint8_t table_entries = 48;
    uint8_t handler_address[4] = {0x00, 0x38, 0x7a, 0xcc};

    uint8_t command_length = 1 + 3 + 1 + data_size;

    for (uint8_t i = 0; i < table_entries; i++) {
        BufferMessage msg = {
            .messageType = MSG_COMMAND,
            .length = command_length,
        };

        msg.data[0] = 0x3d;
        msg.data[1] = 0x38;
        msg.data[2] = 0x7a;
        msg.data[3] = 0x00 + (i * data_size);
        msg.data[4] = data_size;
        memcpy(&msg.data[5], handler_address, 4);

        push(&msg);
        wait_ms(100);
    }
}

// The 0xB7 "set logging variables" request: SID then a leading format byte
// (0x03, still under investigation) then one 3-byte big-endian address per
// variable. These map to nmot, ub, wped, plsol, tmot on the 8N0906018BP ECU
// (see me7log/ecu_files/8N0906018BP 0002.ecu for scaling). Positive resp = 0xF7.
static const uint8_t set_log_vars_cmd[] = {
    0xB7,
    0x03,
    0x00, 0xF8, 0x9A,   // nmot  - engine speed
    0x38, 0x09, 0x91,   // ub    - battery voltage
    0x38, 0x09, 0x9D,   // wped  - accelerator pedal
    0x38, 0x09, 0xF6,   // plsol - target boost
    0x38, 0x0A, 0x32,   // tmot  - coolant temp
};

// Drive the whole handler-injection sequence in the order proven to work. The
// ECU must already be connected (5-baud init done via CMD_CONNECT). See the
// "Handler Injection Flow" section of CLAUDE.md for why each step is here.
static void start_logging(void) {
    report("=== start-logging: injecting fast-logging handler ===");

    // 1. Keep the KWP session alive throughout. Without this the session times
    //    out in the gaps between steps and every later command fails.
    report("[1/6] heartbeat -> 2000ms");
    send_set_heartbeat(2000);
    wait_ms(200);

    // 2. Manufacturer session - required before ReadMemoryByAddress/WriteMemory.
    report("[2/6] diagnostic session (10 86)");
    { uint8_t d[] = {0x10, 0x86}; send_kwp(d, sizeof(d)); }
    wait_ms(300);

    // 3. Write the 582-byte handler to RAM at 0x387A00. Must happen before the
    //    redirect, while 0x3D still routes through the original dispatcher.
    report("[3/6] loading handler");
    load_handler_setzi();
    wait_ms(200);

    // 4. Redirect the service-table pointer at 0xE228 to our table at 0x387A00.
    //    Bytes 00 3A E1 00 are the C166 far-pointer encoding of 0x387A00.
    report("[4/6] redirecting service table (0xE228 -> 0x387A00)");
    { uint8_t d[] = {0x3D, 0x00, 0xE2, 0x28, 0x04, 0x00, 0x3A, 0xE1, 0x00};
      send_kwp(d, sizeof(d)); }
    wait_ms(300);

    // 5. First post-redirect call runs the handler's table-copy init. It returns
    //    SNS for 0x3E itself; that is expected and harmless.
    report("[5/6] init trigger (3E)");
    { uint8_t d[] = {0x3E}; send_kwp(d, sizeof(d)); }
    wait_ms(300);

    // 6. Hand the variable list to the handler. Positive response is 0xF7.
    //    After this, a bare CMD_READ_LOG returns the packed values.
    report("[6/6] setting log variables (B7 + list)");
    send_kwp(set_log_vars_cmd, sizeof(set_log_vars_cmd));
    wait_ms(300);

    report("=== handler ready. Sample with read-log. ===");
}

static CommandStatus queue_simple(uint8_t type) {
    BufferMessage msg = { .messageType = type, .length = 0 };
    return push(&msg) ? COMMAND_OK : COMMAND_QUEUE_FULL;
}

static CommandStatus queue_kwp(const uint8_t *data, uint8_t length) {
    return send_kwp(data, length) ? COMMAND_OK : COMMAND_QUEUE_FULL;
}

CommandStatus command_execute(const Command *cmd) {
    switch (cmd->id) {
        case CMD_CONNECT:
            return queue_simple(MSG_CONNECT_ECU);

        case CMD_DISCONNECT:
            return queue_simple(MSG_DISCONNECT_ECU);

        case CMD_ECU_ID: {
            uint8_t d[] = {0x1A, 0x9B};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_READ_DTCS: {
            uint8_t d[] = {0x18, 0x00, 0xFF, 0x00};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_CLEAR_DTCS: {
            uint8_t d[] = {0x14, 0xFF, 0x00};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_DIAG_SESSION: {
            uint8_t d[] = {0x10, 0x86};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_LOAD_HANDLER:
            load_handler_setzi();
            return COMMAND_OK;

        case CMD_START_LOGGING:
            start_logging();
            return COMMAND_OK;

        case CMD_READ_LOG: {
            // Bare 0xB7: the handler returns the packed variable values.
            uint8_t d[] = {0xB7};
            return queue_kwp(d, sizeof(d));
        }

        case CMD_FILL_DISTRIBUTOR_TABLE:
            fill_distributor_table();
            return COMMAND_OK;

        case CMD_RAW_KWP:
            return queue_kwp(cmd->payload, cmd->payload_length);

        case CMD_RAW_DUMP: {
            BufferMessage msg = {
                .messageType = MSG_RAW_COMMAND,
                .length = cmd->payload_length,
            };
            memcpy(msg.data, cmd->payload, cmd->payload_length);
            return push(&msg) ? COMMAND_OK : COMMAND_QUEUE_FULL;
        }

        case CMD_SET_HEARTBEAT:
            report("Setting heartbeat interval to %u ms", (unsigned)cmd->value);
            return send_set_heartbeat(cmd->value) ? COMMAND_OK : COMMAND_QUEUE_FULL;

        case CMD_SET_BAUD:
            return send_set_baud(cmd->value) ? COMMAND_OK : COMMAND_QUEUE_FULL;

        default:
            return COMMAND_UNKNOWN;
    }
}
