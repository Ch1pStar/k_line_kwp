#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pico/critical_section.h"

#define RING_BUFFER_SIZE 256
#define MAX_MESSAGE_SIZE 256

typedef struct {
    uint8_t messageType;
    uint8_t length;
    uint8_t data[MAX_MESSAGE_SIZE];
} BufferMessage;

typedef struct {
    BufferMessage messages[RING_BUFFER_SIZE];
    volatile uint32_t readIndex;
    volatile uint32_t writeIndex;
    uint32_t capacity;
    critical_section_t critSec;
} RingBuffer;

void ringbuffer_init(RingBuffer* buffer);
bool ringbuffer_push(RingBuffer* buffer, const BufferMessage* message);
bool ringbuffer_pop(RingBuffer* buffer, BufferMessage* message);
bool ringbuffer_is_empty(RingBuffer* buffer);
bool ringbuffer_is_full(RingBuffer* buffer); 