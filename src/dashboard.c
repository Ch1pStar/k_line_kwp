#include "dashboard.h"
#include "ecu_state_machine.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"

extern unsigned char _binary_handler_bin_start[];
extern unsigned char _binary_handler_bin_end[];

extern unsigned char _binary_handler_setzi_bin_start[];
extern unsigned char _binary_handler_setzi_bin_end[];

// Helper: push a MSG_COMMAND with write-memory service (0x3d) for one chunk
// data_offset is passed in rather than derived from a chunk index: computing it
// here as a uint8_t silently wrapped at 256 bytes and re-sent the start of the
// binary for every chunk past 31.
static void send_write_memory_chunk(Dashboard *dash, uint32_t address,
                                    const unsigned char *data, size_t size, size_t data_offset) {
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

    printf("Writing memory chunk to ECU. Address: 0x%06X, Size: %u, File offset: %u\n",
           (unsigned)address, (unsigned)size, (unsigned)data_offset);

    ringbuffer_push(dash->tx_buffer, &msg);
}

// Helper: set the ECU heartbeat interval via message
static void send_set_heartbeat(Dashboard *dash, uint32_t interval_ms) {
    BufferMessage msg = {
        .messageType = MSG_SET_HEARTBEAT,
        .length = 4,
    };
    msg.data[0] = (interval_ms >> 24) & 0xFF;
    msg.data[1] = (interval_ms >> 16) & 0xFF;
    msg.data[2] = (interval_ms >> 8) & 0xFF;
    msg.data[3] = interval_ms & 0xFF;
    ringbuffer_push(dash->tx_buffer, &msg);
}

// Forward declarations
static void fill_distributor_table(Dashboard *dash);
static void process_ecu_messages(Dashboard *dash);

// Wait, but keep draining core 0's messages while doing it. Loading the handler
// spends ~7s in these pauses, and a console that stops consuming during them
// overflows the log queue - the first run of this dropped 70 lines. Phase 3
// moves the sequencing to core 0 and these waits disappear entirely.
static void dash_wait_ms(Dashboard *dash, uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    do {
        process_ecu_messages(dash);
        sleep_ms(1);
    } while (!time_reached(deadline));
}

static void load_handler_setzi(Dashboard *dash) {
    size_t size = _binary_handler_setzi_bin_end - _binary_handler_setzi_bin_start;
    const unsigned char *data = _binary_handler_setzi_bin_start;
    const size_t chunk_size = 0x08;
    const size_t num_chunks = (size + chunk_size - 1) / chunk_size;
    uint32_t start_address = 0x387a00;

    printf("Loading handler_setzi into ECU RAM. Size: %u, Number of chunks: %u\n",
           (unsigned)size, (unsigned)num_chunks);

    for (size_t i = 0; i < num_chunks; i++) {
        size_t offset = i * chunk_size;
        size_t this_chunk = chunk_size;
        if (offset + this_chunk > size) {
            this_chunk = size - offset;
        }

        send_write_memory_chunk(dash, start_address + offset, data, this_chunk, offset);
        dash_wait_ms(dash, 100);
    }

    // send_set_heartbeat(dash, 2000);
}

static void load_handler_prj(Dashboard *dash) {
    size_t size = _binary_handler_bin_end - _binary_handler_bin_start;
    const unsigned char *data = _binary_handler_bin_start;
    const size_t chunk_size = 0x08;
    const size_t num_chunks = (size + chunk_size - 1) / chunk_size;
    uint32_t start_address = 0x387acc;

    printf("Loading handler into ECU RAM. Size: %u, Number of chunks: %u\n",
           (unsigned)size, (unsigned)num_chunks);

    for (size_t i = 0; i < num_chunks; i++) {
        size_t offset = i * chunk_size;
        size_t this_chunk = chunk_size;
        if (offset + this_chunk > size) {
            this_chunk = size - offset;
        }

        send_write_memory_chunk(dash, start_address + offset, data, this_chunk, offset);
        dash_wait_ms(dash, 100);
    }

    dash_wait_ms(dash, 3200);
    fill_distributor_table(dash);
}

static void fill_distributor_table(Dashboard *dash) {
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

        ringbuffer_push(dash->tx_buffer, &msg);
        dash_wait_ms(dash, 100);
    }
}

// Send a simple MSG_COMMAND from hex bytes
static void send_command(Dashboard *dash, const uint8_t *data, uint8_t length) {
    BufferMessage msg = {
        .messageType = MSG_COMMAND,
        .length = length,
    };
    memcpy(msg.data, data, length);
    ringbuffer_push(dash->tx_buffer, &msg);
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
// ECU must already be connected (5-baud init done via `connect`). See the
// "Fast Logging" section of README.md for why each step is here.
static void start_logging(Dashboard *dash) {
    printf("\n=== start-logging: injecting fast-logging handler ===\n");

    // 1. Keep the KWP session alive throughout. Without this the session times
    //    out in the gaps between steps and every later command fails.
    printf("[1/6] heartbeat -> 2000ms\n");
    send_set_heartbeat(dash, 2000);
    dash_wait_ms(dash, 200);

    // 2. Manufacturer session - required before ReadMemoryByAddress/WriteMemory.
    printf("[2/6] diagnostic session (10 86)\n");
    { uint8_t d[] = {0x10, 0x86}; send_command(dash, d, sizeof(d)); }
    dash_wait_ms(dash, 300);

    // 3. Write the 582-byte handler to RAM at 0x387A00. Must happen before the
    //    redirect, while 0x3D still routes through the original dispatcher.
    printf("[3/6] loading handler\n");
    load_handler_setzi(dash);
    dash_wait_ms(dash, 200);

    // 4. Redirect the service-table pointer at 0xE228 to our table at 0x387A00.
    //    Bytes 00 3A E1 00 are the C166 far-pointer encoding of 0x387A00.
    printf("[4/6] redirecting service table (0xE228 -> 0x387A00)\n");
    { uint8_t d[] = {0x3D, 0x00, 0xE2, 0x28, 0x04, 0x00, 0x3A, 0xE1, 0x00};
      send_command(dash, d, sizeof(d)); }
    dash_wait_ms(dash, 300);

    // 5. First post-redirect call runs the handler's table-copy init. It returns
    //    SNS for 0x3E itself; that is expected and harmless.
    printf("[5/6] init trigger (3E)\n");
    { uint8_t d[] = {0x3E}; send_command(dash, d, sizeof(d)); }
    dash_wait_ms(dash, 300);

    // 6. Hand the variable list to the handler. Positive response is 0xF7.
    //    After this, a bare `read-log` returns the packed values.
    printf("[6/6] setting log variables (B7 + list)\n");
    send_command(dash, set_log_vars_cmd, sizeof(set_log_vars_cmd));
    dash_wait_ms(dash, 300);

    printf("=== handler ready. Use `read-log` to sample. ===\n\n");
}

static void process_command(Dashboard *dash, const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        printf("\n--- Available Commands ---\n");
        printf("connect          - Connect to ECU\n");
        printf("disconnect       - Disconnect from ECU\n");
        printf("ecu-id           - Request ECU identification\n");
        printf("read-dtcs        - Read diagnostic trouble codes\n");
        printf("clear-dtcs       - Clear diagnostic trouble codes\n");
        printf("diag-session     - Start special diagnostic session\n");
        printf("load-handler     - Load handler into ECU\n");
        printf("start-logging    - Inject handler + set up fast logging (do after connect)\n");
        printf("read-log         - Sample the logged variables (bare 0xB7)\n");
        printf("fill-distributor-table - Fill distributor table\n");
        printf("cmd:XXXX         - Send raw KWP2000 command (hex)\n");
        printf("raw:XXXX         - Same, but dump the unparsed reply bytes\n");
        printf("heartbeat:MS     - Set keep-alive interval in ms\n");
        printf("baud:N           - Set K-line bit rate (e.g. baud:57600)\n");
        printf("help             - Show this help\n");
        printf("------------------------\n\n");
        return;
    }

    printf("Command received: \"%s\"\n", cmd);

    if (strcmp(cmd, "connect") == 0) {
        printf("Attempting to connect to ECU...\n");
        BufferMessage msg = { .messageType = MSG_CONNECT_ECU, .length = 0 };
        ringbuffer_push(dash->tx_buffer, &msg);
        return;
    }

    if (strcmp(cmd, "disconnect") == 0) {
        printf("Disconnecting from ECU...\n");
        BufferMessage msg = { .messageType = MSG_DISCONNECT_ECU, .length = 0 };
        ringbuffer_push(dash->tx_buffer, &msg);
        return;
    }

    if (strcmp(cmd, "ecu-id") == 0) {
        printf("Requesting ECU ID...\n");
        uint8_t data[] = {0x1A, 0x9B};
        send_command(dash, data, sizeof(data));
        return;
    }

    if (strcmp(cmd, "read-dtcs") == 0) {
        printf("Reading DTCs...\n");
        uint8_t data[] = {0x18, 0x00, 0xFF, 0x00};
        send_command(dash, data, sizeof(data));
        return;
    }

    if (strcmp(cmd, "clear-dtcs") == 0) {
        printf("Clearing DTCs...\n");
        uint8_t data[] = {0x14, 0xFF, 0x00};
        send_command(dash, data, sizeof(data));
        return;
    }

    if (strcmp(cmd, "diag-session") == 0) {
        printf("Starting special diagnostic session...\n");
        uint8_t data[] = {0x10, 0x86};
        send_command(dash, data, sizeof(data));
        return;
    }

    if (strcmp(cmd, "load-handler") == 0) {
        printf("Loading handler into ECU...\n");
        load_handler_setzi(dash);
        return;
    }

    if (strcmp(cmd, "start-logging") == 0) {
        start_logging(dash);
        return;
    }

    if (strcmp(cmd, "read-log") == 0) {
        // Bare 0xB7: the handler returns the packed variable values (resp 0xF7).
        uint8_t data[] = {0xB7};
        send_command(dash, data, sizeof(data));
        return;
    }

    // "baud:57600" - follow the ECU after it switches rate mid-session
    if (strncmp(cmd, "baud:", 5) == 0) {
        uint32_t baud = (uint32_t)strtoul(cmd + 5, NULL, 10);
        if (baud == 0) {
            printf("Error: invalid baud rate\n");
            return;
        }
        BufferMessage msg = { .messageType = MSG_SET_BAUD, .length = 4 };
        msg.data[0] = (baud >> 24) & 0xFF;
        msg.data[1] = (baud >> 16) & 0xFF;
        msg.data[2] = (baud >> 8) & 0xFF;
        msg.data[3] = baud & 0xFF;
        ringbuffer_push(dash->tx_buffer, &msg);
        return;
    }

    // "heartbeat:60000" - park the keep-alive so it can't consume the first
    // service call after a handler redirect, which is the one that runs init.
    if (strncmp(cmd, "heartbeat:", 10) == 0) {
        uint32_t interval = (uint32_t)strtoul(cmd + 10, NULL, 10);
        printf("Setting heartbeat interval to %u ms\n", (unsigned)interval);
        send_set_heartbeat(dash, interval);
        return;
    }

    if (strcmp(cmd, "fill-distributor-table") == 0) {
        printf("Filling distributor table...\n");
        fill_distributor_table(dash);
        return;
    }

    // Raw hex command: "cmd:1A9B", or "raw:1A9B" to dump the unparsed reply
    if (strncmp(cmd, "cmd:", 4) == 0 || strncmp(cmd, "raw:", 4) == 0) {
        const bool is_raw = (cmd[0] == 'r');
        const char *hex_str = cmd + 4;
        size_t hex_len = strlen(hex_str);

        if (hex_len == 0 || hex_len % 2 != 0) {
            printf("Error: Hex string must have even number of characters (length: %zu)\n", hex_len);
            return;
        }

        // The console accepts a 512-char line, which is more bytes than a
        // message slot holds. Reject rather than truncate - a silently
        // shortened KWP frame is a confusing way to fail.
        if (hex_len / 2 > MAX_MESSAGE_SIZE) {
            printf("Error: command is %zu bytes, max %u\n",
                   hex_len / 2, (unsigned)MAX_MESSAGE_SIZE);
            return;
        }

        BufferMessage msg = {
            .messageType = is_raw ? MSG_RAW_COMMAND : MSG_COMMAND,
            .length = hex_len / 2,
        };

        for (size_t i = 0; i < hex_len; i += 2) {
            char hex_byte[3] = {hex_str[i], hex_str[i + 1], '\0'};
            msg.data[i / 2] = (uint8_t)strtol(hex_byte, NULL, 16);
        }

        printf("Sending %s command: ", is_raw ? "raw" : "custom");
        for (size_t i = 0; i < msg.length; i++) {
            printf("%02X ", msg.data[i]);
        }
        printf("\n");

        ringbuffer_push(dash->tx_buffer, &msg);
        return;
    }

    printf("Unknown command: \"%s\". Type 'help' for available commands.\n", cmd);
}

static void process_ecu_messages(Dashboard *dash) {
    BufferMessage msg;

    while (ringbuffer_pop(dash->rx_buffer, &msg)) {
        switch (msg.messageType) {
            case MSG_ACK:
                if (msg.length > 0 && msg.data[0] == 1) {
                    printf("ECU connected successfully\n");
                } else {
                    printf("Command acknowledged\n");
                }
                break;

            case MSG_NACK:
                printf("Command failed with error code: %d\n",
                       msg.length > 0 ? msg.data[0] : 0);
                break;

            case MSG_ECU_DATA:
                printf("ECU Response Data (%d bytes): ", msg.length);
                for (size_t i = 0; i < msg.length; i++) {
                    printf("%02X ", msg.data[i]);
                }
                printf("\n");

                printf("ECU Response ASCII: \"");
                for (size_t i = 0; i < msg.length; i++) {
                    if (msg.data[i] >= 32 && msg.data[i] <= 126) {
                        putchar(msg.data[i]);
                    } else {
                        putchar('.');
                    }
                }
                printf("\"\n");
                break;

            case MSG_ECU_DISCONNECTED:
                printf("ECU disconnected\n");
                break;

            // Core 0 never prints directly (see log.h) - it queues lines here.
            case MSG_LOG:
                printf("%.*s\n", (int)msg.length, (const char *)msg.data);
                break;

            default:
                printf("Unknown response type: %d\n", msg.messageType);
                break;
        }
    }
}

static bool read_console_input(Dashboard *dash) {
    static char cmd_buffer[512];
    static int buf_pos = 0;
    int c;

    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\n' || c == '\r') {
            if (buf_pos > 0) {
                cmd_buffer[buf_pos] = '\0';
                process_command(dash, cmd_buffer);
                buf_pos = 0;
                return true;
            }
        } else if (buf_pos < (int)sizeof(cmd_buffer) - 1 && c >= 32 && c <= 126) {
            cmd_buffer[buf_pos++] = (char)c;
        }
    }

    return false;
}

void dashboard_init(Dashboard *dash, RingBuffer *tx, RingBuffer *rx) {
    dash->rx_buffer = rx;
    dash->tx_buffer = tx;
    printf("Dashboard interface ready. Type 'help' for commands.\n");
}

void dashboard_update(Dashboard *dash) {
    read_console_input(dash);
    process_ecu_messages(dash);
}
