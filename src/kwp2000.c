#include "kwp2000.h"
#include "uart.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>

typedef struct {
    uint8_t length;
    uint8_t serviceId;
    uint8_t dataBytes[MAX_DATA_SIZE];
    uint8_t checksum;
} KWP2000Packet;

static uint8_t calculate_checksum(const KWP2000Service *service) {
    uint8_t csum = 0;
    csum += (1 + service->dataLength);
    csum += service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        csum += service->dataBytes[i];
    }
    return csum;
}

static void send_packet(const KWP2000Packet *packet) {
    uart_send_byte(packet->length);
    uart_send_byte(packet->serviceId);
    for (size_t i = 0; i < packet->length - 1; ++i) {
        uart_send_byte(packet->dataBytes[i]);
    }
    uart_send_byte(packet->checksum);
}

size_t kwp2000_send(const KWP2000Service *service, bool silent) {
    KWP2000Packet packet;
    packet.length = 1 + service->dataLength;
    packet.serviceId = service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        packet.dataBytes[i] = service->dataBytes[i];
    }
    packet.checksum = calculate_checksum(service);

    if (!silent) {
        printf("KWP2000 Packet: Length=0x%02X, Service ID=0x%02X, Data=", packet.length, packet.serviceId);
        for (size_t i = 0; i < service->dataLength; ++i) {
            printf("0x%02X ", packet.dataBytes[i]);
        }
        printf("Checksum=0x%02X\n", packet.checksum);
    }

    send_packet(&packet);
    return (size_t)packet.length + 2;
}

ResponseStatus kwp2000_read_response(size_t command_length, KWP2000Response *response, bool silent) {
    response->dataSize = 0;

    // Skip echoed command bytes
    for (size_t i = 0; i < command_length; ++i) {
        uint32_t byte = uart_read_byte_timeout(100000);
        if (byte == UART_TIMEOUT) {
            if (!silent) printf("Timeout while reading echo bytes\n");
            return RESPONSE_ERROR;
        }
    }

    // Read response length
    uint32_t resp_len = uart_read_byte_timeout(100000);
    if (resp_len == UART_TIMEOUT) {
        if (!silent) printf("Timeout while reading response length\n");
        return RESPONSE_ERROR;
    }
    uint8_t response_length = (uint8_t)resp_len;
    uint8_t checksum = response_length;

    // Read response status
    uint32_t resp_status = uart_read_byte_timeout(100000);
    if (resp_status == UART_TIMEOUT) {
        if (!silent) printf("Timeout while reading response status\n");
        return RESPONSE_ERROR;
    }
    uint8_t response_status = (uint8_t)resp_status;
    checksum += response_status;

    if (response_status == 0x7f) {
        if (!silent) printf("Error in response, length: %02x\n", response_length);
        printf("Error response: %02x ", response_status);
        if (response_length > 0) {
            for (size_t i = 0; i < response_length; ++i) {
                uint32_t data_byte = uart_read_byte_timeout(100000);
                if (data_byte == UART_TIMEOUT) {
                    printf("Timeout while reading error data byte %zu\n", i);
                } else {
                    printf("%02X ", data_byte);
                }
            }
            printf("\n");
        }
        return RESPONSE_ERROR;
    } else {
        if (!silent) {
            printf("Response status: %02x, length: %02x\n", response_status, response_length);
        }
    }

    // Read data bytes
    for (size_t i = 0; i < response_length - 1; ++i) {
        uint32_t data_byte = uart_read_byte_timeout(100000);
        if (data_byte == UART_TIMEOUT) {
            if (!silent) printf("Timeout while reading data byte %zu\n", i);
            return RESPONSE_ERROR;
        }
        if (i < MAX_RESPONSE_SIZE) {
            checksum += (uint8_t)data_byte;
            response->data[i] = (uint8_t)data_byte;
            response->dataSize++;
        } else {
            return RESPONSE_OVERFLOW;
        }
    }

    // Read and validate checksum
    uint32_t recv_checksum = uart_read_byte_timeout(100000);
    if (recv_checksum == UART_TIMEOUT) {
        if (!silent) printf("Timeout while reading checksum\n");
        return RESPONSE_ERROR;
    }

    if (checksum == (uint8_t)recv_checksum) {
        if (!silent) printf("Response checksum valid.\n");
        return RESPONSE_OK;
    } else {
        if (!silent) {
            printf("Response checksum invalid. Calculated: %02x, Received: %02x\n",
                   checksum, (uint8_t)recv_checksum);
        }
        return RESPONSE_CHECKSUM_INVALID;
    }
}

ResponseStatus kwp2000_execute(const KWP2000Service *service, KWP2000Response *response, bool silent) {
    size_t packet_len = kwp2000_send(service, silent);
    return kwp2000_read_response(packet_len, response, silent);
}

void kwp2000_print_response(const KWP2000Response *response) {
    printf("Response Data (size: %zu): ", response->dataSize);
    for (size_t i = 0; i < response->dataSize; ++i) {
        printf("%02X ", response->data[i]);
    }
    printf("\n");
}

void kwp2000_print_str_response(const KWP2000Response *response) {
    printf("Response String: \"");
    for (size_t i = 0; i < response->dataSize; ++i) {
        if (isprint(response->data[i])) {
            putchar(response->data[i]);
        } else {
            putchar('.');
        }
    }
    printf("\"\n");
}

uint8_t kwp2000_parse_dtcs(const KWP2000Response *response, DTCData *dtc_array, size_t dtc_array_size) {
    const uint8_t num_dtcs = response->data[0];
    printf("%d Faults Found:\n", num_dtcs);

    for (size_t i = 0; i < num_dtcs && i < dtc_array_size; i++) {
        size_t offset = 1 + i * 3;
        dtc_array[i].highByte = response->data[offset];
        dtc_array[i].lowByte = response->data[offset + 1];
        dtc_array[i].status = response->data[offset + 2];
    }

    return num_dtcs;
}

static void print_dtc_status(uint8_t status) {
    printf("\tStatus: ");
    switch (status & 0x0F) {
        case 0x00: printf("No fault symptom available"); break;
        case 0x01: printf("Above maximum threshold"); break;
        case 0x02: printf("Below minimum threshold"); break;
        case 0x04: printf("No signal"); break;
        case 0x08: printf("Invalid signal"); break;
        default:   printf("Unknown");
    }

    printf(",\n\tTest %s", (status & 0x10) ? "not complete" : "complete or not applicable");

    if ((status & 0x60) == 0x00) {
        printf(",\n\tNo DTC detected or stored");
    } else if ((status & 0x60) == 0x20) {
        printf(",\n\tDTC not present at time of request but stored");
    } else if ((status & 0x60) == 0x40) {
        printf(",\n\tDTC maturing - intermittent, insufficient data for storage");
    } else if ((status & 0x60) == 0x60) {
        printf(",\n\tDTC present at time of request and stored");
    }

    printf(",\n\tWarning Lamp %s\n", (status & 0x80) ? "enabled" : "disabled");
}

static void convert_dtc_to_readable(uint8_t high_byte, uint8_t low_byte, char *dtc_string) {
    uint8_t first_letter_index = (high_byte >> 6) & 0x03;
    char first_letter;
    switch (first_letter_index) {
        case 0: first_letter = 'P'; break;
        case 1: first_letter = 'C'; break;
        case 2: first_letter = 'B'; break;
        case 3: first_letter = 'U'; break;
        default: first_letter = '?'; break;
    }
    sprintf(dtc_string, "%c%02X%02X", first_letter, high_byte & 0x3F, low_byte);
}

void kwp2000_print_dtcs(DTCData *dtcs, size_t num_dtcs) {
    for (size_t i = 0; i < num_dtcs; ++i) {
        char dtc_string[6];
        convert_dtc_to_readable(dtcs[i].highByte, dtcs[i].lowByte, dtc_string);
        printf("%s, Status: 0x%02X\n", dtc_string, dtcs[i].status);
        print_dtc_status(dtcs[i].status);
    }
}
