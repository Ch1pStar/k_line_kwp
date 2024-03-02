#include "config.h"

uint8_t calculate_checksum(const KWP2000Service* service) {
    uint8_t csum = 0;
    csum += (1 + service->dataLength);
    csum += service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        csum += service->dataBytes[i];
    }

    return csum;
}

ResponseStatus read_response(size_t commandLength, KWP2000Response* response) {
    response->dataSize = 0;

    // Skip echoed command bytes
    for (size_t i = 0; i < commandLength; ++i) {
        read_byte();
    }

    uint8_t checksum = 0;
    uint8_t responseLength = read_byte();
    checksum += responseLength;

    uint8_t responseStatus = read_byte();
    checksum += responseStatus;

    if (responseStatus == 0x7f) {
        printf("Error in response\n");
        return RESPONSE_ERROR;
    }else{
        printf("Response status: %02x\n", responseStatus);
    }

    // Read the data bytes
    for (size_t i = 0; i < responseLength - 1; ++i) {
        if (i < MAX_RESPONSE_SIZE) {
            uint8_t dataByte = read_byte();
            checksum += dataByte;
            response->data[i] = dataByte;
            response->dataSize++;
        } else {
            // Exceeds buffer, read remaining to maintain protocol but do not store
            read_byte();
            return RESPONSE_OVERFLOW;
        }
    }

    //read and validate the checksum byte
    uint8_t receivedChecksum = read_byte();
    if (checksum == receivedChecksum) {
        printf("Response checksum valid.\n");
        return RESPONSE_OK;
    } else {
        printf("Response checksum invalid. Calculated: %02x, Received: %02x\n", checksum, receivedChecksum);
        return RESPONSE_CHECKSUM_INVALID;
    }
}

void send_packet(const KWP2000Packet* packet) {
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

    printf(", Test %s", (status & 0x10) ? "not complete" : "complete or not applicable");

    if ((status & 0x60) == 0x00) {
        printf(", No DTC detected or stored");
    } else if ((status & 0x60) == 0x20) {
        printf(", DTC not present at time of request but stored");
    } else if ((status & 0x60) == 0x40) {
        printf(", DTC maturing - intermittent, insufficient data for storage");
    } else if ((status & 0x60) == 0x60) {
        printf(", DTC present at time of request and stored");
    }

    printf(", Warning Lamp %s\n", (status & 0x80) ? "enabled" : "disabled");
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
        printf("DTC %zu: %s, Status: 0x%02X\n", i + 1, dtcString, dtcs[i].status);
        print_dtc_status(dtcs[i].status);
    }
}

uint32_t build_packet(const KWP2000Service* service) {
    KWP2000Packet packet;

    packet.length = 1 + service->dataLength; // Only service ID + data bytes, without checksum
    packet.serviceId = service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        packet.dataBytes[i] = service->dataBytes[i];
    }
    packet.checksum = calculate_checksum(service);

    // Display the packet
    printf("KWP2000 Packet: Length=0x%02X, Service ID=0x%02X, Data=", packet.length, packet.serviceId);
    for (size_t i = 0; i < service->dataLength; ++i) {
        printf("0x%02X ", packet.dataBytes[i]);
    }
    printf("Checksum=0x%02X\n", packet.checksum);

    send_packet(&packet);

    return packet.length+2;
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
    print_response(&ecu_id_response);
}
