#pragma once

#include <stdbool.h>

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
