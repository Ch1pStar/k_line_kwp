#include "ring_buffer.h"

#include <string.h>
#include "hardware/sync.h"

void ringbuffer_init(RingBuffer* buffer) {
    buffer->readIndex = 0;
    buffer->writeIndex = 0;
}

bool ringbuffer_push(RingBuffer* buffer, const BufferMessage* message) {
    const uint32_t write = buffer->writeIndex;
    const uint32_t next = (write + 1) % RING_BUFFER_SIZE;

    if (next == buffer->readIndex) {
        return false;  // Buffer full
    }

    BufferMessage *slot = &buffer->messages[write];
    uint8_t length = message->length;
    if (length > MAX_MESSAGE_SIZE) length = MAX_MESSAGE_SIZE;

    slot->messageType = message->messageType;
    slot->length = length;
    if (length > 0) {
        memcpy(slot->data, message->data, length);
    }

    // The slot contents must be visible to the other core before the index that
    // publishes them, or the consumer can read a half-written message.
    __dmb();
    buffer->writeIndex = next;
    return true;
}

bool ringbuffer_pop(RingBuffer* buffer, BufferMessage* message) {
    const uint32_t read = buffer->readIndex;

    if (read == buffer->writeIndex) {
        return false;  // Buffer empty
    }

    // Pairs with the producer's barrier: having seen the new writeIndex, make
    // sure we also see the slot contents written before it.
    __dmb();

    const BufferMessage *slot = &buffer->messages[read];
    message->messageType = slot->messageType;
    message->length = slot->length;
    if (slot->length > 0) {
        memcpy(message->data, slot->data, slot->length);
    }

    // Don't release the slot for reuse until it has been copied out.
    __dmb();
    buffer->readIndex = (read + 1) % RING_BUFFER_SIZE;
    return true;
}

bool ringbuffer_is_empty(RingBuffer* buffer) {
    return buffer->readIndex == buffer->writeIndex;
}

bool ringbuffer_is_full(RingBuffer* buffer) {
    return ((buffer->writeIndex + 1) % RING_BUFFER_SIZE) == buffer->readIndex;
}
