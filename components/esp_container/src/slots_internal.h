#ifndef ECONTAINER_SLOTS_INTERNAL_H
#define ECONTAINER_SLOTS_INTERNAL_H

#include "esp_container_product.h"

/* Package identity copied only from the durable record under the slot lock. */
typedef struct {
    uint8_t slot;
    uint8_t package_sha256[32];
    uint32_t package_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
} econtainer_slot_package_t;

typedef econtainer_slots_result_t (*econtainer_slot_selected_fn)(
    void *context, const econtainer_slot_package_t *package, uint32_t offset_bytes,
    const econtainer_slot_references_t *references);

/* Component-private, synchronous, no durable mutation. The firmware set must
 * remain stable under Base's existing outer owner. Callback runs under the
 * existing slot lock and must not reenter slot operations or retain pointers.
 * Selection never resumes another boot's trial or promotes a prepared package. */
econtainer_slots_result_t econtainer_slots_with_selected_package(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_selection_request_t *request,
    econtainer_slot_selected_fn selected_fn, void *context);

#endif
