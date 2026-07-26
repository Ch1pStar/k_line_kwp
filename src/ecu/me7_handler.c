#include "me7_handler.h"
#include "kwp2000.h"
#include "log.h"

#include <string.h>
#include <stdint.h>

extern unsigned char _binary_handler_setzi_bin_start[];
extern unsigned char _binary_handler_setzi_bin_end[];

// Where the handler lives in ECU RAM (segment #038h).
#define HANDLER_RAM_ADDRESS 0x387A00

// WriteMemoryByAddress payload size. 8 is what this bench has proven; the
// reference JS uses 0x80 and would cut the write from 73 requests to 5, which
// is worth trying once the sampler makes reinstall latency matter.
#define WRITE_CHUNK_SIZE 8

// Give up on the load rather than spend 100ms of timeout per chunk for all 73
// when the link has actually gone away.
#define MAX_CONSECUTIVE_FAILURES 4

// Progress line every this many chunks - one per chunk would be 73 log lines.
#define PROGRESS_EVERY 24

typedef enum {
    EXPECT_POSITIVE,   // anything but a positive reply is a failure
    EXPECT_ANY_REPLY,  // a rejection still counts: the ECU answered
} StepExpectation;

// Send one frame and wait for the reply. Silent: the sequence narrates itself
// at a useful granularity instead of dumping every frame.
static ResponseStatus run_frame(const uint8_t *frame, size_t length,
                                KWP2000Response *response) {
    KWP2000Service service;
    service.serviceId = frame[0];
    service.dataLength = length - 1;
    memcpy(service.dataBytes, &frame[1], length - 1);
    return kwp2000_execute(&service, response, true);
}

static bool step(const char *name, const uint8_t *frame, size_t length,
                 StepExpectation expectation) {
    KWP2000Response response;
    const ResponseStatus status = run_frame(frame, length, &response);

    const bool ok = (status == RESPONSE_OK) ||
                    (expectation == EXPECT_ANY_REPLY && status == RESPONSE_NEGATIVE);

    klog("me7: %s -> %s%s", name, kwp2000_status_name(status),
         ok ? "" : "  [FAILED]");
    return ok;
}

bool me7_handler_load(void) {
    const unsigned char *data = _binary_handler_setzi_bin_start;
    const size_t size = _binary_handler_setzi_bin_end - _binary_handler_setzi_bin_start;
    const size_t num_chunks = (size + WRITE_CHUNK_SIZE - 1) / WRITE_CHUNK_SIZE;

    klog("me7: writing %u bytes to 0x%06X in %u chunks",
         (unsigned)size, (unsigned)HANDLER_RAM_ADDRESS, (unsigned)num_chunks);

    size_t failures = 0;
    size_t consecutive_failures = 0;

    for (size_t i = 0; i < num_chunks; i++) {
        const size_t offset = i * WRITE_CHUNK_SIZE;
        size_t chunk = WRITE_CHUNK_SIZE;
        if (offset + chunk > size) {
            chunk = size - offset;
        }

        const uint32_t address = HANDLER_RAM_ADDRESS + offset;

        // 3D [3-byte address] [size] [data...]
        uint8_t frame[5 + WRITE_CHUNK_SIZE];
        frame[0] = 0x3D;
        frame[1] = (address >> 16) & 0xFF;
        frame[2] = (address >> 8) & 0xFF;
        frame[3] = address & 0xFF;
        frame[4] = (uint8_t)chunk;
        memcpy(&frame[5], &data[offset], chunk);

        KWP2000Response response;
        const ResponseStatus status = run_frame(frame, 5 + chunk, &response);

        if (status == RESPONSE_OK) {
            consecutive_failures = 0;
        } else {
            failures++;
            consecutive_failures++;
            klog("me7: chunk %u at 0x%06X -> %s",
                 (unsigned)i, (unsigned)address, kwp2000_status_name(status));

            if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                klog("me7: %u consecutive failures, aborting load",
                     (unsigned)consecutive_failures);
                return false;
            }
        }

        if ((i + 1) % PROGRESS_EVERY == 0) {
            klog("me7:   %u/%u chunks", (unsigned)(i + 1), (unsigned)num_chunks);
        }
    }

    if (failures > 0) {
        klog("me7: load finished with %u failed chunk(s) - handler is corrupt",
             (unsigned)failures);
        return false;
    }

    klog("me7: load complete (%u bytes)", (unsigned)size);
    return true;
}

// The 0xB7 "set logging variables" request: SID then a leading format byte
// (0x03, still under investigation - it is constant across every var-list
// length in me7log, so it is not a count) then one 3-byte big-endian address
// per variable, where bit 0x40 of the first byte marks a 2-byte variable.
//
// The default set maps to nmot, ub, wped, plsol, tmot on the 8N0906018BP ECU
// (see me7log/ecu_files/8N0906018BP 0002.ecu for scaling). It is only a
// fallback: the host owns this list and replaces it with me7_handler_set_vars,
// because the host is the side that has the .ecu file. Positive response 0xF7.
#define VAR_FRAME_HEADER 2
#define MAX_VAR_FRAME (VAR_FRAME_HEADER + ME7_ADDRESS_BYTES * ME7_MAX_LOG_VARS)

static uint8_t var_frame[MAX_VAR_FRAME] = {
    0xB7,
    0x03,
    0x00, 0xF8, 0x9A,   // nmot  - engine speed
    0x38, 0x09, 0x91,   // ub    - battery voltage
    0x38, 0x09, 0x9D,   // wped  - accelerator pedal
    0x38, 0x09, 0xF6,   // plsol - target boost
    0x38, 0x0A, 0x32,   // tmot  - coolant temp
};
static size_t var_frame_length = VAR_FRAME_HEADER + 5 * ME7_ADDRESS_BYTES;

size_t me7_handler_var_count(void) {
    return (var_frame_length - VAR_FRAME_HEADER) / ME7_ADDRESS_BYTES;
}

// Derived from the list on every call rather than cached: one less piece of
// state that can disagree with the frame actually sent to the ECU.
size_t me7_handler_sample_size(void) {
    size_t size = 0;
    for (size_t i = VAR_FRAME_HEADER; i + ME7_ADDRESS_BYTES <= var_frame_length;
         i += ME7_ADDRESS_BYTES) {
        size += (var_frame[i] & 0x40) ? 2 : 1;
    }
    return size;
}

bool me7_handler_set_vars(const uint8_t *addresses, size_t address_bytes) {
    if (address_bytes == 0 || address_bytes % ME7_ADDRESS_BYTES != 0) {
        klog("me7: variable list must be whole 3-byte addresses (got %u)",
             (unsigned)address_bytes);
        return false;
    }
    if (address_bytes > ME7_ADDRESS_BYTES * ME7_MAX_LOG_VARS) {
        klog("me7: %u variables requested, max %u",
             (unsigned)(address_bytes / ME7_ADDRESS_BYTES), (unsigned)ME7_MAX_LOG_VARS);
        return false;
    }

    var_frame[0] = 0xB7;
    var_frame[1] = 0x03;
    memcpy(&var_frame[VAR_FRAME_HEADER], addresses, address_bytes);
    var_frame_length = VAR_FRAME_HEADER + address_bytes;

    const bool ok = step("set log variables (B7 + list)",
                         var_frame, var_frame_length, EXPECT_POSITIVE);
    klog("me7: %u variable(s), %u byte sample",
         (unsigned)me7_handler_var_count(), (unsigned)me7_handler_sample_size());
    return ok;
}

bool me7_handler_install(void) {
    klog("me7: installing fast-logging handler");

    // 1. Manufacturer session - required before ReadMemoryByAddress/WriteMemory.
    {
        const uint8_t frame[] = {0x10, 0x86};
        if (!step("diagnostic session (10 86)", frame, sizeof(frame), EXPECT_POSITIVE)) {
            return false;
        }
    }

    // 2. Write the handler to RAM. Must happen before the redirect, while 0x3D
    //    still routes through the ECU's original dispatcher.
    if (!me7_handler_load()) {
        return false;
    }

    // 3. Repoint the service-table pointer at 0xE228 to our table. Bytes
    //    00 3A E1 00 are the C166 far-pointer encoding of 0x387A00 - kept as a
    //    literal because that encoding is not a plain function of the address.
    //    Without this step the ECU keeps using its BootRom table and the
    //    handler is never reached.
    {
        const uint8_t frame[] = {0x3D, 0x00, 0xE2, 0x28, 0x04, 0x00, 0x3A, 0xE1, 0x00};
        if (!step("redirect service table (0xE228 -> 0x387A00)",
                  frame, sizeof(frame), EXPECT_POSITIVE)) {
            return false;
        }
    }

    // 4. The first call after the redirect makes the handler copy the original
    //    service table into its own. It rejects this 0x3E itself while doing
    //    so - a rejection here is the expected outcome, not a failure.
    {
        const uint8_t frame[] = {0x3E};
        step("init trigger (3E, rejection expected)", frame, sizeof(frame), EXPECT_ANY_REPLY);
    }

    // 5. Hand over the variable list. After this a bare 0xB7 returns the packed
    //    values.
    if (!step("set log variables (B7 + list)",
              var_frame, var_frame_length, EXPECT_POSITIVE)) {
        return false;
    }

    klog("me7: handler ready - %u variable(s), %u byte sample",
         (unsigned)me7_handler_var_count(), (unsigned)me7_handler_sample_size());
    return true;
}
