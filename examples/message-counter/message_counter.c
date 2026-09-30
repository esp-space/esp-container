#include "econtainer_guest.h"

#include <stdint.h>

enum {
    MESSAGE_COUNTER_IDLE = 0,
    MESSAGE_COUNTER_ACTIVE = 1,
    MESSAGE_COUNTER_PAUSED = 2,
    MESSAGE_COUNTER_WINDOW_MS = 100,
};

static uint32_t byte_count;
static int32_t state;
static uint64_t window_timer;

int32_t econtainer_init(void)
{
    byte_count = 0;
    state = MESSAGE_COUNTER_IDLE;
    window_timer = 0;
    return 0;
}

int32_t econtainer_on_event(const uint8_t *bytes, uint32_t size_bytes)
{
    if (bytes == 0 || size_bytes == 0) return -1;
    uint64_t handle = 0;
    uint32_t skipped = 0;
    if (econtainer_timer_event_decode(bytes, size_bytes, &handle, &skipped)) {
        /* A stale instance handle cannot close the current window. */
        if (state == MESSAGE_COUNTER_ACTIVE && handle == window_timer && skipped == 0) {
            window_timer = 0;
            state = MESSAGE_COUNTER_IDLE;
        }
        return (int32_t)byte_count;
    }
    switch (bytes[0]) {
    case 1: /* Count the nonempty message body within a bounded batch window. */
        if (size_bytes == 1) return -1;
        if (state == MESSAGE_COUNTER_PAUSED) return -2;
        if (size_bytes - 1 > (uint32_t)INT32_MAX - byte_count) return -3;
        if (state == MESSAGE_COUNTER_IDLE) {
            const uint64_t timer = econtainer_timer_start(MESSAGE_COUNTER_WINDOW_MS, 0);
            if (timer == 0) return -4;
            window_timer = timer;
            state = MESSAGE_COUNTER_ACTIVE;
        }
        byte_count += size_bytes - 1;
        return (int32_t)byte_count;
    case 2: /* Pause preserves the count and cancels the pending window. */
        if (size_bytes != 1) return -1;
        if (window_timer != 0 && econtainer_timer_cancel(window_timer) != 0) return -4;
        window_timer = 0;
        state = MESSAGE_COUNTER_PAUSED;
        return (int32_t)byte_count;
    case 3: /* Resume starts no timer until another message arrives. */
        if (size_bytes != 1 || state != MESSAGE_COUNTER_PAUSED) return -1;
        state = MESSAGE_COUNTER_IDLE;
        return (int32_t)byte_count;
    case 4:
        return size_bytes == 1 ? state : -1;
    case 5:
        return size_bytes == 1 ? (int32_t)byte_count : -1;
    default:
        return -1;
    }
}

int32_t econtainer_stop(void)
{
    /* The host revokes every native timer before entering guest stop. */
    window_timer = 0;
    state = MESSAGE_COUNTER_IDLE;
    byte_count = 0;
    return 0;
}
