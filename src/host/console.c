#include "console.h"
#include "command.h"
#include "messages.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"

// --- text -> Command ------------------------------------------------------

// Bare "stream-on" rate. Full rate floods a USB console at ~85 samples/s.
#define DEFAULT_STREAM_INTERVAL_MS 100

static void print_help(void) {
    printf("\n--- Available Commands ---\n");
    printf("connect          - Connect to ECU\n");
    printf("disconnect       - Disconnect from ECU\n");
    printf("ecu-id           - Request ECU identification\n");
    printf("read-dtcs        - Read diagnostic trouble codes\n");
    printf("clear-dtcs       - Clear diagnostic trouble codes\n");
    printf("diag-session     - Start special diagnostic session\n");
    printf("load-handler     - Load handler into ECU\n");
    printf("start-logging    - Inject handler + set up fast logging (do after connect)\n");
    printf("read-log         - Sample the logged variables once (bare 0xB7)\n");
    printf("stream-on[:MS]   - Start free-running sampling (default 100ms, 0 = full rate)\n");
    printf("stream-off       - Stop free-running sampling\n");
    printf("set-vars:HEX     - Set logged variables (3-byte addresses, e.g. 00F89A380A32)\n");
    printf("dump:ADDR:LEN    - Hex dump ECU memory (hex addr, decimal length)\n");
    printf("cmd:XXXX         - Send raw KWP2000 command (hex)\n");
    printf("raw:XXXX         - Same, but dump the unparsed reply bytes\n");
    printf("heartbeat:MS     - Set keep-alive interval in ms\n");
    printf("baud:N           - Set K-line bit rate (e.g. baud:57600)\n");
    printf("proto-test       - Run the host-link framing self-test\n");
    printf("help             - Show this help\n");
    printf("------------------------\n\n");
}

typedef struct {
    const char *text;
    CommandId id;
} CommandName;

static const CommandName command_names[] = {
    {"connect",                CMD_CONNECT},
    {"disconnect",             CMD_DISCONNECT},
    {"ecu-id",                 CMD_ECU_ID},
    {"read-dtcs",              CMD_READ_DTCS},
    {"clear-dtcs",             CMD_CLEAR_DTCS},
    {"diag-session",           CMD_DIAG_SESSION},
    {"load-handler",           CMD_LOAD_HANDLER},
    {"start-logging",          CMD_START_LOGGING},
    {"read-log",               CMD_READ_LOG},
    {"stream-off",             CMD_STREAM_STOP},
};

// Parse "cmd:1A9B" / "raw:1A9B" into a frame payload.
static bool parse_hex_command(const char *hex_str, Command *out) {
    size_t hex_len = strlen(hex_str);

    if (hex_len == 0 || hex_len % 2 != 0) {
        printf("Error: Hex string must have even number of characters (length: %zu)\n", hex_len);
        return false;
    }

    // The console accepts a 512-char line, which is more bytes than a message
    // slot holds. Reject rather than truncate - a silently shortened KWP frame
    // is a confusing way to fail.
    if (hex_len / 2 > MAX_MESSAGE_SIZE) {
        printf("Error: command is %zu bytes, max %u\n",
               hex_len / 2, (unsigned)MAX_MESSAGE_SIZE);
        return false;
    }

    out->payload_length = (uint8_t)(hex_len / 2);
    for (size_t i = 0; i < hex_len; i += 2) {
        char hex_byte[3] = {hex_str[i], hex_str[i + 1], '\0'};
        out->payload[i / 2] = (uint8_t)strtol(hex_byte, NULL, 16);
    }
    return true;
}

// Returns false when the line was handled locally (help, parse error) or was
// not a command at all.
static bool parse_command(const char *text, Command *out) {
    memset(out, 0, sizeof(*out));

    for (size_t i = 0; i < sizeof(command_names) / sizeof(command_names[0]); ++i) {
        if (strcmp(text, command_names[i].text) == 0) {
            out->id = command_names[i].id;
            return true;
        }
    }

    if (strncmp(text, "cmd:", 4) == 0 || strncmp(text, "raw:", 4) == 0) {
        const bool is_raw = (text[0] == 'r');
        out->id = is_raw ? CMD_RAW_DUMP : CMD_RAW_KWP;

        if (!parse_hex_command(text + 4, out)) return false;

        printf("Sending %s command: ", is_raw ? "raw" : "custom");
        for (size_t i = 0; i < out->payload_length; i++) {
            printf("%02X ", out->payload[i]);
        }
        printf("\n");
        return true;
    }

    // "stream-on" / "stream-on:20". Bare form picks a rate that is readable on
    // a console rather than the fastest possible - ask for 0 explicitly.
    if (strcmp(text, "stream-on") == 0 || strncmp(text, "stream-on:", 10) == 0) {
        out->id = CMD_STREAM_START;
        out->value = (text[9] == ':') ? (uint32_t)strtoul(text + 10, NULL, 10)
                                      : DEFAULT_STREAM_INTERVAL_MS;
        return true;
    }

    if (strncmp(text, "set-vars:", 9) == 0) {
        out->id = CMD_SET_LOG_VARS;
        if (!parse_hex_command(text + 9, out)) return false;
        if (out->payload_length % 3 != 0) {
            printf("Error: variable list must be whole 3-byte addresses\n");
            return false;
        }
        printf("Setting %u logged variable(s)\n", out->payload_length / 3);
        return true;
    }

    // "dump:387A00:256" - read a block of ECU memory as a hex dump
    if (strncmp(text, "dump:", 5) == 0) {
        const char *colon = strchr(text + 5, ':');
        if (colon == NULL) {
            printf("Error: usage dump:ADDR:LEN, e.g. dump:387A00:256\n");
            return false;
        }
        const uint32_t address = (uint32_t)strtoul(text + 5, NULL, 16);
        const uint32_t length = (uint32_t)strtoul(colon + 1, NULL, 10);
        if (length == 0 || length > 0xFFFF) {
            printf("Error: length must be 1..65535\n");
            return false;
        }

        out->id = CMD_DUMP_MEMORY;
        out->payload[0] = (address >> 16) & 0xFF;
        out->payload[1] = (address >> 8) & 0xFF;
        out->payload[2] = address & 0xFF;
        out->payload[3] = (length >> 8) & 0xFF;
        out->payload[4] = length & 0xFF;
        out->payload_length = 5;
        printf("Dumping %u bytes from 0x%06X\n", (unsigned)length, (unsigned)address);
        return true;
    }

    // "baud:57600" - follow the ECU after it switches rate mid-session
    if (strncmp(text, "baud:", 5) == 0) {
        uint32_t baud = (uint32_t)strtoul(text + 5, NULL, 10);
        if (baud == 0) {
            printf("Error: invalid baud rate\n");
            return false;
        }
        out->id = CMD_SET_BAUD;
        out->value = baud;
        return true;
    }

    // "heartbeat:60000" - park the keep-alive so it can't consume the first
    // service call after a handler redirect, which is the one that runs init.
    if (strncmp(text, "heartbeat:", 10) == 0) {
        out->id = CMD_SET_HEARTBEAT;
        out->value = (uint32_t)strtoul(text + 10, NULL, 10);
        return true;
    }

    printf("Unknown command: \"%s\". Type 'help' for available commands.\n", text);
    return false;
}

static void process_command(const char *text) {
    if (strcmp(text, "help") == 0) {
        print_help();
        return;
    }

    // Pure software check with no ECU involved, so it is handled here rather
    // than going through the command layer.
    if (strcmp(text, "proto-test") == 0) {
        proto_selftest();
        return;
    }

    printf("Command received: \"%s\"\n", text);

    Command cmd;
    if (!parse_command(text, &cmd)) return;

    switch (command_execute(&cmd)) {
        case COMMAND_OK:
            break;
        case COMMAND_QUEUE_FULL:
            printf("Error: command queue full - core 0 is not keeping up\n");
            break;
        case COMMAND_UNKNOWN:
            printf("Error: command not implemented\n");
            break;
    }
}

// --- core 0 messages -> console -------------------------------------------

// One message from core 0. The drain loop lives in host.c, which fans each
// message out to every frontend - the queue has a single consumer by design.
void console_on_message(const BufferMessage *msg) {
    {
        switch (msg->messageType) {
            case MSG_ACK:
                if (msg->length > 0 && msg->data[0] == 1) {
                    printf("ECU connected successfully\n");
                } else {
                    printf("Command acknowledged\n");
                }
                break;

            case MSG_NACK:
                printf("Command failed with error code: %d\n",
                       msg->length > 0 ? msg->data[0] : 0);
                break;

            case MSG_ECU_DATA:
                printf("ECU Response Data (%d bytes): ", msg->length);
                for (size_t i = 0; i < msg->length; i++) {
                    printf("%02X ", msg->data[i]);
                }
                printf("\n");

                printf("ECU Response ASCII: \"");
                for (size_t i = 0; i < msg->length; i++) {
                    if (msg->data[i] >= 32 && msg->data[i] <= 126) {
                        putchar(msg->data[i]);
                    } else {
                        putchar('.');
                    }
                }
                printf("\"\n");
                break;

            case MSG_SAMPLE: {
                if (msg->length < 6) break;
                const uint16_t seq = ((uint16_t)msg->data[0] << 8) | msg->data[1];
                const uint32_t t_ms = ((uint32_t)msg->data[2] << 24) |
                                      ((uint32_t)msg->data[3] << 16) |
                                      ((uint32_t)msg->data[4] << 8)  |
                                      ((uint32_t)msg->data[5]);
                printf("S %5u %8u ms |", seq, (unsigned)t_ms);
                for (size_t i = 6; i < msg->length; i++) {
                    printf(" %02X", msg->data[i]);
                }
                printf("\n");
                break;
            }

            case MSG_ECU_DISCONNECTED:
                printf("ECU disconnected\n");
                break;

            // Core 0 never prints directly (see log.h) - it queues lines here.
            case MSG_LOG:
                printf("%.*s\n", (int)msg->length, (const char *)msg->data);
                break;

            default:
                printf("Unknown response type: %d\n", msg->messageType);
                break;
        }
    }
}

void console_poll_input(void) {
    static char cmd_buffer[512];
    static int buf_pos = 0;
    int c;

    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\n' || c == '\r') {
            if (buf_pos > 0) {
                cmd_buffer[buf_pos] = '\0';
                process_command(cmd_buffer);
                buf_pos = 0;
                return;
            }
        } else if (buf_pos < (int)sizeof(cmd_buffer) - 1 && c >= 32 && c <= 126) {
            cmd_buffer[buf_pos++] = (char)c;
        }
    }
}

void console_init(void) {
    printf("Console ready. Type 'help' for commands.\n");
}
