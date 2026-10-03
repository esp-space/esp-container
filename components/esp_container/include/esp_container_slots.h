#ifndef ESP_CONTAINER_SLOTS_H
#define ESP_CONTAINER_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECONTAINER_SLOT_COUNT 3U
#define ECONTAINER_SLOT_BINDING_COUNT 2U
#define ECONTAINER_SLOT_BLOB_BYTES 288U
#define ECONTAINER_SLOT_OPERATION_ID_BYTES 16U
#define ECONTAINER_SLOT_BOOT_ID_BYTES 16U

typedef enum {
    ECONTAINER_SLOTS_OK = 0,
    ECONTAINER_SLOTS_EMPTY,
    ECONTAINER_SLOTS_INVALID,
    ECONTAINER_SLOTS_IO_FAILED,
    ECONTAINER_SLOTS_UNCERTAIN,
    ECONTAINER_SLOTS_BUSY,
    ECONTAINER_SLOTS_NO_SPACE,
    ECONTAINER_SLOTS_CONFLICT,
    ECONTAINER_SLOTS_UNTRUSTED,
} econtainer_slots_result_t;

typedef enum {
    ECONTAINER_SLOT_BLOB_FOUND = 0,
    ECONTAINER_SLOT_BLOB_NOT_FOUND,
    ECONTAINER_SLOT_BLOB_READ_FAILED,
} econtainer_slot_blob_result_t;

typedef enum {
    ECONTAINER_SLOT_IDLE = 0,
    ECONTAINER_SLOT_WRITING,
    ECONTAINER_SLOT_PREPARED,
    ECONTAINER_SLOT_TRIAL_STARTED,
    ECONTAINER_SLOT_HEALTH_VERIFIED,
    ECONTAINER_SLOT_CONFIRMED,
    ECONTAINER_SLOT_ABORTED,
} econtainer_slot_phase_t;

typedef struct {
    uint32_t offset_bytes;
    uint32_t size_bytes;
} econtainer_slot_region_t;

typedef struct {
    uint32_t partition_offset_bytes;
    uint32_t partition_size_bytes;
    uint32_t erase_unit_bytes;
    uint32_t write_unit_bytes;
    econtainer_slot_region_t slots[ECONTAINER_SLOT_COUNT];
} econtainer_slots_geometry_t;

/* One entry per independently bootable firmware digest. An absent package is explicit. */
typedef struct {
    bool present;
    bool package_present;
    uint8_t slot;
    uint8_t firmware_sha256[32];
    uint8_t package_sha256[32];
    uint32_t package_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
} econtainer_slot_binding_t;

typedef enum {
    ECONTAINER_SLOT_PACKAGE_WRITE = 0,
    ECONTAINER_SLOT_PACKAGE_REUSE,
    ECONTAINER_SLOT_NO_PACKAGE,
} econtainer_slot_operation_kind_t;

typedef struct {
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t target_firmware_sha256[32];
    uint8_t package_sha256[32];
    uint8_t slot;
    econtainer_slot_operation_kind_t kind;
    bool firmware_transition;
    uint32_t package_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
    uint8_t trial_boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
} econtainer_slot_operation_t;

typedef struct {
    uint32_t sequence;
    econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT];
    econtainer_slot_phase_t phase;
    econtainer_slot_operation_t operation;
} econtainer_slots_state_t;

/*
 * Base supplies exact bootable firmware digests from real app/OTA state while
 * serializing firmware changes with package operations. Only the first
 * bootable_count entries are populated; running must be one of them.
 */
typedef struct {
    uint8_t bootable_count;
    uint8_t bootable_firmware_sha256[ECONTAINER_SLOT_BINDING_COUNT][32];
    uint8_t running_firmware_sha256[32];
} econtainer_slot_firmware_set_t;

/*
 * write_blob returns true only after committing one NVS key durably.
 * read_blob must fetch the committed view, never an uncommitted handle cache.
 */
typedef struct {
    bool (*lock)(void *context);
    void (*unlock)(void *context);
    econtainer_slot_blob_result_t (*read_blob)(void *context,
                                                uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES]);
    bool (*write_blob)(void *context,
                       const uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES]);
    bool (*flash_read)(void *context, uint32_t offset_bytes,
                       uint8_t *destination, size_t size_bytes);
    bool (*flash_erase)(void *context, uint32_t offset_bytes, uint32_t size_bytes);
    bool (*flash_write)(void *context, uint32_t offset_bytes,
                        const uint8_t *source, size_t size_bytes);
    /* Optional for storage-only consumers, required by the private loader.
     * Called only under this same lock. Success exposes exactly size_bytes
     * immutable bytes until flash_unmap; failure leaves no mapping and clears
     * both outputs. A successful handle may be zero. No write/commit is allowed
     * between map and unmap. These are short-lived loader resources, not leases.
     * Unmap returns false when resource or physical I/O lease release fails;
     * the loader must close any new runtime and return an I/O failure. */
    bool (*flash_map)(void *context, uint32_t offset_bytes, size_t size_bytes,
                      const uint8_t **mapped, uintptr_t *handle);
    bool (*flash_unmap)(void *context, uintptr_t handle);
    void *context;
} econtainer_slots_io_t;

typedef bool (*econtainer_slot_read_fn)(void *context, size_t relative_offset_bytes,
                                         uint8_t *destination, size_t size_bytes);
typedef bool (*econtainer_slot_source_fn)(void *context, size_t relative_offset_bytes,
                                           uint8_t *destination, size_t size_bytes);
/* Borrowed exact durable bindings under the existing slot lock. The reader
 * permits only package-present binding indexes, nonzero reads <=512 bytes,
 * and offsets inside that binding's package. Do not retain or reenter. */
typedef struct {
    const econtainer_slot_binding_t *bindings;
    bool (*read)(void *context, unsigned binding_index, size_t relative_offset_bytes,
                 uint8_t *destination, size_t size_bytes);
    void *context;
} econtainer_slot_references_t;

typedef enum {
    ECONTAINER_SLOT_VALIDATION_OK = 0,
    ECONTAINER_SLOT_VALIDATION_UNTRUSTED,
    ECONTAINER_SLOT_VALIDATION_IO_FAILED,
} econtainer_slot_validation_result_t;
/*
 * The operation is the exact durable reservation read under the slot lock.
 * Validate against it, not a separately held copy that may have changed.
 * Must verify signature, Wasm/profile, product authorization and grant.
 * A read failure is distinct from a complete but rejected package.
 */
typedef econtainer_slot_validation_result_t (*econtainer_slot_validate_fn)(
    void *context, const econtainer_slot_operation_t *operation,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references);

/* For a firmware transition that reuses an already confirmed package. The
 * platform supplies the proposed firmware's independent product/grant policy. */
typedef econtainer_slot_validation_result_t (*econtainer_slot_validate_binding_fn)(
    void *context, const econtainer_slot_binding_t *binding,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references);

/* The unique executor owner checks that this trial has stopped and has no native references. */
typedef bool (*econtainer_slot_trial_stopped_fn)(
    void *context, const uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES]);

/* The unique executor owner proves that this exact confirmed instance has
 * stopped and all native references have been reclaimed. Called under the
 * package-slot lock; it must not enter another slot operation. */
typedef bool (*econtainer_slot_instance_stopped_fn)(
    void *context, const econtainer_slot_binding_t *binding);

typedef enum {
    ECONTAINER_SLOT_BOOT_BLOCKED = 0,
    ECONTAINER_SLOT_BOOT_CONFIRMED,
    ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED,
    ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID,
    ECONTAINER_SLOT_BOOT_START_TRIAL,
} econtainer_slot_boot_decision_t;

bool econtainer_slots_geometry_valid(const econtainer_slots_geometry_t *geometry);

/* Explicit first installation/migration; bindings must equal the actual firmware set. */
econtainer_slots_result_t econtainer_slots_initialize(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set,
    const econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT]);

econtainer_slots_result_t econtainer_slots_load(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    econtainer_slots_state_t *state);

/* Base owns app/otadata and has fully verified the signed inactive image.
 * While holding that owner, replace the inactive firmware identity and commit
 * one pending operation in the same NVS blob before selecting boot. WRITE
 * reserves a protected, unreferenced package slot before any erase; REUSE
 * validates the running firmware's confirmed package for the new firmware and
 * stores PREPARED without copying it; NO_PACKAGE stores explicit PREPARED with
 * no guest. Running firmware and its confirmed package remain unchanged.
 * The candidate is never a confirmed package until confirm succeeds after
 * firmware VALID. On uncertain commit, read the persisted state before any
 * retry or Flash/otadata write. The transition flag is set by this API; the
 * caller supplies it as false. */
econtainer_slots_result_t econtainer_slots_stage_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const econtainer_slot_firmware_set_t *prepared_set,
    const econtainer_slot_operation_t *operation,
    econtainer_slot_validate_binding_fn validate_fn, void *validate_context,
    econtainer_slots_state_t *state);

/* Base has a durable OTA write receipt and, under the same firmware/storage
 * owner, has verified the signed running firmware is OTA VALID and the exact
 * retired inactive image can no longer boot. actual_set must contain only the
 * running firmware. Retire that old binding only from IDLE/CONFIRMED, after
 * checking all existing confirmed package references, and persist A-only as
 * IDLE in the same ECS2 blob. An already IDLE A-only state with the current
 * sequence is a validated no-op; a stale sequence is always a conflict.
 * Container cannot inspect the receipt, app image or otadata itself. */
econtainer_slots_result_t econtainer_slots_retire_inactive_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const econtainer_slot_firmware_set_t *actual_set,
    const uint8_t retired_firmware_sha256[32],
    econtainer_slots_state_t *state);

/* After explicit abandonment and Base proof that the failed target is no
 * longer bootable, remove that inactive pending binding. Never erase a package
 * here. The only remaining binding must exactly match actual_set. */
econtainer_slots_result_t econtainer_slots_drop_aborted_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const econtainer_slot_firmware_set_t *actual_set,
    econtainer_slots_state_t *state);

/*
 * Hash confirmed references first. If only a complete pending candidate is
 * damaged, return its IO/UNTRUSTED error while preserving state and setting
 * BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID. That decision allows only the
 * separately verified old confirmed binding; it forbids candidate progress
 * and further erasure until the operation is explicitly canceled. A mismatch
 * with the real bootable firmware set blocks startup; it is not auto-migrated.
 */
econtainer_slots_result_t econtainer_slots_reconcile(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision);

/*
 * Reserve an unreferenced slot in the durable blob BEFORE the caller may erase
 * it. The firmware set must match both persisted bindings exactly.
 */
econtainer_slots_result_t econtainer_slots_reserve(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *firmware_set,
    const econtainer_slot_operation_t *operation,
    econtainer_slots_state_t *state);

/* Erase/write only the reserved slot, hash full Flash readback, then validate it. */
econtainer_slots_result_t econtainer_slots_write_and_prepare(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, econtainer_slot_source_fn source_fn,
    void *source_context, econtainer_slot_validate_fn validate_fn,
    void *validate_context, econtainer_slots_state_t *state);

/* Product-only uninstall. Base holds the app/package operation owner, has
 * resolved any selected firmware OTA receipt and stopped the current confirmed
 * instance. Match the exact firmware set,
 * sequence and package digest, then clear only the running firmware's package
 * binding in one committed and independently read-back blob. The operation ID
 * remains in a terminal NO_PACKAGE record for result lookup. Neither package
 * Flash nor product data is erased; another bootable firmware's binding stays
 * protected. A failed or uncertain commit must be resolved from durable state
 * before another operation, never assumed to have removed the package. */
econtainer_slots_result_t econtainer_slots_uninstall(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *firmware_set,
    const uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES],
    const uint8_t expected_package_sha256[32],
    econtainer_slot_instance_stopped_fn instance_stopped_fn, void *instance_context,
    econtainer_slots_state_t *state);

/* Product-only trial: caller has already stopped/reclaimed the previous instance. */
econtainer_slots_result_t econtainer_slots_begin_trial(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t running_firmware_sha256[32],
    const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slots_state_t *state);

econtainer_slots_result_t econtainer_slots_mark_healthy(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slots_state_t *state);

/* Commit a product-only or firmware/package trial. Health proof, persisted
 * trial boot ID and actual running firmware must match this operation. For a
 * firmware transition, Base must first prove the signed target is OTA VALID
 * and read back that state under the same storage owner. This API cannot
 * inspect otadata. After a reboot in HEALTH_VERIFIED, Base may use the stored
 * original trial boot ID only after that VALID proof and local boot checks;
 * reconcile alone never authorizes a new trial or confirmation. */
econtainer_slots_result_t econtainer_slots_confirm(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t running_firmware_sha256[32],
    const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slots_state_t *state);

/*
 * Explicit cancellation. A trial in the same boot requires trial_stopped_fn
 * to prove native stop/reclamation under the storage owner lock. A different
 * boot has no surviving in-memory trial instance. Cancellation never requires
 * the discarded candidate's bytes to be intact.
 */
econtainer_slots_result_t econtainer_slots_abandon(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slot_trial_stopped_fn trial_stopped_fn, void *trial_context,
    econtainer_slots_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
