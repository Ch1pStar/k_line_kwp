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
static void send_write_memory_chunk(Dashboard *dash, uint32_t address,
                                    const unsigned char *data, size_t size, uint8_t chunk_number) {
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

    const uint8_t data_offset = chunk_number * 8;
    memcpy(&msg.data[5], &data[data_offset], size);

    printf("Writing memory chunk to ECU. Address: 0x%08X, Size: 0x%02X(%u), Chunk: %d\n",
           address, size, size, chunk_number);

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

static void load_handler_setzi(Dashboard *dash) {
    size_t size = _binary_handler_setzi_bin_end - _binary_handler_setzi_bin_start;
    const unsigned char *data = _binary_handler_setzi_bin_start;
    uint8_t chunk_size = 0x08;
    const uint8_t num_chunks = (size + chunk_size - 1) / chunk_size;
    uint32_t start_address = 0x387a00;

    printf("Loading handler_setzi into ECU RAM. Size: %zu, Number of chunks: %u\n", size, num_chunks);

    for (uint8_t i = 0; i < num_chunks; i++) {
        uint8_t this_chunk = chunk_size;
        if (i + 1 == num_chunks) {
            this_chunk = size - (i * chunk_size);
        }

        uint32_t chunk_address = start_address + (i * chunk_size);
        send_write_memory_chunk(dash, chunk_address, data, this_chunk, i);
        sleep_ms(100);
    }

    // send_set_heartbeat(dash, 2000);
}

static void load_handler_prj(Dashboard *dash) {
    size_t size = _binary_handler_bin_end - _binary_handler_bin_start;
    const unsigned char *data = _binary_handler_bin_start;
    uint8_t chunk_size = 0x08;
    const uint8_t num_chunks = (size + chunk_size - 1) / chunk_size;
    uint32_t start_address = 0x387acc;

    printf("Loading handler into ECU RAM. Size: %zu, Number of chunks: %u\n", size, num_chunks);

    for (uint8_t i = 0; i < num_chunks; i++) {
        uint8_t this_chunk = chunk_size;
        if (i + 1 == num_chunks) {
            this_chunk = size - (i * chunk_size);
        }

        uint32_t chunk_address = start_address + (i * chunk_size);
        send_write_memory_chunk(dash, chunk_address, data, this_chunk, i);
        sleep_ms(100);
    }

    sleep_ms(3200);
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
        sleep_ms(100);
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
        printf("fill-distributor-table - Fill distributor table\n");
        printf("cmd:XXXX         - Send raw KWP2000 command (hex)\n");
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

    if (strcmp(cmd, "fill-distributor-table") == 0) {
        printf("Filling distributor table...\n");
        fill_distributor_table(dash);
        return;
    }

    // Raw hex command: "cmd:1A9B"
    if (strncmp(cmd, "cmd:", 4) == 0) {
        const char *hex_str = cmd + 4;
        size_t hex_len = strlen(hex_str);

        if (hex_len == 0 || hex_len % 2 != 0) {
            printf("Error: Hex string must have even number of characters (length: %zu)\n", hex_len);
            return;
        }

        BufferMessage msg = {
            .messageType = MSG_COMMAND,
            .length = hex_len / 2,
        };

        for (size_t i = 0; i < hex_len; i += 2) {
            char hex_byte[3] = {hex_str[i], hex_str[i + 1], '\0'};
            msg.data[i / 2] = (uint8_t)strtol(hex_byte, NULL, 16);
        }

        printf("Sending custom command: ");
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
