#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Capped by the KWP2000 frame limit, not by the ECU. The 0xB7 request is
// [SID][format][3 bytes per variable], and a request's length field is 6 bits,
// so 63 bytes total => (63 - 2) / 3 = 20 variables. Going over produces a frame
// the ECU silently misparses (see MAX_REQUEST_LENGTH in kwp2000.c).
//
// The ECU itself allows far more - ME7Logger documents 254 bytes - and so would
// we, once the extended-length format is implemented on transmit.
#define ME7_MAX_LOG_VARS 20
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

// Open the manufacturer session and switch the K-line to the faster logging
// rate (10 86 64 -> 57600 baud). Must be called immediately after the 5-baud
// init; the ECU only accepts it in a short window after the handshake. Returns
// false if the ECU refused, in which case the link stays at 10400 and
// everything still works, just slower.
bool me7_handler_open_fast_session(void);

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
