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

// Flatten the packet into the exact byte sequence that goes on the wire.
static size_t build_frame(const KWP2000Packet *packet, uint8_t *out) {
    size_t n = 0;
    out[n++] = packet->length;
    out[n++] = packet->serviceId;
    for (size_t i = 0; i + 1 < packet->length; ++i) {
        out[n++] = packet->dataBytes[i];
    }
    out[n++] = packet->checksum;
    return n;
}

// K-line is half duplex on a single wire, so every byte we transmit lands back
// on RX. Transmitting the whole frame before reading overflows the PIO RX FIFO
// (~9 bytes including the shift register) and silently drops echo bytes, which
// desynchronises every field of the reply. Read each byte back as we send it.
static size_t send_packet(const KWP2000Packet *packet, bool silent) {
    uint8_t frame[MAX_DATA_SIZE + 4];
    const size_t frame_len = build_frame(packet, frame);

    for (size_t i = 0; i < frame_len; ++i) {
        uart_send_byte(frame[i]);

        uint32_t echo = uart_read_byte_timeout(100000);
        if (echo == UART_TIMEOUT) {
            if (!silent) printf("Echo timeout at byte %u of %u\n",
                                (unsigned)i, (unsigned)frame_len);
        } else if ((uint8_t)echo != frame[i]) {
            if (!silent) printf("Echo mismatch at byte %u: sent %02X, read %02X\n",
                                (unsigned)i, frame[i], (uint8_t)echo);
        }
    }

    return frame_len;
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

    // Echo is consumed inline by send_packet, so nothing is left for the reader
    // to skip. Returns the frame length for logging only.
    return send_packet(&packet, silent);
}

ResponseStatus kwp2000_read_response(size_t echo_bytes, KWP2000Response *response, bool silent) {
    response->dataSize = 0;

    // Skip echoed command bytes
    for (size_t i = 0; i < echo_bytes; ++i) {
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
        // Negative response: [len][7F][rejected SID][NRC][checksum]. Consume the
        // rest of the frame so the next read stays in sync, keeping the payload
        // (everything bar the trailing checksum) for the caller to inspect.
        response->dataSize = 0;
        for (size_t i = 0; i < response_length; ++i) {
            uint32_t data_byte = uart_read_byte_timeout(100000);
            if (data_byte == UART_TIMEOUT) {
                if (!silent) printf("Timeout while reading negative response byte %zu\n", i);
                return RESPONSE_ERROR;
            }
            if (i + 1 < response_length && response->dataSize < MAX_RESPONSE_SIZE) {
                response->data[response->dataSize++] = (uint8_t)data_byte;
            }
        }

        if (!silent) {
            printf("Negative response: 7F");
            for (size_t i = 0; i < response->dataSize; ++i) {
                printf(" %02X", response->data[i]);
            }
            if (response->dataSize >= 2) {
                printf("   (service 0x%02X rejected, NRC 0x%02X)",
                       response->data[0], response->data[1]);
            }
            printf("\n");
        }

        // The ECU answered, so the link is up - this is a rejection, not a fault.
        return RESPONSE_NEGATIVE;
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
    kwp2000_send(service, silent);
    // 0: the echo was already consumed byte-by-byte during the send.
    return kwp2000_read_response(0, response, silent);
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
    if (response->dataSize < 1) {
        printf("DTC response too short (%zu bytes)\n", response->dataSize);
        return 0;
    }

    // Response layout after the 0x58 service id: [count] then 3 bytes per DTC.
    const uint8_t reported = response->data[0];
    const size_t available = (response->dataSize - 1) / 3;

    size_t count = reported;
    if (count > available) count = available;
    if (count > dtc_array_size) count = dtc_array_size;

    // Don't trust the count blindly - a truncated or odd-length reply would
    // otherwise walk off the end of response->data.
    if (count != reported) {
        printf("DTC count mismatch: ECU reported %u, %zu usable in %zu response bytes\n",
               reported, count, response->dataSize);
    }

    for (size_t i = 0; i < count; i++) {
        size_t offset = 1 + i * 3;
        dtc_array[i].highByte = response->data[offset];
        dtc_array[i].lowByte = response->data[offset + 1];
        dtc_array[i].status = response->data[offset + 2];
    }

    return (uint8_t)count;
}

static void print_dtc_status(uint8_t status) {
    printf("      symptom : ");
    switch (status & 0x0F) {
        case 0x00: printf("no fault symptom available"); break;
        case 0x01: printf("above maximum threshold"); break;
        case 0x02: printf("below minimum threshold"); break;
        case 0x04: printf("no signal"); break;
        case 0x08: printf("invalid signal"); break;
        default:   printf("unknown (0x%X)", status & 0x0F); break;
    }
    printf("\n");

    printf("      state   : ");
    switch (status & 0x60) {
        case 0x00: printf("not detected or stored"); break;
        case 0x20: printf("stored, not present now (history/intermittent)"); break;
        case 0x40: printf("maturing - insufficient data to store"); break;
        default:   printf("present now and stored (ACTIVE)"); break;
    }
    printf("\n");

    printf("      test    : %s\n", (status & 0x10) ? "not complete" : "complete / not applicable");
    printf("      MIL     : %s\n", (status & 0x80) ? "on" : "off");
}

typedef struct {
    uint16_t code;              // raw 16-bit DTC as sent by the ECU
    const char *description;
} DTCDescription;

// ME7.5 sends DTCs in standard SAE J2012 form, so the raw 16-bit value maps
// straight onto the printed code: 0x0562 -> P0562. Only SAE-defined powertrain
// codes are listed. P1xxx and up are manufacturer specific and deliberately
// absent - add them here as they get confirmed against this ECU.
static const DTCDescription dtc_descriptions[] = {
    // Fuel and air metering
    {0x0100, "Mass or Volume Air Flow Circuit"},
    {0x0101, "Mass or Volume Air Flow Circuit Range/Performance"},
    {0x0102, "Mass or Volume Air Flow Circuit Low Input"},
    {0x0103, "Mass or Volume Air Flow Circuit High Input"},
    {0x0105, "Manifold Absolute Pressure / Barometric Pressure Circuit"},
    {0x0106, "MAP / Baro Pressure Circuit Range/Performance"},
    {0x0107, "MAP / Baro Pressure Circuit Low Input"},
    {0x0108, "MAP / Baro Pressure Circuit High Input"},
    {0x0110, "Intake Air Temperature Sensor 1 Circuit"},
    {0x0111, "Intake Air Temperature Sensor 1 Circuit Range/Performance"},
    {0x0112, "Intake Air Temperature Sensor 1 Circuit Low"},
    {0x0113, "Intake Air Temperature Sensor 1 Circuit High"},
    {0x0115, "Engine Coolant Temperature Circuit"},
    {0x0116, "Engine Coolant Temperature Circuit Range/Performance"},
    {0x0117, "Engine Coolant Temperature Circuit Low"},
    {0x0118, "Engine Coolant Temperature Circuit High"},
    {0x0120, "Throttle / Pedal Position Sensor A Circuit"},
    {0x0121, "Throttle / Pedal Position Sensor A Circuit Range/Performance"},
    {0x0122, "Throttle / Pedal Position Sensor A Circuit Low"},
    {0x0123, "Throttle / Pedal Position Sensor A Circuit High"},
    {0x0125, "Insufficient Coolant Temperature for Closed Loop Fuel Control"},
    {0x0130, "O2 Sensor Circuit (Bank 1 Sensor 1)"},
    {0x0131, "O2 Sensor Circuit Low Voltage (Bank 1 Sensor 1)"},
    {0x0132, "O2 Sensor Circuit High Voltage (Bank 1 Sensor 1)"},
    {0x0133, "O2 Sensor Circuit Slow Response (Bank 1 Sensor 1)"},
    {0x0134, "O2 Sensor Circuit No Activity Detected (Bank 1 Sensor 1)"},
    {0x0135, "O2 Sensor Heater Circuit (Bank 1 Sensor 1)"},
    {0x0136, "O2 Sensor Circuit (Bank 1 Sensor 2)"},
    {0x0140, "O2 Sensor Circuit No Activity Detected (Bank 1 Sensor 2)"},
    {0x0141, "O2 Sensor Heater Circuit (Bank 1 Sensor 2)"},
    {0x0170, "Fuel Trim (Bank 1)"},
    {0x0171, "System Too Lean (Bank 1)"},
    {0x0172, "System Too Rich (Bank 1)"},

    // Injector / boost
    {0x0201, "Injector Circuit - Cylinder 1"},
    {0x0202, "Injector Circuit - Cylinder 2"},
    {0x0203, "Injector Circuit - Cylinder 3"},
    {0x0204, "Injector Circuit - Cylinder 4"},
    {0x0234, "Turbocharger / Supercharger Overboost Condition"},
    {0x0235, "Turbocharger / Supercharger Boost Sensor A Circuit"},
    {0x0236, "Turbocharger / Supercharger Boost Sensor A Circuit Range/Performance"},
    {0x0237, "Turbocharger / Supercharger Boost Sensor A Circuit Low"},
    {0x0238, "Turbocharger / Supercharger Boost Sensor A Circuit High"},
    {0x0243, "Turbocharger / Supercharger Wastegate Solenoid A"},

    // Ignition / misfire
    {0x0300, "Random / Multiple Cylinder Misfire Detected"},
    {0x0301, "Cylinder 1 Misfire Detected"},
    {0x0302, "Cylinder 2 Misfire Detected"},
    {0x0303, "Cylinder 3 Misfire Detected"},
    {0x0304, "Cylinder 4 Misfire Detected"},
    {0x0321, "Ignition / Distributor Engine Speed Input Circuit Range/Performance"},
    {0x0325, "Knock Sensor 1 Circuit (Bank 1)"},
    {0x0327, "Knock Sensor 1 Circuit Low (Bank 1)"},
    {0x0328, "Knock Sensor 1 Circuit High (Bank 1)"},
    {0x0335, "Crankshaft Position Sensor A Circuit"},
    {0x0336, "Crankshaft Position Sensor A Circuit Range/Performance"},
    {0x0340, "Camshaft Position Sensor A Circuit"},
    {0x0341, "Camshaft Position Sensor A Circuit Range/Performance"},

    // Emissions
    {0x0420, "Catalyst System Efficiency Below Threshold (Bank 1)"},
    {0x0440, "Evaporative Emission System"},
    {0x0441, "Evaporative Emission System Incorrect Purge Flow"},
    {0x0442, "Evaporative Emission System Leak Detected (small leak)"},
    {0x0444, "Evaporative Emission System Purge Control Valve Circuit Open"},
    {0x0445, "Evaporative Emission System Purge Control Valve Circuit Shorted"},

    // Speed / idle / auxiliary inputs
    {0x0500, "Vehicle Speed Sensor A"},
    {0x0501, "Vehicle Speed Sensor A Range/Performance"},
    {0x0505, "Idle Air Control System"},
    {0x0506, "Idle Air Control System RPM Lower Than Expected"},
    {0x0507, "Idle Air Control System RPM Higher Than Expected"},
    {0x0560, "System Voltage"},
    {0x0562, "System Voltage Low"},
    {0x0563, "System Voltage High"},

    // ECU internal / communication
    {0x0600, "Serial Communication Link"},
    {0x0601, "Internal Control Module Memory Check Sum Error"},
    {0x0603, "Internal Control Module Keep Alive Memory (KAM) Error"},
    {0x0604, "Internal Control Module Random Access Memory (RAM) Error"},
    {0x0605, "Internal Control Module Read Only Memory (ROM) Error"},
    {0x0685, "ECM / PCM Power Relay Control Circuit Open"},
};

static const char *dtc_lookup(uint16_t code) {
    for (size_t i = 0; i < sizeof(dtc_descriptions) / sizeof(dtc_descriptions[0]); ++i) {
        if (dtc_descriptions[i].code == code) {
            return dtc_descriptions[i].description;
        }
    }
    return NULL;
}

// No exact match - J2012 still tells us the subsystem from the second digit,
// which beats printing nothing at all.
static const char *dtc_subsystem(uint16_t code) {
    if (((code >> 14) & 0x03) != 0) {
        return "non-powertrain (chassis / body / network)";
    }

    switch ((code >> 8) & 0x0F) {
        case 0x0: return "fuel and air metering, auxiliary emission controls";
        case 0x1: return "fuel and air metering";
        case 0x2: return "fuel and air metering (injector circuit)";
        case 0x3: return "ignition system or misfire";
        case 0x4: return "auxiliary emission controls";
        case 0x5: return "vehicle speed control, idle control, auxiliary inputs";
        case 0x6: return "computer output circuit / internal ECU";
        case 0x7:
        case 0x8:
        case 0x9: return "transmission";
        default:  return "unknown subsystem";
    }
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
    if (num_dtcs == 0) {
        printf("\n=== No fault codes stored ===\n\n");
        return;
    }

    printf("\n=== %zu fault code%s ===\n", num_dtcs, num_dtcs == 1 ? "" : "s");

    for (size_t i = 0; i < num_dtcs; ++i) {
        char code_string[6];
        convert_dtc_to_readable(dtcs[i].highByte, dtcs[i].lowByte, code_string);
        uint16_t raw = ((uint16_t)dtcs[i].highByte << 8) | dtcs[i].lowByte;

        const char *description = dtc_lookup(raw);
        if (description != NULL) {
            printf("\n  [%zu] %s  %s\n", i + 1, code_string, description);
        } else {
            // Say plainly that we have no entry rather than inventing one.
            printf("\n  [%zu] %s  <no description - %s>\n",
                   i + 1, code_string, dtc_subsystem(raw));
        }

        printf("      raw 0x%04X | status 0x%02X\n", raw, dtcs[i].status);
        print_dtc_status(dtcs[i].status);
    }
    printf("\n");
}
