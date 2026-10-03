#include "econtainer_guest.h"
#include <stdint.h>
static uint64_t timer;
int32_t econtainer_init(void) { return econtainer_monotonic_ms() ? 0 : -1; }
int32_t econtainer_on_event(const uint8_t *bytes, uint32_t size)
{
    static const uint8_t log[] = {'n', 'e', 'w'};
    if (bytes == 0 || size != 1) return -10;
    switch (bytes[0]) {
    case 1: return econtainer_log(log, sizeof(log));
    case 2: return econtainer_log(log, 0);
    case 3: return econtainer_log((const uint8_t *)(uintptr_t)0xfffffff0U, 1);
    case 4: {
        const uint64_t opened = econtainer_timer_start(1000, 0);
        if (opened != 0) timer = opened;
        return opened ? 0 : -1;
    }
    case 5: return econtainer_timer_start(0, 0) ? 0 : -1;
    case 6: return econtainer_timer_cancel(timer);
    case 7: return econtainer_timer_cancel(0);
    case 8: return econtainer_monotonic_ms() ? 0 : -1;
    case 9: {
        int32_t count = 0;
        for (unsigned index = 0; index < 4; ++index)
            if (econtainer_monotonic_ms()) ++count;
        return count;
    }
    default: return -11;
    }
}
int32_t econtainer_stop(void) { return econtainer_monotonic_ms() ? 0 : -1; }
