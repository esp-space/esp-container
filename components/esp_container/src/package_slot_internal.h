#ifndef ECONTAINER_PACKAGE_SLOT_INTERNAL_H
#define ECONTAINER_PACKAGE_SLOT_INTERNAL_H

#include "esp_container_package_slot.h"
#include "slots_internal.h"

/* Shared admission core for a real reservation or a real confirmed binding.
 * Do not fabricate an operation to validate a confirmed package. */
econtainer_slot_validation_result_t econtainer_package_slot_check(
    econtainer_package_slot_validation_t *validation,
    const econtainer_slot_package_t *package,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references);

#endif
