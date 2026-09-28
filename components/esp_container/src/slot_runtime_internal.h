#ifndef ECONTAINER_SLOT_RUNTIME_INTERNAL_H
#define ECONTAINER_SLOT_RUNTIME_INTERNAL_H

#include "esp_container_package_slot.h"
#include "runtime_internal.h"
#include "slots_internal.h"

/* Component-private synchronous bridge, on the unique pthread executor owner.
 * out must point to NULL. Base must hold its existing firmware/storage owner
 * around the call; every package writer must use io's same lock. The old
 * instance must already be closed. TRIAL_STARTED must already be durable.
 *
 * Revalidates current signed bytes and policy under the slot lock, maps only
 * for this call, and never executes a guest entrypoint. runtime_open owns its
 * required code/data copies before unmap/unlock. init is a separate owner call.
 * The validation's verified_info pointer is ignored: no old admission proof is
 * consumed. Workspaces remain caller-owned for the call.
 *
 * Signed memory/stack/budgets and requested capabilities bound the separately
 * granted runtime limits. Heap/event bytes/log/timer counts and whole-entry
 * duration remain independent platform policy. Queue/storage/individual host
 * timeout are still admission-only; this bridge does not add those facilities.
 * Successful open exports the freshly verified queue limit and package digest
 * to let the caller own a matching event queue. */
econtainer_slot_runtime_result_t econtainer_slot_runtime_open(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_selection_request_t *request,
    const econtainer_package_slot_validation_t *validation,
    const econtainer_runtime_limits_t *platform_limits,
    econtainer_runtime_t **out);

#endif
