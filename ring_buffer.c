#include "ring_buffer.h"
#include <string.h>
#include "hardware/sync.h"

void ringbuffer_init(RingBuffer* buffer) {
    buffer->readIndex = 0;
    buffer->writeIndex = 0;
    buffer->capacity = RING_BUFFER_SIZE;
    critical_section_init(&buffer->critSec);
}

bool ringbuffer_push(RingBuffer* buffer, const BufferMessage* message) {
    uint32_t nextWrite = (buffer->writeIndex + 1) % buffer->capacity;
    
    if (nextWrite == buffer->readIndex) {
        return false;  // Buffer full
    }
    
    memcpy(&buffer->messages[buffer->writeIndex], message, sizeof(BufferMessage));
    buffer->writeIndex = nextWrite;
    return true;
}

bool ringbuffer_pop(RingBuffer* buffer, BufferMessage* message) {
    if (buffer->readIndex == buffer->writeIndex) {
        return false;  // Buffer empty
    }
    
    memcpy(message, &buffer->messages[buffer->readIndex], sizeof(BufferMessage));
    buffer->readIndex = (buffer->readIndex + 1) % buffer->capacity;
    return true;
}

bool ringbuffer_is_empty(RingBuffer* buffer) {
    bool empty;
    critical_section_enter_blocking(&buffer->critSec);
    empty = (buffer->readIndex == buffer->writeIndex);
    critical_section_exit(&buffer->critSec);
    return empty;
}

bool ringbuffer_is_full(RingBuffer* buffer) {
    bool full;
    critical_section_enter_blocking(&buffer->critSec);
    full = ((buffer->writeIndex + 1) % buffer->capacity) == buffer->readIndex;
    critical_section_exit(&buffer->critSec);
    return full;
} 