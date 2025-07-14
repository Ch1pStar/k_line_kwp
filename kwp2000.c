#include "config.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dashboard_state_machine.h"

extern unsigned char _binary_handler_bin_start[];
extern unsigned char _binary_handler_bin_end[];

void load_handler(DashboardStateMachine* sm) {
    size_t size = _binary_handler_bin_end - _binary_handler_bin_start;
    const unsigned char *data = _binary_handler_bin_start;
    uint8_t chunk_size = 0x08;
    const uint8_t num_chunks = (size + chunk_size - 1) / chunk_size;
    uint32_t start_address = 0x387acc;

    // printf("----------------------------------------------\n");
    // printf("Data(size: %02X): ", size);
    // for(uint8_t i = 0; i < size; i++) {
    //     printf("%02X ", data[i]);
    // }
    // printf("\n");
    // printf("----------------------------------------------\n");

    printf("Loading handler into ECU RAM. Size: %zu, Number of chunks: %u\n", size, num_chunks);

    for (uint8_t i = 0; i < num_chunks; i++) {
        if(i+1 == num_chunks) {
            chunk_size = size - (i * chunk_size);
        }

        uint32_t chunk_address = start_address + (i * chunk_size);
        write_memory_chunk(sm, chunk_address, data, chunk_size, i);

        sleep_ms(100);
    }
}

void fill_distibutor_table(DashboardStateMachine* sm) {
    // uint8_t handler_address[4] = {0xCC, 0x7A, 0x38, 0x00};
    uint8_t data_size = 4;
    uint8_t new_distributor_table_address[3] = {0x38, 0x7a, 0x00};
    uint8_t new_distributor_end = 0xC0/data_size;
    uint8_t handler_address[4] = {0x00, 0x38, 0x7a, 0xcc};

    uint8_t command_length = 1 + 3 + 1 + data_size; // sid len(1) + address len(3) + data size(1) + data(size)
    
    for(uint8_t i = 0; i < new_distributor_end; i++) {
        BufferMessage cmdMsg = {
            .messageType = MSG_COMMAND,
            .length = command_length,
        };
    
        cmdMsg.data[0] = 0x3d; // Write memory service id

        // target address
        cmdMsg.data[1] = new_distributor_table_address[0];
        cmdMsg.data[2] = new_distributor_table_address[1];
        cmdMsg.data[3] = new_distributor_table_address[2] + (i*data_size);

        // data size
        cmdMsg.data[4] = data_size;

        // data
        memcpy(&cmdMsg.data[5], handler_address, 4); // data

        ringbuffer_push(sm->txBuffer, &cmdMsg);

        sleep_ms(100);
    }

}


uint8_t calculate_checksum(const KWP2000Service* service) {
    uint8_t csum = 0;
    csum += (1 + service->dataLength);
    csum += service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        csum += service->dataBytes[i];
    }

    return csum;
}

ResponseStatus _do_read_response(size_t commandLength, KWP2000Response* response, bool silent) {
    response->dataSize = 0;

    // Skip echoed command bytes
    for (size_t i = 0; i < commandLength; ++i) {
        uint32_t byte = read_byte_timeout(100000);  // 100ms timeout
        if (byte == UINT32_MAX) {
            if (!silent) {
                printf("Timeout while reading echo bytes\n");
            }
            return RESPONSE_ERROR;
        }
    }

    // Read response length
    uint32_t respLen = read_byte_timeout(100000);
    if (respLen == UINT32_MAX) {
        if (!silent) {
            printf("Timeout while reading response length\n");
        }
        return RESPONSE_ERROR;
    }
    uint8_t responseLength = (uint8_t)respLen;
    uint8_t checksum = responseLength;

    // Read response status
    uint32_t respStatus = read_byte_timeout(100000);
    if (respStatus == UINT32_MAX) {
        if (!silent) {
            printf("Timeout while reading response status\n");
        }
        return RESPONSE_ERROR;
    }
    uint8_t responseStatus = (uint8_t)respStatus;
    checksum += responseStatus;

    if (responseStatus == 0x7f) {
        if (!silent) {
            printf("Error in response, length: %02x\n", responseLength);
        }
        printf("Error response: %02x ", responseStatus);
        if(responseLength > 0) {
            for (size_t i = 0; i < responseLength; ++i) {
                uint32_t dataByte = read_byte_timeout(100000);
                if (dataByte == UINT32_MAX) {
                    printf("Timeout while reading error data byte %zu\n", i);
                }else{
                    printf("%02X ", dataByte);
                }
            }
            printf("\n");
        }

        return RESPONSE_ERROR;
    } else {
        if (!silent) {
            printf("Response status: %02x, length: %02x\n", responseStatus, responseLength);
        }
    }

    // Read the data bytes
    for (size_t i = 0; i < responseLength - 1; ++i) {
        if (i < MAX_RESPONSE_SIZE) {
            uint32_t dataByte = read_byte_timeout(100000);
            if (dataByte == UINT32_MAX) {
                if (!silent) {
                    printf("Timeout while reading data byte %zu\n", i);
                }
                return RESPONSE_ERROR;
            }
            checksum += (uint8_t)dataByte;
            response->data[i] = (uint8_t)dataByte;
            response->dataSize++;
        } else {
            // Exceeds buffer, read remaining to maintain protocol but do not store
            uint32_t dataByte = read_byte_timeout(100000);
            if (dataByte == UINT32_MAX) {
                if (!silent) {
                    printf("Timeout while reading overflow byte\n");
                }
                return RESPONSE_ERROR;
            }
            return RESPONSE_OVERFLOW;
        }
    }

    // Read and validate the checksum byte
    uint32_t recvChecksum = read_byte_timeout(100000);
    if (recvChecksum == UINT32_MAX) {
        if (!silent) {
            printf("Timeout while reading checksum\n");
        }
        return RESPONSE_ERROR;
    }

    if (checksum == (uint8_t)recvChecksum) {
        if (!silent) {
            printf("Response checksum valid.\n");
        }
        return RESPONSE_OK;
    } else {
        if (!silent) {
            printf("Response checksum invalid. Calculated: %02x, Received: %02x\n", 
               checksum, (uint8_t)recvChecksum);
        }
        return RESPONSE_CHECKSUM_INVALID;
    }
}

ResponseStatus read_response(size_t commandLength, KWP2000Response* response) {
    return _do_read_response(commandLength, response, false);
}

ResponseStatus read_response_silent(size_t commandLength, KWP2000Response* response) {
    return _do_read_response(commandLength, response, true);
}

void send_packet(const KWP2000Packet* packet) {
    // printf("Sending packet: %x %x", packet->length, packet->serviceId);
    // for (size_t i = 0; i < packet->length - 1; ++i) {
    //     printf(" %x", packet->dataBytes[i]);
    // }
    // printf(" %x\n", packet->checksum);

    send_byte(packet->length);
    send_byte(packet->serviceId);

    for (size_t i = 0; i < packet->length - 1; ++i) {
        send_byte(packet->dataBytes[i]);
    }

    send_byte(packet->checksum);
}

void print_response(const KWP2000Response* response) {
    printf("Response Data (size: %zu): ", response->dataSize);
    for (size_t i = 0; i < response->dataSize; ++i) {
        printf("%02X ", response->data[i]);
    }
    printf("\n");
}

void print_str_response(const KWP2000Response* response) {
    printf("Response String: \"");
    for (size_t i = 0; i < response->dataSize; ++i) {
        if (isprint(response->data[i])) {
            putchar(response->data[i]);
        } else {
            putchar('.'); // Replace non-printable characters with '.'
        }
    }
    printf("\"\n");
}

uint8_t parse_dtcs_response(const KWP2000Response* response, DTCData* dtcArray, size_t dtcArraySize) {
    const uint8_t num_dtcs = response->data[0];

    printf("%d Faults Found:\n", num_dtcs);

    for (size_t i = 0; i < num_dtcs; i++) {
        size_t offset = 1 + i * 3; // Calculate the offset for each DTC (skip the count byte)
        dtcArray[i].highByte = response->data[offset];
        dtcArray[i].lowByte = response->data[offset + 1];
        dtcArray[i].status = response->data[offset + 2];
    }

    return num_dtcs;
}

void print_dtc_status(uint8_t status) {
    printf("\tStatus: ");
    switch (status & 0x0F) { // Masking with 0x0F to get the lower 4 bits
        case 0x00:
            printf("No fault symptom available");
            break;
        case 0x01:
            printf("Above maximum threshold");
            break;
        case 0x02:
            printf("Below minimum threshold");
            break;
        case 0x04:
            printf("No signal");
            break;
        case 0x08:
            printf("Invalid signal");
            break;
        default:
            printf("Unknown");
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

// Helper function to convert DTC bytes into a human-readable DTC code
void convert_dtc_to_readable_format(uint8_t highByte, uint8_t lowByte, char *dtcString) {
    // Assuming the DTC format is ISO 14229-1
    uint8_t firstLetterIndex = (highByte >> 6) & 0x03; // First 2 bits
    char firstLetter = 'P'; // Default to powertrain
    switch (firstLetterIndex) {
        case 0: firstLetter = 'P'; break; // Powertrain
        case 1: firstLetter = 'C'; break; // Chassis
        case 2: firstLetter = 'B'; break; // Body
        case 3: firstLetter = 'U'; break; // Network
    }

    sprintf(dtcString, "%c%02X%02X", firstLetter, highByte & 0x3F, lowByte);
}

// Function to print DTCs in a human-readable format
void print_dtc_data(DTCData *dtcs, size_t numDtc) {
    for (size_t i = 0; i < numDtc; ++i) {
        char dtcString[6]; // DTC string format: C1234
        convert_dtc_to_readable_format(dtcs[i].highByte, dtcs[i].lowByte, dtcString);
        printf("%s, Status: 0x%02X\n", dtcString, dtcs[i].status);
        print_dtc_status(dtcs[i].status);
    }
}

size_t _do_build_packet(const KWP2000Service* service, bool silent) {
    KWP2000Packet packet;

    packet.length = 1 + service->dataLength; // Only service ID + data bytes, without checksum
    packet.serviceId = service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        packet.dataBytes[i] = service->dataBytes[i];
    }
    packet.checksum = calculate_checksum(service);

    if (!silent) {
        // Display the packet
        printf("KWP2000 Packet: Length=0x%02X, Service ID=0x%02X, Data=", packet.length, packet.serviceId);
        for (size_t i = 0; i < service->dataLength; ++i) {
            printf("0x%02X ", packet.dataBytes[i]);
        }
        printf("Checksum=0x%02X\n", packet.checksum);
    }

    send_packet(&packet);

    return (size_t)packet.length+2;
}

size_t build_packet(const KWP2000Service* service) {
    return _do_build_packet(service, false);
}

size_t build_packet_silent(const KWP2000Service* service) {
    return _do_build_packet(service, true);
}

void clear_dtcs() {
    KWP2000Service  clear_dtcs_service = {0x14, {0xff, 0x00}, 2};
    KWP2000Response clear_dtcs_response;

    size_t packet_len = build_packet(&clear_dtcs_service);
    read_response(packet_len, &clear_dtcs_response);
}

void read_dtcs() {
    KWP2000Service  read_dtcs_service = {0x18, {0x00, 0xff, 0x00}, 3};
    KWP2000Response dtcs_response;
    size_t packet_len = build_packet(&read_dtcs_service);
    read_response(packet_len, &dtcs_response);

    print_response(&dtcs_response);
    print_str_response(&dtcs_response);

    DTCData dtcArray[MAX_DTC_COUNT];
    size_t numDTCs = parse_dtcs_response(&dtcs_response, dtcArray, MAX_DTC_COUNT);

    print_dtc_data(dtcArray, numDTCs);
}

void read_ecu_id() {
    KWP2000Service ecu_id_service = {0x1A, {0x9B}, 1}; // ECU ID
    KWP2000Response ecu_id_response;
    size_t packet_len = build_packet(&ecu_id_service);

    read_response(packet_len, &ecu_id_response);
    print_str_response(&ecu_id_response);
}
