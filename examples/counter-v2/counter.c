// SPDX-License-Identifier: Apache-2.0
#include "econtainer_guest.h"

#include <stdint.h>

/* Same ABI and product identity as counter v1; only the business rule changes. */
static uint32_t event_sum;

int32_t econtainer_init(void)
{
    event_sum = 0;
    return 0;
}

int32_t econtainer_on_event(const uint8_t *bytes, uint32_t size_bytes)
{
    if (size_bytes != 0 && bytes == 0) {
        return -1;
    }
    for (uint32_t index = 0; index < size_bytes; ++index) {
        if (event_sum > INT32_MAX - bytes[index]) {
            return -2;
        }
        event_sum += bytes[index];
    }
    return (int32_t)event_sum;
}

int32_t econtainer_stop(void)
{
    event_sum = 0;
    return 0;
}
