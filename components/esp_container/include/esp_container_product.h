#ifndef ESP_CONTAINER_PRODUCT_H
#define ESP_CONTAINER_PRODUCT_H

#include "esp_container_package_slot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The instance is opaque. There is no public raw-Wasm loading entry: every
 * product instance must come from a selected, durable package-slot binding. */
typedef struct econtainer_runtime econtainer_runtime_t;

typedef struct {
    uint32_t max_wasm_bytes;
    uint32_t max_memory_pages;
    uint32_t stack_size_bytes;
    uint32_t max_event_bytes;
    uint32_t allowed_capabilities;
    uint32_t max_log_bytes;
    uint32_t max_timers;
    int32_t init_instruction_budget;
    int32_t event_instruction_budget;
    int32_t stop_instruction_budget;
    /* Bounds accepted results, not synchronous WAMR/SDK return latency. */
    uint32_t max_entry_duration_ms;
} econtainer_runtime_limits_t;

typedef enum {
    ECONTAINER_RUNTIME_OK = 0,
    ECONTAINER_RUNTIME_INVALID_INPUT,
    ECONTAINER_RUNTIME_BUSY,
    ECONTAINER_RUNTIME_INVALID_STATE,
    ECONTAINER_RUNTIME_BAD_WASM,
    ECONTAINER_RUNTIME_BAD_ABI,
    ECONTAINER_RUNTIME_NO_MEMORY,
    ECONTAINER_RUNTIME_ENGINE_FAILURE,
    ECONTAINER_RUNTIME_INSTRUCTION_LIMIT,
    ECONTAINER_RUNTIME_GUEST_FAILURE,
    ECONTAINER_RUNTIME_NO_LOG,
    ECONTAINER_RUNTIME_NO_TIMER,
    ECONTAINER_RUNTIME_NOT_AUTHORIZED,
    ECONTAINER_RUNTIME_ENTRY_EXPIRED,
} econtainer_runtime_result_t;

typedef struct {
    uint64_t handle;
    uint32_t skipped_periods;
} econtainer_timer_event_t;

typedef enum {
    ECONTAINER_SLOT_SELECT_CONFIRMED,
    ECONTAINER_SLOT_SELECT_TRIAL,
} econtainer_slot_selection_t;

typedef struct {
    uint32_t expected_sequence;
    econtainer_slot_firmware_set_t firmware_set;
    econtainer_slot_selection_t selection;
    /* TRIAL requires both exact IDs from this boot's executor owner.
     * CONFIRMED requires both arrays to be zero. */
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
} econtainer_slot_selection_request_t;

/* A storage, firmware-set, sequence or signed-package admission failure leaves
 * runtime INVALID_STATE. Once admission passed, runtime reports the separate
 * WAMR result, including a tighter runtime grant's rejection. Success requires
 * both OK and a non-NULL instance. */
typedef struct {
    econtainer_slots_result_t slots;
    econtainer_runtime_result_t runtime;
    /* Populated only with a live runtime after the selected Flash package was
     * reverified. The caller uses the signed event limit and package identity
     * to bind its external event queue; failure leaves both fields zero. */
    uint32_t event_queue_limit;
    uint8_t package_sha256[32];
    /* Freshly admitted manifest metadata. Identifier slices refer to the
     * caller's validation.package_workspace->manifest after return, until that
     * workspace is modified. No mapping or runtime-owned pointer escapes.
     * All metadata stays zero unless both results are OK and runtime is live. */
    size_t product_id_offset_bytes;
    size_t product_id_size_bytes;
    size_t product_version_offset_bytes;
    size_t product_version_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
} econtainer_slot_runtime_result_t;

/* Base owns the outer app/otadata and package-operation serialization. The
 * real, signed firmware set must remain stable across this call. io must be
 * the shared package-slot provider and use its own distinct lock. A confirmed
 * request selects only the running firmware's persisted confirmed binding;
 * trial requires the exact durable TRIAL_STARTED operation and current boot.
 * No package is an EMPTY selection, not a fabricated guest.
 *
 * The implementation rechecks the selected Flash bytes, signature, product,
 * ABI and independent grants under the slot lock. It releases the temporary
 * mapping before returning. validation workspaces are caller-owned for this
 * synchronous call; verified_info is ignored. Signed limits are intersected
 * with the independent runtime_limits. out must point to NULL.
 * On ESP-IDF the unique executor owner must be a pthread_create thread. A
 * successful result also returns the reverified signed queue limit and package
 * digest and manifest identifier slices/ABI/schema, so the caller need not
 * trust an earlier admission snapshot. Copy identifier bytes before reusing
 * the validation workspace; the package format's manifest bound is unchanged. */
econtainer_slot_runtime_result_t econtainer_product_open(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_selection_request_t *request,
    const econtainer_package_slot_validation_t *validation,
    const econtainer_runtime_limits_t *runtime_limits,
    econtainer_runtime_t **out);

/* Only the same serialized owner calls these. Open does not execute guest
 * code; init is explicit. Event bytes are copied into the guest and cleared
 * on return. A failed/trapped instance cannot receive another event. Stop
 * failure is not success: the owner must retain its native resources until
 * references have been reclaimed, then close the failed instance. */
econtainer_runtime_result_t econtainer_product_init(econtainer_runtime_t *runtime);
econtainer_runtime_result_t econtainer_product_on_event(econtainer_runtime_t *runtime,
                                                       const uint8_t *event,
                                                       size_t event_size_bytes,
                                                       int32_t *guest_result);
econtainer_runtime_result_t econtainer_product_stop(econtainer_runtime_t *runtime);
/* These are owner-driven drains for the already implemented LOG/TIMER imports.
 * No callback enters guest code from a native timer or a network task. */
econtainer_runtime_result_t econtainer_product_take_log(econtainer_runtime_t *runtime,
                                                       uint8_t *output,
                                                       size_t output_capacity,
                                                       size_t *log_size_bytes);
econtainer_runtime_result_t econtainer_product_poll_timer(econtainer_runtime_t *runtime,
                                                         econtainer_timer_event_t *event,
                                                         int32_t *guest_result);
econtainer_runtime_result_t econtainer_product_next_timer_deadline(
    econtainer_runtime_t *runtime, uint64_t *deadline_ms);
econtainer_runtime_result_t econtainer_product_close(econtainer_runtime_t **runtime);

#ifdef __cplusplus
}
#endif

#endif
