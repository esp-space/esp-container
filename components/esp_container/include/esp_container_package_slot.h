#ifndef ESP_CONTAINER_PACKAGE_SLOT_H
#define ESP_CONTAINER_PACKAGE_SLOT_H

#include "esp_container_package.h"
#include "esp_container_slots.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Product and limits come from independent platform policy. The validator
 * receives the durable operation directly from the slot engine. All pointers
 * remain valid throughout the call under the shared slot lock.
 */
typedef struct {
    const char *expected_product_id;
    const uint8_t *public_key_rsa_der;
    size_t public_key_size_bytes;
    const char *expected_key_id;
    size_t max_wasm_bytes;
    econtainer_wasm_authorization_t wasm_authorization;
    uint32_t max_event_queue_limit;
    uint32_t max_instruction_budget;
    uint32_t max_host_call_timeout_ms;
    uint32_t max_storage_limit_bytes;
    econtainer_package_workspace_t *package_workspace;
    econtainer_wasm_workspace_t *wasm_workspace;
    econtainer_package_info_t *verified_info;
} econtainer_package_slot_validation_t;

/*
 * Drop-in validate_fn for econtainer_slots_write_and_prepare. It reads the
 * actual candidate Flash through the slot's bounded callback, verifies the
 * signed package and Wasm, and checks signed metadata against the durable
 * operation and independent product/resource policy. A successful result
 * admits only this immutable slot snapshot; it does not start or activate a
 * guest. Every other package-slot writer must honor the same storage lock.
 */
econtainer_slot_validation_result_t econtainer_package_slot_validate(
    void *context, const econtainer_slot_operation_t *operation,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references);

/* Recheck a confirmed package against the independently supplied policy for
 * a newly signed firmware before storing a REUSE prepared operation. */
econtainer_slot_validation_result_t econtainer_package_slot_validate_binding(
    void *context, const econtainer_slot_binding_t *binding,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references);

#ifdef __cplusplus
}
#endif

#endif
