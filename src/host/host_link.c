#include "host_link.h"
#include "proto.h"
#include "command.h"
#include "messages.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#define HOST_UART        uart0
#define HOST_UART_TX_PIN 0
#define HOST_UART_RX_PIN 1
#define HOST_UART_BAUD   921600

// Typed on the USB port to get the text console back without replugging. Framed
// traffic can only contain 0x00 as a delimiter, so a run of printable bytes this
// specific does not occur inside a frame.
static const char USB_ESCAPE[] = "console";

static ProtoDecoder decoder;
static uint8_t tx_sequence = 0;
static bool usb_mode = false;
static uint8_t escape_matched = 0;

void host_link_init(void) {
    uart_init(HOST_UART, HOST_UART_BAUD);
    gpio_set_function(HOST_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(HOST_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(HOST_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(HOST_UART, true);
    uart_set_hw_flow(HOST_UART, false, false);

    proto_decoder_reset(&decoder);
    decoder.crc_errors = 0;
    decoder.overflow_errors = 0;
    decoder.short_frames = 0;
}

static void send_frame(uint8_t type, const uint8_t *data, size_t length) {
    uint8_t wire[PROTO_MAX_FRAME];
    const size_t n = proto_encode(type, tx_sequence, data, length, wire, sizeof(wire));
    if (n == 0) return;  // payload too large to frame; dropping beats truncating
    tx_sequence++;

    if (usb_mode) {
        // putchar_raw, not putchar: the default stdio driver rewrites 0x0A to
        // CRLF, which inside a frame is a corrupt byte and a failed CRC. The
        // explicit flush is because stdio's USB driver otherwise waits for a
        // newline that binary data never contains.
        for (size_t i = 0; i < n; i++) putchar_raw(wire[i]);
        stdio_flush();
        return;
    }

    // With a receiver present this is only ~0.2ms for a sample frame at 921600.
    uart_write_blocking(HOST_UART, wire, n);
}

void host_link_on_message(const BufferMessage *msg) {
    switch (msg->messageType) {
        case MSG_SAMPLE:
            send_frame(PROTO_SAMPLE, msg->data, msg->length);
            break;

        case MSG_LOG:
            send_frame(PROTO_LOG, msg->data, msg->length);
            break;

        case MSG_ECU_DATA:
            send_frame(PROTO_RESPONSE, msg->data, msg->length);
            break;

        // The outcome of an operation, not a receipt for a command frame - see
        // the event codes in proto.h for why the two are kept apart.
        case MSG_ACK:
        case MSG_NACK: {
            const uint8_t event[2] = {
                (uint8_t)(msg->messageType == MSG_ACK ? PROTO_EVENT_OPERATION_OK
                                                      : PROTO_EVENT_OPERATION_FAILED),
                (uint8_t)(msg->length > 0 ? msg->data[0] : 0),
            };
            send_frame(PROTO_EVENT, event, sizeof(event));
            break;
        }

        case MSG_ECU_DISCONNECTED: {
            const uint8_t event[2] = { PROTO_EVENT_LINK_LOST, 0 };
            send_frame(PROTO_EVENT, event, sizeof(event));
            break;
        }

        default:
            break;
    }
}

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

// A CMD frame is [CommandId][args]. Commands carrying a scalar take 4 big-endian
// bytes; the rest take their argument bytes verbatim.
static void handle_command_frame(const ProtoFrame *frame) {
    if (frame->length < 1) return;

    Command cmd = {0};
    cmd.id = (CommandId)frame->data[0];

    const uint8_t *args = &frame->data[1];
    const size_t args_length = frame->length - 1;

    switch (cmd.id) {
        case CMD_SET_HEARTBEAT:
        case CMD_SET_BAUD:
        case CMD_STREAM_START:
            if (args_length >= 4) cmd.value = read_be32(args);
            break;

        default:
            if (args_length > 0) {
                size_t n = args_length;
                if (n > MAX_MESSAGE_SIZE) n = MAX_MESSAGE_SIZE;
                memcpy(cmd.payload, args, n);
                cmd.payload_length = (uint8_t)n;
            }
            break;
    }

    const CommandStatus status = command_execute(&cmd);

    const uint8_t reply[2] = { (uint8_t)cmd.id, (uint8_t)status };
    send_frame(PROTO_ACK, reply, sizeof(reply));
}

void host_link_set_usb(bool on) {
    if (usb_mode == on) return;
    usb_mode = on;
    escape_matched = 0;
    // The half-decoded frame belonged to the other transport.
    proto_decoder_reset(&decoder);
}

bool host_link_usb_active(void) {
    return usb_mode;
}

static void feed(uint8_t byte) {
    ProtoFrame frame;
    if (proto_decode_byte(&decoder, byte, &frame) && frame.type == PROTO_CMD) {
        handle_command_frame(&frame);
    }
}

void host_link_poll(void) {
    if (usb_mode) {
        // console_poll_input() has stood down, so this owns USB input. Each byte
        // goes to the decoder and to the escape matcher; the matcher is what
        // lets a human take the port back with a typed word.
        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            const uint8_t byte = (uint8_t)c;

            escape_matched = (byte == (uint8_t)USB_ESCAPE[escape_matched])
                                 ? (uint8_t)(escape_matched + 1)
                                 : (byte == (uint8_t)USB_ESCAPE[0] ? 1 : 0);
            if (USB_ESCAPE[escape_matched] == '\0') {
                host_link_set_usb(false);
                printf("\nhost link back on uart0; console live. Type 'help'.\n");
                return;
            }

            feed(byte);
        }
        return;
    }

    // Drain the whole FIFO each time rather than a byte per loop: at 921600 the
    // 32-byte FIFO fills in ~0.35ms, well under the core 1 loop period.
    while (uart_is_readable(HOST_UART)) {
        feed(uart_getc(HOST_UART));
    }
}
