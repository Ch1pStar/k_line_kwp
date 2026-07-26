#include "host.h"
#include "command.h"
#include "console.h"
#include "host_link.h"

#include <stddef.h>

static RingBuffer *ecu_messages = NULL;

void host_init(RingBuffer *to_ecu, RingBuffer *from_ecu) {
    ecu_messages = from_ecu;

    command_init(to_ecu);
    console_init();
    host_link_init();
}

void host_update(void) {
    console_poll_input();
    host_link_poll();

    BufferMessage msg;
    while (ringbuffer_pop(ecu_messages, &msg)) {
        console_on_message(&msg);
        host_link_on_message(&msg);
    }
}
