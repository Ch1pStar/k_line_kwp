#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RING_BUFFER_SIZE 128

// Largest message payload. The biggest real messages are a KWP2000 response
// (MAX_RESPONSE_SIZE, 80 bytes) and a log line; 128 covers both with room to
// spare. It was 256 payload x 256 slots, which cost 132 KB of the RP2040's
// 264 KB for capacity nothing came close to using.
#define MAX_MESSAGE_SIZE 128

typedef struct {
    uint8_t messageType;
    uint8_t length;
    uint8_t data[MAX_MESSAGE_SIZE];
} BufferMessage;

// Single-producer/single-consumer queue: exactly one core may push and exactly
// one (different) core may pop a given buffer. That is what makes it safe with
// no lock - the producer only ever writes writeIndex, the consumer only ever
// writes readIndex, and the payload copy is ordered against the index update by
// a barrier. A second producer or consumer would need real locking, not a
// critical section around the index reads (which is what this used to have, and
// which protected nothing - push and pop never took it).
typedef struct {
    BufferMessage messages[RING_BUFFER_SIZE];
    volatile uint32_t readIndex;
    volatile uint32_t writeIndex;
} RingBuffer;

void ringbuffer_init(RingBuffer* buffer);

// Copies only the bytes the message actually uses. Payloads longer than
// MAX_MESSAGE_SIZE are truncated rather than overrunning the slot.
bool ringbuffer_push(RingBuffer* buffer, const BufferMessage* message);
bool ringbuffer_pop(RingBuffer* buffer, BufferMessage* message);
bool ringbuffer_is_empty(RingBuffer* buffer);
bool ringbuffer_is_full(RingBuffer* buffer);
