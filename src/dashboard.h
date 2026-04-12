#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ring_buffer.h"

typedef struct {
    RingBuffer *tx_buffer;  // Messages to ECU
    RingBuffer *rx_buffer;  // Messages from ECU
} Dashboard;

void dashboard_init(Dashboard *dash, RingBuffer *tx, RingBuffer *rx);
void dashboard_update(Dashboard *dash);
