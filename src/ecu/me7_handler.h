#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The ECU itself allows far more (ME7Logger documents 254 bytes / 127
// locations); this is only what our message slot and sample budget allow.
#define ME7_MAX_LOG_VARS 32
#define ME7_ADDRESS_BYTES 3

// Injection of the ME7.5 fast-logging RAM handler (handler_setzi.bin).
//
// Runs on core 0, and every step waits for the ECU's own reply before the next
// one goes out - the sequence is paced by the ECU rather than by fixed sleeps
// guessed from the other side of a queue. That is also how the reference
// implementation (me7log's loadHandler) drives it.
//
// The whole sequence is a continuous stream of requests, so it keeps the KWP
// session alive on its own while it runs.

// Write the handler blob into ECU RAM. Assumes a diagnostic session is already
// open (install() does that itself).
bool me7_handler_load(void);

// The full proven sequence: diagnostic session, load, service-table redirect,
// init trigger, variable list. Returns true when the handler is ready to
// sample with a bare 0xB7.
bool me7_handler_install(void);

// Replace the logged variable list and send it to the handler.
//
// `addresses` is a flat list of 3-byte big-endian ECU addresses, so its length
// must be a multiple of 3. The leading 0xB7 and format byte are added here -
// callers supply addresses only. Bit 0x40 of an address's first byte marks a
// 2-byte variable; everything else is 1 byte.
bool me7_handler_set_vars(const uint8_t *addresses, size_t address_bytes);

// Bytes a sample is expected to contain, derived from the variable list alone.
// This is why decoding can live entirely on the host: the firmware needs the
// widths to frame a sample, never the scaling.
size_t me7_handler_sample_size(void);

size_t me7_handler_var_count(void);
