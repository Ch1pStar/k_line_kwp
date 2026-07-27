#include "kwp2000.h"
#include "uart_pio.h"
#include "log.h"

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
//
// Two header forms, per ISO 14230-2:
//
//   short     [Fmt|len]            SID data... CS     len <= 0x3F
//   extended  [Fmt=0x00] [Len]     SID data... CS     len <= 0xFF
//
// The format byte carries the length in its low 6 bits; zero there means the
// real length follows in its own byte. The extended form is what the reference
// implementation uses for its 128-byte handler writes (`00 85 3d 38 7a 00 80 ...`).
//
// Note the checksum is unchanged between the two: it sums every byte ahead of
// it, and the extra header byte is 0x00.
static size_t build_frame(const KWP2000Packet *packet, uint8_t *out) {
    size_t n = 0;

    if (packet->length > KWP_SHORT_FRAME_MAX) {
        out[n++] = 0x00;
    }
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
    uint8_t frame[MAX_DATA_SIZE + 8];  // fmt + len + SID + data + checksum
    const size_t frame_len = build_frame(packet, frame);

    for (size_t i = 0; i < frame_len; ++i) {
        uart_send_byte(frame[i]);

        uint32_t echo = uart_read_byte_timeout(100000);
        if (echo == UART_TIMEOUT) {
            if (!silent) klog("Echo timeout at byte %u of %u",
                              (unsigned)i, (unsigned)frame_len);
        } else if ((uint8_t)echo != frame[i]) {
            if (!silent) klog("Echo mismatch at byte %u: sent %02X, read %02X",
                              (unsigned)i, frame[i], (uint8_t)echo);
        }
    }

    return frame_len;
}

size_t kwp2000_send(const KWP2000Service *service, bool silent) {
    KWP2000Packet packet;

    // The length is one byte wherever it lives, so SID + data cannot exceed 255.
    // Frames over KWP_SHORT_FRAME_MAX go out in the extended form; see
    // build_frame. Before that existed, a 64-byte request went out as 0x40 and
    // the ECU read it as "address information follows, length 0".
    if (1 + service->dataLength > KWP_MAX_FRAME_LENGTH) {
        klog("Request of %u bytes exceeds the %u byte maximum - not sent",
             (unsigned)(1 + service->dataLength), (unsigned)KWP_MAX_FRAME_LENGTH);
        return 0;
    }

    packet.length = (uint8_t)(1 + service->dataLength);
    packet.serviceId = service->serviceId;
    for (size_t i = 0; i < service->dataLength; ++i) {
        packet.dataBytes[i] = service->dataBytes[i];
    }
    packet.checksum = calculate_checksum(service);

    if (!silent) {
        // One line per frame: the log queue drops rather than blocks, so dense
        // output survives a burst that a byte-at-a-time printf would not.
        // Must start empty: a bare service like 0xB7 has no data bytes, and an
        // uninitialised buffer printed whatever stack garbage was left there.
        char data_hex[3 * 24 + 4] = "";
        size_t n = 0;
        for (size_t i = 0; i < service->dataLength && n + 4 < sizeof(data_hex); ++i) {
            n += snprintf(data_hex + n, sizeof(data_hex) - n, "%s%02X",
                          i ? " " : "", packet.dataBytes[i]);
        }
        if (service->dataLength > 24) {
            snprintf(data_hex + n, sizeof(data_hex) - n, "...");
        }
        klog("TX len=%02X SID=%02X data=[%s] csum=%02X",
             packet.length, packet.serviceId, data_hex, packet.checksum);
    }

    // Echo is consumed inline by send_packet, so nothing is left for the reader
    // to skip. Returns the frame length for logging only.
    return send_packet(&packet, silent);
}

// One inter-byte timeout for every field of a response. P2max is 50ms for this
// ECU; 100ms leaves margin without stalling the state machine for long.
#define RESPONSE_TIMEOUT_US 100000

ResponseStatus kwp2000_read_response(size_t echo_bytes, KWP2000Response *response, bool silent) {
    response->dataSize = 0;
    response->serviceId = 0;

    // Skip echoed command bytes
    for (size_t i = 0; i < echo_bytes; ++i) {
        uint32_t byte = uart_read_byte_timeout(RESPONSE_TIMEOUT_US);
        if (byte == UART_TIMEOUT) {
            if (!silent) klog("Timeout while reading echo bytes");
            return RESPONSE_ERROR;
        }
    }

    // Header, per ISO 14230-2: Fmt [Tgt] [Src] [Len]. The format byte carries
    // the length in bits 0-5 and the addressing mode in bits 6-7. Everything
    // this ECU has ever sent uses mode 00 with a length that fits in 6 bits, so
    // the old parser read the format byte as a plain length. That misreads any
    // reply over 63 bytes (length field 0 => the count lives in a separate Len
    // byte) and would walk straight through address bytes as if they were
    // payload, desynchronising every following frame.
    uint32_t byte = uart_read_byte_timeout(RESPONSE_TIMEOUT_US);
    if (byte == UART_TIMEOUT) {
        if (!silent) klog("Timeout while reading response format byte");
        return RESPONSE_ERROR;
    }
    const uint8_t format = (uint8_t)byte;
    uint8_t checksum = format;

    const uint8_t address_mode = format & 0xC0;
    const size_t address_bytes = (address_mode == 0x80 || address_mode == 0xC0) ? 2 : 0;
    for (size_t i = 0; i < address_bytes; ++i) {
        byte = uart_read_byte_timeout(RESPONSE_TIMEOUT_US);
        if (byte == UART_TIMEOUT) {
            if (!silent) klog("Timeout while reading address byte %u", (unsigned)i);
            return RESPONSE_ERROR;
        }
        checksum += (uint8_t)byte;
    }
    if (address_bytes && !silent) {
        klog("Note: response carried address info (format %02X)", format);
    }

    size_t length = format & 0x3F;
    if (length == 0) {
        byte = uart_read_byte_timeout(RESPONSE_TIMEOUT_US);
        if (byte == UART_TIMEOUT) {
            if (!silent) klog("Timeout while reading extended length byte");
            return RESPONSE_ERROR;
        }
        length = (uint8_t)byte;
        checksum += (uint8_t)byte;
    }

    // Length counts the service id plus its data. Zero means there is no
    // service id at all - a malformed frame, and the value that used to
    // underflow the data loop into reading SIZE_MAX bytes.
    if (length == 0) {
        if (!silent) klog("Malformed response: zero length");
        return RESPONSE_ERROR;
    }

    byte = uart_read_byte_timeout(RESPONSE_TIMEOUT_US);
    if (byte == UART_TIMEOUT) {
        if (!silent) klog("Timeout while reading response service id");
        return RESPONSE_ERROR;
    }
    response->serviceId = (uint8_t)byte;
    checksum += response->serviceId;

    // Read the payload. On overflow keep consuming the frame instead of
    // returning early: the unread bytes would otherwise still be on the wire,
    // and every subsequent response would be parsed one frame behind.
    bool overflow = false;
    for (size_t i = 0; i + 1 < length; ++i) {
        byte = uart_read_byte_timeout(RESPONSE_TIMEOUT_US);
        if (byte == UART_TIMEOUT) {
            if (!silent) klog("Timeout while reading data byte %u", (unsigned)i);
            return RESPONSE_ERROR;
        }
        checksum += (uint8_t)byte;
        if (response->dataSize < MAX_RESPONSE_SIZE) {
            response->data[response->dataSize++] = (uint8_t)byte;
        } else {
            overflow = true;
        }
    }

    uint32_t recv_checksum = uart_read_byte_timeout(RESPONSE_TIMEOUT_US);
    if (recv_checksum == UART_TIMEOUT) {
        if (!silent) klog("Timeout while reading checksum");
        return RESPONSE_ERROR;
    }
    const bool checksum_ok = (checksum == (uint8_t)recv_checksum);

    if (response->serviceId == 0x7F) {
        // Negative response: [Fmt][7F][rejected SID][NRC][checksum]. The ECU
        // answered, so the link is up - this is a rejection, not a fault.
        if (!silent) {
            if (response->dataSize >= 2) {
                klog("Negative response: 7F %02X %02X - service 0x%02X rejected, NRC 0x%02X",
                     response->data[0], response->data[1],
                     response->data[0], response->data[1]);
            } else {
                klog("Negative response: 7F (%u payload bytes)", (unsigned)response->dataSize);
            }
            if (!checksum_ok) {
                klog("  (checksum mismatch on negative response: calc %02X, recv %02X)",
                     checksum, (uint8_t)recv_checksum);
            }
        }
        return RESPONSE_NEGATIVE;
    }

    if (!silent) {
        klog("Response SID %02X, length %u", response->serviceId, (unsigned)length);
    }

    if (overflow) {
        if (!silent) klog("Response overflowed %u byte buffer (frame drained)",
                          (unsigned)MAX_RESPONSE_SIZE);
        return RESPONSE_OVERFLOW;
    }

    if (!checksum_ok) {
        if (!silent) {
            klog("Response checksum invalid. Calculated: %02X, Received: %02X",
                 checksum, (uint8_t)recv_checksum);
        }
        return RESPONSE_CHECKSUM_INVALID;
    }

    return RESPONSE_OK;
}

ResponseStatus kwp2000_execute(const KWP2000Service *service, KWP2000Response *response, bool silent) {
    kwp2000_send(service, silent);
    // 0: the echo was already consumed byte-by-byte during the send.
    return kwp2000_read_response(0, response, silent);
}

const char *kwp2000_status_name(ResponseStatus status) {
    switch (status) {
        case RESPONSE_OK:               return "ok";
        case RESPONSE_ERROR:            return "no reply / malformed";
        case RESPONSE_NEGATIVE:         return "rejected (7F)";
        case RESPONSE_CHECKSUM_INVALID: return "bad checksum";
        case RESPONSE_OVERFLOW:         return "response too long";
        default:                        return "unknown";
    }
}

void kwp2000_print_response(const KWP2000Response *response) {
    char hex[3 * 24 + 4] = "";
    size_t n = 0;
    for (size_t i = 0; i < response->dataSize && n + 4 < sizeof(hex); ++i) {
        n += snprintf(hex + n, sizeof(hex) - n, "%s%02X", i ? " " : "", response->data[i]);
    }
    klog("Response data (%u): [%s]", (unsigned)response->dataSize, hex);
}

void kwp2000_print_str_response(const KWP2000Response *response) {
    char text[64];
    size_t n = 0;
    for (size_t i = 0; i < response->dataSize && n + 1 < sizeof(text); ++i) {
        text[n++] = isprint(response->data[i]) ? (char)response->data[i] : '.';
    }
    text[n] = '\0';
    klog("Response string: \"%s\"", text);
}

uint8_t kwp2000_parse_dtcs(const KWP2000Response *response, DTCData *dtc_array, size_t dtc_array_size) {
    if (response->dataSize < 1) {
        klog("DTC response too short (%u bytes)", (unsigned)response->dataSize);
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
        klog("DTC count mismatch: ECU reported %u, %u usable in %u response bytes",
             reported, (unsigned)count, (unsigned)response->dataSize);
    }

    for (size_t i = 0; i < count; i++) {
        size_t offset = 1 + i * 3;
        dtc_array[i].highByte = response->data[offset];
        dtc_array[i].lowByte = response->data[offset + 1];
        dtc_array[i].status = response->data[offset + 2];
    }

    return (uint8_t)count;
}

static const char *dtc_symptom(uint8_t status) {
    switch (status & 0x0F) {
        case 0x00: return "no symptom";
        case 0x01: return "above max threshold";
        case 0x02: return "below min threshold";
        case 0x04: return "no signal";
        case 0x08: return "invalid signal";
        default:   return "unknown symptom";
    }
}

static const char *dtc_state(uint8_t status) {
    switch (status & 0x60) {
        case 0x00: return "not detected/stored";
        case 0x20: return "stored, not present (history)";
        case 0x40: return "maturing";
        default:   return "ACTIVE (present + stored)";
    }
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

// Two log lines per fault rather than the eight this used to print: the log
// queue drops when full, and a six-fault bench dump at eight lines each was
// enough of a burst to lose the tail of its own output.
void kwp2000_print_dtcs(DTCData *dtcs, size_t num_dtcs) {
    if (num_dtcs == 0) {
        klog("=== No fault codes stored ===");
        return;
    }

    klog("=== %u fault code%s ===", (unsigned)num_dtcs, num_dtcs == 1 ? "" : "s");

    for (size_t i = 0; i < num_dtcs; ++i) {
        char code_string[6];
        convert_dtc_to_readable(dtcs[i].highByte, dtcs[i].lowByte, code_string);
        uint16_t raw = ((uint16_t)dtcs[i].highByte << 8) | dtcs[i].lowByte;

        const char *description = dtc_lookup(raw);
        if (description != NULL) {
            klog("  [%u] %s  %s", (unsigned)(i + 1), code_string, description);
        } else {
            // Say plainly that we have no entry rather than inventing one.
            klog("  [%u] %s  <no description - %s>",
                 (unsigned)(i + 1), code_string, dtc_subsystem(raw));
        }

        klog("      raw %04X status %02X | %s | %s | test %s | MIL %s",
             raw, dtcs[i].status,
             dtc_state(dtcs[i].status), dtc_symptom(dtcs[i].status),
             (dtcs[i].status & 0x10) ? "incomplete" : "complete",
             (dtcs[i].status & 0x80) ? "on" : "off");
    }
}
