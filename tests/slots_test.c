#include "esp_container_slots.h"

#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

enum { FLASH_BASE = 0x10000, SLOT_BYTES = 4096, FLASH_BYTES = SLOT_BYTES * 3 };

typedef struct {
    uint8_t flash[FLASH_BYTES];
    uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES];
    bool blob_present;
    bool locked;
    bool fail_lock;
    bool fail_blob_read;
    bool fail_next_blob_read;
    bool fail_read_after_write;
    bool fail_blob_write_before;
    bool fail_blob_write_after;
    bool corrupt_blob_write;
    bool fail_erase;
    bool fail_flash_write;
    bool corrupt_flash_write;
    bool fail_flash_read;
    unsigned fail_flash_read_slot;
    bool trial_stopped;
    unsigned blob_write_count;
    unsigned erase_count[ECONTAINER_SLOT_COUNT];
} fake_store_t;

typedef struct {
    uint8_t bytes[SLOT_BYTES + 1U];
    size_t length;
    bool reject_validation;
    bool validation_called;
} fixture_t;

static const econtainer_slots_geometry_t geometry = {
    .partition_offset_bytes = FLASH_BASE,
    .partition_size_bytes = FLASH_BYTES,
    .erase_unit_bytes = SLOT_BYTES,
    .write_unit_bytes = 4,
    .slots = {
        {FLASH_BASE, SLOT_BYTES},
        {FLASH_BASE + SLOT_BYTES, SLOT_BYTES},
        {FLASH_BASE + 2 * SLOT_BYTES, SLOT_BYTES},
    },
};

static bool fake_lock(void *context)
{
    fake_store_t *store = context;
    if (store->locked || store->fail_lock) {
        return false;
    }
    store->locked = true;
    return true;
}

static void fake_unlock(void *context)
{
    fake_store_t *store = context;
    assert(store->locked);
    store->locked = false;
}

static econtainer_slot_blob_result_t fake_blob_read(
    void *context, uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES])
{
    fake_store_t *store = context;
    assert(store->locked);
    if (store->fail_blob_read) {
        return ECONTAINER_SLOT_BLOB_READ_FAILED;
    }
    if (store->fail_next_blob_read) {
        store->fail_next_blob_read = false;
        return ECONTAINER_SLOT_BLOB_READ_FAILED;
    }
    if (!store->blob_present) {
        return ECONTAINER_SLOT_BLOB_NOT_FOUND;
    }
    memcpy(blob, store->blob, ECONTAINER_SLOT_BLOB_BYTES);
    return ECONTAINER_SLOT_BLOB_FOUND;
}

static bool fake_blob_write(void *context,
                            const uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES])
{
    fake_store_t *store = context;
    assert(store->locked);
    ++store->blob_write_count;
    if (store->fail_blob_write_before) {
        return false;
    }
    memcpy(store->blob, blob, ECONTAINER_SLOT_BLOB_BYTES);
    store->blob_present = true;
    if (store->fail_read_after_write) {
        store->fail_next_blob_read = true;
    }
    if (store->corrupt_blob_write) {
        store->blob[54] ^= 0x40;
    }
    return !store->fail_blob_write_after;
}

static bool flash_bounds(uint32_t offset_bytes, size_t size_bytes)
{
    return offset_bytes >= FLASH_BASE &&
           (uint64_t)offset_bytes + size_bytes <= FLASH_BASE + FLASH_BYTES;
}

static bool fake_flash_read(void *context, uint32_t offset_bytes,
                            uint8_t *destination, size_t size_bytes)
{
    fake_store_t *store = context;
    assert(store->locked);
    if (store->fail_flash_read || !flash_bounds(offset_bytes, size_bytes) ||
        store->fail_flash_read_slot ==
            1U + (offset_bytes - FLASH_BASE) / SLOT_BYTES) {
        return false;
    }
    memcpy(destination, store->flash + (offset_bytes - FLASH_BASE), size_bytes);
    return true;
}

static bool fake_flash_erase(void *context, uint32_t offset_bytes,
                             uint32_t size_bytes)
{
    fake_store_t *store = context;
    assert(store->locked);
    if (store->fail_erase || !flash_bounds(offset_bytes, size_bytes) ||
        size_bytes != SLOT_BYTES || (offset_bytes - FLASH_BASE) % SLOT_BYTES != 0) {
        return false;
    }
    ++store->erase_count[(offset_bytes - FLASH_BASE) / SLOT_BYTES];
    memset(store->flash + (offset_bytes - FLASH_BASE), 0xff, size_bytes);
    return true;
}

static bool fake_flash_write(void *context, uint32_t offset_bytes,
                             const uint8_t *source, size_t size_bytes)
{
    fake_store_t *store = context;
    assert(store->locked);
    if (!flash_bounds(offset_bytes, size_bytes) || size_bytes % 4 != 0) {
        return false;
    }
    if (store->fail_flash_write && offset_bytes % SLOT_BYTES >= 256) {
        return false;
    }
    for (size_t index = 0; index < size_bytes; ++index) {
        uint8_t *destination = &store->flash[offset_bytes - FLASH_BASE + index];
        if ((*destination & source[index]) != source[index]) {
            return false;
        }
        *destination &= source[index];
    }
    if (store->corrupt_flash_write) {
        store->flash[offset_bytes - FLASH_BASE] ^= 1;
    }
    return true;
}

static econtainer_slots_io_t fake_io(fake_store_t *store)
{
    const econtainer_slots_io_t io = {
        .lock = fake_lock,
        .unlock = fake_unlock,
        .read_blob = fake_blob_read,
        .write_blob = fake_blob_write,
        .flash_read = fake_flash_read,
        .flash_erase = fake_flash_erase,
        .flash_write = fake_flash_write,
        .context = store,
    };
    return io;
}

static bool trial_stopped_proof(void *context,
                                const uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES])
{
    const fake_store_t *store = context;
    return store->locked && store->trial_stopped && operation_id[0] == 2;
}

typedef struct {
    fake_store_t *store;
    uint8_t package_sha256[32];
    bool stopped;
    unsigned calls;
} uninstall_proof_t;

static bool instance_stopped_proof(void *context,
                                   const econtainer_slot_binding_t *binding)
{
    uninstall_proof_t *proof = context;
    ++proof->calls;
    return proof->store->locked && proof->stopped && binding->present &&
           binding->package_present &&
           binding->firmware_sha256[0] == 0xb0 &&
           memcmp(binding->package_sha256, proof->package_sha256, 32) == 0;
}

static void make_fixture(fixture_t *fixture, uint8_t seed)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->length = 513 + seed;
    for (size_t index = 0; index < fixture->length; ++index) {
        fixture->bytes[index] = (uint8_t)(seed + index * 17U);
    }
}

static void make_oversize_fixture(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->length = SLOT_BYTES + 1U;
    for (size_t index = 0; index < fixture->length; ++index) {
        fixture->bytes[index] = (uint8_t)(index * 17U);
    }
}

static void digest_fixture(const fixture_t *fixture, uint8_t result[32])
{
    assert(SHA256(fixture->bytes, fixture->length, result) != NULL);
}

static bool source_read(void *context, size_t offset_bytes,
                        uint8_t *destination, size_t size_bytes)
{
    const fixture_t *fixture = context;
    if (offset_bytes > fixture->length || size_bytes > fixture->length - offset_bytes) {
        return false;
    }
    memcpy(destination, fixture->bytes + offset_bytes, size_bytes);
    return true;
}

static econtainer_slot_validation_result_t validate_readback(
    void *context, const econtainer_slot_operation_t *operation,
    econtainer_slot_read_fn read_fn, void *read_context, size_t size_bytes)
{
    fixture_t *fixture = context;
    uint8_t chunk[97];
    fixture->validation_called = true;
    if (fixture->reject_validation || size_bytes != fixture->length ||
        operation->package_size_bytes != fixture->length) {
        return ECONTAINER_SLOT_VALIDATION_UNTRUSTED;
    }
    for (size_t offset = 0; offset < size_bytes; offset += sizeof(chunk)) {
        const size_t count = size_bytes - offset < sizeof(chunk) ?
                             size_bytes - offset : sizeof(chunk);
        if (!read_fn(read_context, offset, chunk, count)) {
            return ECONTAINER_SLOT_VALIDATION_IO_FAILED;
        }
        if (memcmp(chunk, fixture->bytes + offset, count) != 0) {
            return ECONTAINER_SLOT_VALIDATION_UNTRUSTED;
        }
    }
    return read_fn(read_context, size_bytes, chunk, 1)
               ? ECONTAINER_SLOT_VALIDATION_UNTRUSTED
               : ECONTAINER_SLOT_VALIDATION_OK;
}

static econtainer_slot_binding_t binding(uint8_t firmware_seed, uint8_t slot,
                                         const fixture_t *package)
{
    econtainer_slot_binding_t result = {0};
    result.present = true;
    memset(result.firmware_sha256, firmware_seed, 32);
    if (package != NULL) {
        result.package_present = true;
        result.slot = slot;
        result.package_size_bytes = (uint32_t)package->length;
        result.guest_abi_version = 2;
        result.data_schema_version = 1;
        digest_fixture(package, result.package_sha256);
    }
    return result;
}

static econtainer_slot_operation_t operation(uint8_t id, uint8_t firmware_seed,
                                             const fixture_t *package)
{
    econtainer_slot_operation_t result = {0};
    result.operation_id[0] = id;
    memset(result.target_firmware_sha256, firmware_seed, 32);
    digest_fixture(package, result.package_sha256);
    result.package_size_bytes = (uint32_t)package->length;
    result.guest_abi_version = 2;
    result.data_schema_version = 1;
    return result;
}

static econtainer_slot_firmware_set_t firmware_set(uint8_t running_seed)
{
    econtainer_slot_firmware_set_t result = {0};
    result.bootable_count = 2;
    memset(result.bootable_firmware_sha256[0], 0xa0, 32);
    memset(result.bootable_firmware_sha256[1], 0xb0, 32);
    memset(result.running_firmware_sha256, running_seed, 32);
    return result;
}

static void seed_two_packages(fake_store_t *store, fixture_t *p0, fixture_t *p1)
{
    memset(store, 0, sizeof(*store));
    memset(store->flash, 0xff, sizeof(store->flash));
    make_fixture(p0, 10);
    make_fixture(p1, 11);
    memcpy(store->flash, p0->bytes, p0->length);
    memcpy(store->flash + SLOT_BYTES, p1->bytes, p1->length);
    const econtainer_slot_binding_t bindings[2] = {
        binding(0xa0, 0, p0), binding(0xb0, 1, p1),
    };
    const econtainer_slots_io_t io = fake_io(store);
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    assert(econtainer_slots_initialize(&io, &geometry, &bootable,
                                       bindings) == ECONTAINER_SLOTS_OK);
}

static void apply_product(fake_store_t *store, fixture_t *package, uint8_t id,
                          uint8_t expected_slot)
{
    econtainer_slots_io_t io = fake_io(store);
    econtainer_slots_state_t state;
    uint8_t firmware[32];
    memset(firmware, 0xb0, sizeof(firmware));
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES] = {0};
    boot_id[0] = id;
    const econtainer_slot_operation_t candidate = operation(id, 0xb0, package);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence, &bootable,
                                    &candidate, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_WRITING && state.operation.slot == expected_slot);
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, package, validate_readback, package, &state) == ECONTAINER_SLOTS_OK);
    assert(package->validation_called && state.phase == ECONTAINER_SLOT_PREPARED);
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
                                        firmware, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
                                         boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_confirm(&io, &geometry, state.sequence,
                                    firmware, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_CONFIRMED);
    assert(state.bindings[0].slot == 0 && state.bindings[1].slot == expected_slot);
}

static void test_four_packages(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2, p3, oversized;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    make_fixture(&p3, 13);
    const econtainer_slots_io_t io = fake_io(&store);
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    const econtainer_slot_firmware_set_t old_bootable = firmware_set(0xa0);
    assert(econtainer_slots_reconcile(&io, &geometry, &old_bootable,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED && state.bindings[0].slot == 0);
    apply_product(&store, &p2, 2, 2);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0 &&
           store.erase_count[2] == 1);
    assert(econtainer_slots_reconcile(&io, &geometry, &old_bootable,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED && state.bindings[0].slot == 0);
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED && state.bindings[1].slot == 2);
    make_oversize_fixture(&oversized);
    const econtainer_slot_operation_t no_space = operation(4, 0xb0, &oversized);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence, &bootable,
                                    &no_space, &state) == ECONTAINER_SLOTS_NO_SPACE);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0 &&
           store.erase_count[2] == 1);
    apply_product(&store, &p3, 3, 1);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 1 &&
           store.erase_count[2] == 1);
    assert(memcmp(store.flash, p0.bytes, p0.length) == 0);
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED && state.bindings[1].slot == 1);
    assert(econtainer_slots_reconcile(&io, &geometry, &old_bootable,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED && state.bindings[0].slot == 0);
}

static void test_firmware_set_and_capacity_guards(void)
{
    fake_store_t store;
    fixture_t p0, p1, oversized;
    seed_two_packages(&store, &p0, &p1);
    make_oversize_fixture(&oversized);
    const econtainer_slots_io_t io = fake_io(&store);
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t sequence = state.sequence;
    uint8_t saved_blob[ECONTAINER_SLOT_BLOB_BYTES];
    memcpy(saved_blob, store.blob, sizeof(saved_blob));

    econtainer_slot_firmware_set_t stale = bootable;
    memset(stale.bootable_firmware_sha256[0], 0xc0, 32);
    assert(econtainer_slots_reconcile(&io, &geometry, &stale,
                                      &state, &decision) == ECONTAINER_SLOTS_CONFLICT);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED && state.sequence == 0U);
    const econtainer_slot_operation_t p2 = operation(2, 0xb0, &p1);
    assert(econtainer_slots_reserve(&io, &geometry, sequence, &stale,
                                    &p2, &state) == ECONTAINER_SLOTS_CONFLICT);

    stale = bootable;
    stale.bootable_count = 1;
    memset(stale.bootable_firmware_sha256[0], 0xb0, 32);
    memset(stale.bootable_firmware_sha256[1], 0, 32);
    assert(econtainer_slots_reconcile(&io, &geometry, &stale,
                                      &state, &decision) == ECONTAINER_SLOTS_CONFLICT);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED);
    stale = bootable;
    memcpy(stale.bootable_firmware_sha256[0],
           stale.bootable_firmware_sha256[1], 32);
    assert(econtainer_slots_reconcile(&io, &geometry, &stale,
                                      &state, &decision) == ECONTAINER_SLOTS_INVALID);
    stale = bootable;
    memset(stale.running_firmware_sha256, 0xc0, 32);
    assert(econtainer_slots_reconcile(&io, &geometry, &stale,
                                      &state, &decision) == ECONTAINER_SLOTS_INVALID);

    econtainer_slot_firmware_set_t reordered = bootable;
    memcpy(reordered.bootable_firmware_sha256[0],
           bootable.bootable_firmware_sha256[1], 32);
    memcpy(reordered.bootable_firmware_sha256[1],
           bootable.bootable_firmware_sha256[0], 32);
    assert(econtainer_slots_reconcile(&io, &geometry, &reordered,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    assert(econtainer_slots_reconcile(&io, &geometry, &stale,
                                      &state, &decision) == ECONTAINER_SLOTS_INVALID);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED && state.sequence == 0U);

    const econtainer_slot_operation_t too_large = operation(3, 0xb0, &oversized);
    assert(econtainer_slots_reserve(&io, &geometry, sequence, &bootable,
                                    &too_large, &state) == ECONTAINER_SLOTS_NO_SPACE);
    assert(memcmp(saved_blob, store.blob, sizeof(saved_blob)) == 0);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0 &&
           store.erase_count[2] == 0);
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
                                      &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(state.bindings[0].slot == 0 && state.bindings[1].slot == 1);
}

static void test_commit_readback_and_torn_flash(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    const econtainer_slot_operation_t candidate = operation(2, 0xb0, &p2);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t original_sequence = state.sequence;
    store.fail_blob_write_before = true;
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_IO_FAILED);
    assert(store.erase_count[2] == 0);
    store.fail_blob_write_before = false;
    store.fail_blob_write_after = true;
    assert(econtainer_slots_reserve(&io, &geometry, original_sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    assert(store.erase_count[2] == 0);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_WRITING && store.erase_count[2] == 0);
    store.fail_blob_write_after = false;
    store.fail_erase = true;
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_IO_FAILED);
    assert(store.erase_count[2] == 0);
    store.fail_erase = false;
    store.fail_flash_write = true;
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_IO_FAILED);
    assert(store.erase_count[2] == 1);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_WRITING);
    econtainer_slot_boot_decision_t decision;
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED);
    assert(memcmp(store.flash, p0.bytes, p0.length) == 0 &&
           memcmp(store.flash + SLOT_BYTES, p1.bytes, p1.length) == 0);
    store.fail_flash_write = false;
    store.corrupt_flash_write = true;
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(!p2.validation_called);
    store.corrupt_flash_write = false;
    p2.reject_validation = true;
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(p2.validation_called);
    p2.reject_validation = false;
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_OK);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);
    store.flash[2 * SLOT_BYTES] ^= 1;
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID);
    assert(state.phase == ECONTAINER_SLOT_PREPARED && state.bindings[1].slot == 1);
    const econtainer_slot_operation_t next_candidate = operation(3, 0xb0, &p2);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &next_candidate, &state) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);
}

static void test_unknown_recovery_and_record_damage(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    uint8_t firmware[32];
    memset(firmware, 0xb0, sizeof(firmware));
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    const econtainer_slot_operation_t candidate = operation(2, 0xb0, &p2);
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    uint8_t old_boot[ECONTAINER_SLOT_BOOT_ID_BYTES] = {1};
    uint8_t new_boot[ECONTAINER_SLOT_BOOT_ID_BYTES] = {2};
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    store.fail_blob_read = true;
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_IO_FAILED);
    assert(store.erase_count[2] == 0);
    store.fail_blob_read = false;
    store.corrupt_blob_write = true;
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    assert(store.erase_count[2] == 0);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_INVALID);
    store.corrupt_blob_write = false;
    assert(econtainer_slots_reserve(&io, &geometry, 1, &bootable,
           &candidate, &state) == ECONTAINER_SLOTS_INVALID);
    assert(store.erase_count[2] == 0);

    seed_two_packages(&store, &p0, &p1);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
           firmware, old_boot, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED);
    assert(state.bindings[1].slot == 1);
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
                                    old_boot, NULL, NULL, &state) == ECONTAINER_SLOTS_BUSY);
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
                                    new_boot, NULL, NULL, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_ABORTED && state.bindings[1].slot == 1);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);
}

static void test_readback_boundary_and_confirm(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    uint8_t firmware[32];
    memset(firmware, 0xb0, sizeof(firmware));
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    const econtainer_slot_operation_t candidate = operation(2, 0xb0, &p2);
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES] = {1};
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t initial_sequence = state.sequence;
    store.fail_read_after_write = true;
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    assert(store.erase_count[2] == 0);
    store.fail_read_after_write = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_WRITING &&
           state.sequence == initial_sequence + 1);
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED);
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
           firmware, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
           boot_id, &state) == ECONTAINER_SLOTS_OK);
    store.fail_blob_write_before = true;
    assert(econtainer_slots_confirm(&io, &geometry, state.sequence,
           firmware, boot_id, &state) == ECONTAINER_SLOTS_IO_FAILED);
    store.fail_blob_write_before = false;
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED &&
           state.bindings[1].slot == 1);
    store.fail_blob_write_after = true;
    assert(econtainer_slots_confirm(&io, &geometry, state.sequence,
           firmware, boot_id, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    store.fail_blob_write_after = false;
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED && state.bindings[1].slot == 2);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);
}

static void test_damaged_trial_same_boot_stop_proof(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    uint8_t firmware[32];
    memset(firmware, 0xb0, sizeof(firmware));
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    const econtainer_slot_operation_t candidate = operation(2, 0xb0, &p2);
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES] = {2};
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
           firmware, boot_id, &state) == ECONTAINER_SLOTS_OK);
    store.flash[2 * SLOT_BYTES] ^= 1;
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID);
    assert(state.bindings[1].slot == 1 && state.phase == ECONTAINER_SLOT_TRIAL_STARTED);
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
           boot_id, NULL, NULL, &state) == ECONTAINER_SLOTS_BUSY);
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
           boot_id, trial_stopped_proof, &store, &state) == ECONTAINER_SLOTS_BUSY);
    store.trial_stopped = true;
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
           boot_id, trial_stopped_proof, &store, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_ABORTED && state.bindings[1].slot == 1);
    const econtainer_slot_operation_t next_candidate = operation(3, 0xb0, &p2);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &next_candidate, &state) == ECONTAINER_SLOTS_OK);
    assert(state.operation.slot == 2 && store.erase_count[0] == 0 &&
           store.erase_count[1] == 0 && store.erase_count[2] == 1);
}

static void test_candidate_read_failure_keeps_confirmed(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    const econtainer_slot_operation_t candidate = operation(2, 0xb0, &p2);
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
           source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_OK);
    store.fail_flash_read_slot = 3;
    assert(econtainer_slots_reconcile(&io, &geometry, &bootable,
           &state, &decision) == ECONTAINER_SLOTS_IO_FAILED);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID);
    assert(state.phase == ECONTAINER_SLOT_PREPARED && state.bindings[1].slot == 1);
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
           (uint8_t[ECONTAINER_SLOT_BOOT_ID_BYTES]){0}, NULL, NULL, &state) ==
           ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_ABORTED);
}

static void test_guards(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    econtainer_slot_operation_t candidate = operation(2, 0xb0, &p2);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence + 1,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_CONFLICT);
    candidate.data_schema_version = 2;
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_CONFLICT);
    candidate.data_schema_version = 1;
    store.flash[0] ^= 1;
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(store.erase_count[2] == 0);
    store.flash[0] ^= 1;
    econtainer_slots_geometry_t overlap = geometry;
    overlap.slots[2].offset_bytes = overlap.slots[1].offset_bytes;
    assert(!econtainer_slots_geometry_valid(&overlap));
    assert(econtainer_slots_reserve(&io, &overlap, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_INVALID);
    store.fail_lock = true;
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
           &bootable, &candidate, &state) == ECONTAINER_SLOTS_BUSY);
}

static econtainer_slot_firmware_set_t prepared_firmware(uint8_t running_seed)
{
    econtainer_slot_firmware_set_t result = firmware_set(running_seed);
    memset(result.bootable_firmware_sha256[1], 0xc0, 32);
    return result;
}

static void test_firmware_no_package_and_rollback(void)
{
    fake_store_t store;
    fixture_t p0, p1;
    seed_two_packages(&store, &p0, &p1);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t prepared = prepared_firmware(0xa0);
    econtainer_slot_operation_t candidate = {0};
    candidate.kind = ECONTAINER_SLOT_NO_PACKAGE;
    candidate.operation_id[0] = 4;
    memset(candidate.target_firmware_sha256, 0xc0, 32);
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t first_sequence = state.sequence;
    store.fail_blob_write_before = true;
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &prepared, &candidate, NULL, NULL, &state) == ECONTAINER_SLOTS_IO_FAILED);
    store.fail_blob_write_before = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
        state.sequence == first_sequence);
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &prepared, &candidate, NULL, NULL, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_PREPARED &&
        state.bindings[0].package_present && state.bindings[0].slot == 0 &&
        state.bindings[1].present && !state.bindings[1].package_present &&
        store.erase_count[0] == 0 && store.erase_count[1] == 0);
    assert(econtainer_slots_reconcile(&io, &geometry, &prepared,
        &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED);
    econtainer_slot_firmware_set_t running_new = prepared;
    memset(running_new.running_firmware_sha256, 0xc0, 32);
    assert(econtainer_slots_reconcile(&io, &geometry, &running_new,
        &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_START_TRIAL);
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES] = {4};
    uint8_t target[32]; memset(target, 0xc0, sizeof(target));
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
        target, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reconcile(&io, &geometry, &running_new,
        &state, &decision) == ECONTAINER_SLOTS_CONFLICT);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
        boot_id, &state) == ECONTAINER_SLOTS_OK);
    /* A reboot at this point is blocked by normal reconciliation. Only Base
     * can independently prove OTA VALID, then reload this durable health
     * record and complete the original operation with its stored boot ID. */
    assert(econtainer_slots_reconcile(&io, &geometry, &running_new,
        &state, &decision) == ECONTAINER_SLOTS_CONFLICT);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_HEALTH_VERIFIED &&
        memcmp(state.operation.trial_boot_id, boot_id, sizeof(boot_id)) == 0);
    assert(econtainer_slots_confirm(&io, &geometry, state.sequence,
        target, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_CONFIRMED &&
        !state.bindings[1].package_present);
    assert(econtainer_slots_reconcile(&io, &geometry, &running_new,
        &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    assert(econtainer_slots_reconcile(&io, &geometry, &prepared,
        &state, &decision) == ECONTAINER_SLOTS_CONFLICT);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    econtainer_slot_firmware_set_t stale_prepared = prepared;
    memset(stale_prepared.bootable_firmware_sha256[1], 0xd0, 32);
    econtainer_slot_operation_t stale_candidate = {0};
    stale_candidate.kind = ECONTAINER_SLOT_NO_PACKAGE;
    stale_candidate.operation_id[0] = 7;
    memset(stale_candidate.target_firmware_sha256, 0xd0, 32);
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &stale_prepared, &stale_candidate, NULL, NULL, &state) ==
        ECONTAINER_SLOTS_CONFLICT);
    econtainer_slot_operation_t stale_product = operation(8, 0xa0, &p1);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
        &prepared, &stale_product, &state) == ECONTAINER_SLOTS_CONFLICT);

    seed_two_packages(&store, &p0, &p1);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &prepared, &candidate, NULL, NULL, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
        boot_id, NULL, NULL, &state) == ECONTAINER_SLOTS_OK);
    econtainer_slot_firmware_set_t old_only = {.bootable_count = 1};
    memset(old_only.bootable_firmware_sha256[0], 0xa0, 32);
    memset(old_only.running_firmware_sha256, 0xa0, 32);
    assert(econtainer_slots_drop_aborted_firmware(&io, &geometry, state.sequence,
        &old_only, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_IDLE &&
        state.bindings[0].package_present && !state.bindings[1].present &&
        state.sequence == first_sequence + 3);
    assert(econtainer_slots_reconcile(&io, &geometry, &old_only,
        &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    assert(memcmp(store.flash, p0.bytes, p0.length) == 0 &&
        store.erase_count[0] == 0);
}

static void test_firmware_write_before_boot(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    /* Base has replaced the signed inactive B image; its old package is no
     * longer a protected boot reference, even if that retired copy is bad. */
    store.flash[SLOT_BYTES] ^= 1;
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t prepared = prepared_firmware(0xa0);
    econtainer_slot_operation_t candidate = operation(5, 0xc0, &p2);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    store.fail_blob_write_after = true;
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &prepared, &candidate, NULL, NULL, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);
    store.fail_blob_write_after = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_WRITING && state.operation.slot == 1 &&
        !state.bindings[1].package_present && state.operation.firmware_transition);
    econtainer_slot_boot_decision_t decision;
    econtainer_slot_firmware_set_t running_new = prepared;
    memset(running_new.running_firmware_sha256, 0xc0, 32);
    assert(econtainer_slots_reconcile(&io, &geometry, &running_new,
        &state, &decision) == ECONTAINER_SLOTS_CONFLICT);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
        source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_PREPARED && p2.validation_called &&
        store.erase_count[0] == 0 && store.erase_count[1] == 1);
    store.flash[SLOT_BYTES] ^= 1;
    assert(econtainer_slots_reconcile(&io, &geometry, &running_new,
        &state, &decision) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(decision == ECONTAINER_SLOT_BOOT_BLOCKED);
    store.flash[SLOT_BYTES] ^= 1;
    assert(econtainer_slots_reconcile(&io, &geometry, &running_new,
        &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_START_TRIAL);
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES] = {5};
    uint8_t target[32]; memset(target, 0xc0, sizeof(target));
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
        target, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
        boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_confirm(&io, &geometry, state.sequence,
        target, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(state.bindings[1].package_present && state.bindings[1].slot == 1 &&
        memcmp(store.flash, p0.bytes, p0.length) == 0 &&
        memcmp(store.flash + SLOT_BYTES, p2.bytes, p2.length) == 0);
}

static void test_rebind_after_other_firmware_product_update(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    apply_product(&store, &p2, 9, 2);
    const econtainer_slots_io_t io = fake_io(&store);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_CONFIRMED &&
        !state.operation.firmware_transition);
    const econtainer_slot_firmware_set_t prepared = prepared_firmware(0xa0);
    econtainer_slot_operation_t candidate = {0};
    candidate.kind = ECONTAINER_SLOT_NO_PACKAGE;
    candidate.operation_id[0] = 10;
    memset(candidate.target_firmware_sha256, 0xc0, 32);
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &prepared, &candidate, NULL, NULL, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_PREPARED &&
        state.bindings[0].package_present && state.bindings[0].slot == 0 &&
        state.bindings[1].present && !state.bindings[1].package_present);
}

static econtainer_slot_firmware_set_t only_firmware(uint8_t seed)
{
    econtainer_slot_firmware_set_t result = {.bootable_count = 1};
    memset(result.bootable_firmware_sha256[0], seed, 32);
    memset(result.running_firmware_sha256, seed, 32);
    return result;
}

static void test_retire_inactive_firmware(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t old_only = only_firmware(0xa0);
    uint8_t retired[32];
    memset(retired, 0xb0, sizeof(retired));
    econtainer_slots_state_t state;
    econtainer_slot_boot_decision_t decision;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t initial_sequence = state.sequence;
    const unsigned initial_writes = store.blob_write_count;

    uint8_t wrong_retired[32];
    memset(wrong_retired, 0xc0, sizeof(wrong_retired));
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, wrong_retired, &state) == ECONTAINER_SLOTS_CONFLICT);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, old_only.running_firmware_sha256,
        &state) == ECONTAINER_SLOTS_INVALID);
    econtainer_slot_firmware_set_t wrong_running = only_firmware(0xc0);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &wrong_running, retired, &state) == ECONTAINER_SLOTS_CONFLICT);
    econtainer_slot_firmware_set_t two_firmware = firmware_set(0xa0);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &two_firmware, retired, &state) == ECONTAINER_SLOTS_INVALID);
    assert(store.blob_write_count == initial_writes);

    store.fail_flash_read_slot = 2;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_IO_FAILED);
    store.fail_flash_read_slot = 0;
    store.flash[SLOT_BYTES] ^= 1;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_UNTRUSTED);
    store.flash[SLOT_BYTES] ^= 1;
    store.flash[0] ^= 1;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_UNTRUSTED);
    store.flash[0] ^= 1;
    assert(store.blob_write_count == initial_writes);

    store.fail_blob_write_before = true;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_IO_FAILED);
    store.fail_blob_write_before = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
        state.sequence == initial_sequence && state.bindings[1].present);
    store.fail_blob_write_after = true;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    store.fail_blob_write_after = false;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        initial_sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_CONFLICT);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
        state.sequence == initial_sequence + 1U && state.phase == ECONTAINER_SLOT_IDLE &&
        state.bindings[0].present && !state.bindings[1].present);
    const unsigned retired_writes = store.blob_write_count;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_OK);
    assert(state.sequence == initial_sequence + 1U &&
        store.blob_write_count == retired_writes);
    store.flash[0] ^= 1;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_UNTRUSTED);
    store.flash[0] ^= 1;
    assert(store.blob_write_count == retired_writes);
    assert(econtainer_slots_reconcile(&io, &geometry, &old_only,
        &state, &decision) == ECONTAINER_SLOTS_OK &&
        decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);

    /* Retiring B leaves a valid A-only starting point for the next A/C OTA. */
    econtainer_slot_firmware_set_t prepared = prepared_firmware(0xa0);
    econtainer_slot_operation_t candidate = {0};
    candidate.kind = ECONTAINER_SLOT_NO_PACKAGE;
    candidate.operation_id[0] = 12;
    memset(candidate.target_firmware_sha256, 0xc0, 32);
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &prepared, &candidate, NULL, NULL, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_PREPARED &&
        state.bindings[0].present && state.bindings[1].present);

    /* CONFIRMED product history may target the retired B. It must be cleared. */
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    apply_product(&store, &p2, 13, 2);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_CONFIRMED &&
        memcmp(state.operation.target_firmware_sha256, retired, 32) == 0);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_OK);
    assert(state.phase == ECONTAINER_SLOT_IDLE &&
        !state.bindings[1].present && state.operation.operation_id[0] == 0 &&
        memcmp(store.flash, p0.bytes, p0.length) == 0 &&
        store.erase_count[0] == 0 && store.erase_count[2] == 1);
}

static void test_retire_rejects_pending_and_bad_storage(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t old_only = only_firmware(0xa0);
    const econtainer_slot_firmware_set_t bootable = firmware_set(0xb0);
    uint8_t retired[32];
    memset(retired, 0xb0, sizeof(retired));
    econtainer_slots_state_t state;
    make_fixture(&p2, 12);
    const econtainer_slot_operation_t candidate = operation(2, 0xb0, &p2);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence,
        &bootable, &candidate, &state) == ECONTAINER_SLOTS_OK);
    const unsigned writes = store.blob_write_count;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_CONFLICT);
    assert(store.blob_write_count == writes);
    assert(econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
        source_read, &p2, validate_readback, &p2, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_CONFLICT);
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES] = {2};
    uint8_t target[32];
    memset(target, 0xb0, sizeof(target));
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
        target, boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_CONFLICT);
    assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
        boot_id, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_CONFLICT);
    store.trial_stopped = true;
    assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
        boot_id, trial_stopped_proof, &store, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_CONFLICT);

    seed_two_packages(&store, &p0, &p1);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    store.fail_blob_read = true;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_IO_FAILED);
    store.fail_blob_read = false;
    store.fail_lock = true;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_BUSY);
    store.fail_lock = false;
    store.blob[0] ^= 1;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_INVALID);
}

static void test_retire_commit_readback_failures(void)
{
    fake_store_t store;
    fixture_t p0, p1;
    econtainer_slots_state_t state;
    const econtainer_slot_firmware_set_t old_only = only_firmware(0xa0);
    uint8_t retired[32];
    memset(retired, 0xb0, sizeof(retired));

    seed_two_packages(&store, &p0, &p1);
    econtainer_slots_io_t io = fake_io(&store);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t sequence = state.sequence;
    store.fail_read_after_write = true;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    store.fail_read_after_write = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
        state.sequence == sequence + 1U && !state.bindings[1].present);
    const unsigned writes = store.blob_write_count;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_OK &&
        store.blob_write_count == writes);

    seed_two_packages(&store, &p0, &p1);
    io = fake_io(&store);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    store.corrupt_blob_write = true;
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &old_only, retired, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    store.corrupt_blob_write = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_INVALID);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);
}

static void test_product_uninstall_preserves_fallback(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t running = firmware_set(0xb0);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t sequence = state.sequence;
    const unsigned writes = store.blob_write_count;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES] = {23};
    uninstall_proof_t proof = {.store = &store};
    digest_fixture(&p1, proof.package_sha256);
    assert(econtainer_slots_uninstall(&io, &geometry, sequence - 1U, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_CONFLICT);
    const econtainer_slot_firmware_set_t wrong_set = firmware_set(0xa0);
    assert(econtainer_slots_uninstall(&io, &geometry, sequence, &wrong_set,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_CONFLICT);
    uint8_t wrong_digest[32] = {1};
    assert(econtainer_slots_uninstall(&io, &geometry, sequence, &running,
        operation_id, wrong_digest, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_CONFLICT);
    assert(proof.calls == 0 && store.blob_write_count == writes);
    assert(econtainer_slots_uninstall(&io, &geometry, sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_BUSY);
    assert(proof.calls == 1 && store.blob_write_count == writes);

    uint8_t original_flash[FLASH_BYTES];
    memcpy(original_flash, store.flash, sizeof original_flash);
    proof.stopped = true;
    assert(econtainer_slots_uninstall(&io, &geometry, sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_OK);
    assert(proof.calls == 2 && store.blob_write_count == writes + 1U);
    assert(state.sequence == sequence + 1U && state.phase == ECONTAINER_SLOT_CONFIRMED);
    assert(state.operation.kind == ECONTAINER_SLOT_NO_PACKAGE &&
           !state.operation.firmware_transition &&
           memcmp(state.operation.operation_id, operation_id, sizeof operation_id) == 0 &&
           memcmp(state.operation.target_firmware_sha256,
                  running.running_firmware_sha256, 32) == 0);
    assert(!state.bindings[1].package_present && state.bindings[1].present &&
           state.bindings[0].package_present && state.bindings[0].slot == 0);
    assert(econtainer_slots_uninstall(&io, &geometry, state.sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_CONFLICT);
    assert(memcmp(store.flash, original_flash, sizeof original_flash) == 0 &&
           store.erase_count[0] == 0 && store.erase_count[1] == 0);

    /* Recreate the caller after reset. The fallback still owns P0 while the
     * running firmware has no package. A new install may only reserve slot 1. */
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    assert(econtainer_slots_reconcile(&io, &geometry, &running,
        &state, &decision) == ECONTAINER_SLOTS_OK &&
        decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    const econtainer_slot_firmware_set_t fallback = firmware_set(0xa0);
    assert(econtainer_slots_reconcile(&io, &geometry, &fallback,
        &state, &decision) == ECONTAINER_SLOTS_OK &&
        decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    econtainer_slot_operation_t next = operation(24, 0xb0, &p2);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence, &running,
        &next, &state) == ECONTAINER_SLOTS_OK && state.operation.slot == 1);
    assert(memcmp(store.flash, original_flash, sizeof original_flash) == 0);
}

static void test_product_uninstall_shared_slot_and_failures(void)
{
    fake_store_t store;
    fixture_t package, replacement;
    make_fixture(&package, 10);
    make_fixture(&replacement, 12);
    memset(&store, 0, sizeof store);
    memset(store.flash, 0xff, sizeof store.flash);
    memcpy(store.flash, package.bytes, package.length);
    const econtainer_slot_binding_t bindings[2] = {
        binding(0xa0, 0, &package), binding(0xb0, 0, &package),
    };
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t running = firmware_set(0xb0);
    assert(econtainer_slots_initialize(&io, &geometry, &running,
                                       bindings) == ECONTAINER_SLOTS_OK);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES] = {31};
    uninstall_proof_t proof = {.store = &store, .stopped = true};
    digest_fixture(&package, proof.package_sha256);
    assert(econtainer_slots_uninstall(&io, &geometry, state.sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_OK);
    assert(state.bindings[0].package_present && state.bindings[0].slot == 0 &&
           !state.bindings[1].package_present && store.erase_count[0] == 0);
    econtainer_slot_operation_t next = operation(32, 0xb0, &replacement);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence, &running,
        &next, &state) == ECONTAINER_SLOTS_OK && state.operation.slot == 1);

    /* A failed or torn NVS commit never licenses erasing the old package. */
    fixture_t p0, p1;
    seed_two_packages(&store, &p0, &p1);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    digest_fixture(&p1, proof.package_sha256);
    const uint32_t sequence = state.sequence;
    const unsigned writes = store.blob_write_count;
    store.fail_blob_write_before = true;
    assert(econtainer_slots_uninstall(&io, &geometry, sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_IO_FAILED);
    store.fail_blob_write_before = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.sequence == sequence && state.bindings[1].package_present);
    assert(store.blob_write_count == writes + 1U);
    store.fail_read_after_write = true;
    assert(econtainer_slots_uninstall(&io, &geometry, sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_UNCERTAIN);
    store.fail_read_after_write = false;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.sequence == sequence + 1U && !state.bindings[1].package_present &&
           state.bindings[0].package_present);
    assert(store.erase_count[0] == 0 && store.erase_count[1] == 0);

    seed_two_packages(&store, &p0, &p1);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    digest_fixture(&p1, proof.package_sha256);
    store.flash[SLOT_BYTES] ^= 1U; /* Broken current package can be removed. */
    assert(econtainer_slots_uninstall(&io, &geometry, state.sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_OK);
    assert(!state.bindings[1].package_present && state.bindings[0].package_present);
    seed_two_packages(&store, &p0, &p1);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    digest_fixture(&p1, proof.package_sha256);
    store.flash[0] ^= 1U; /* Broken fallback cannot be declared recoverable. */
    const unsigned before = store.blob_write_count;
    assert(econtainer_slots_uninstall(&io, &geometry, state.sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_UNTRUSTED);
    assert(store.blob_write_count == before);
}

static void test_product_uninstall_rejects_pending_operation(void)
{
    fake_store_t store;
    fixture_t p0, p1, p2;
    seed_two_packages(&store, &p0, &p1);
    make_fixture(&p2, 12);
    const econtainer_slots_io_t io = fake_io(&store);
    const econtainer_slot_firmware_set_t running = firmware_set(0xb0);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    econtainer_slot_operation_t candidate = operation(40, 0xb0, &p2);
    assert(econtainer_slots_reserve(&io, &geometry, state.sequence, &running,
        &candidate, &state) == ECONTAINER_SLOTS_OK);
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES] = {41};
    uninstall_proof_t proof = {.store = &store, .stopped = true};
    digest_fixture(&p1, proof.package_sha256);
    const unsigned writes = store.blob_write_count;
    assert(econtainer_slots_uninstall(&io, &geometry, state.sequence, &running,
        operation_id, proof.package_sha256, instance_stopped_proof, &proof,
        &state) == ECONTAINER_SLOTS_BUSY);
    assert(proof.calls == 0 && store.blob_write_count == writes &&
           store.erase_count[0] == 0 && store.erase_count[1] == 0);
}

int main(void)
{
    test_four_packages();
    test_firmware_set_and_capacity_guards();
    test_commit_readback_and_torn_flash();
    test_unknown_recovery_and_record_damage();
    test_readback_boundary_and_confirm();
    test_damaged_trial_same_boot_stop_proof();
    test_candidate_read_failure_keeps_confirmed();
    test_guards();
    test_firmware_no_package_and_rollback();
    test_firmware_write_before_boot();
    test_rebind_after_other_firmware_product_update();
    test_retire_inactive_firmware();
    test_retire_rejects_pending_and_bad_storage();
    test_retire_commit_readback_failures();
    test_product_uninstall_preserves_fallback();
    test_product_uninstall_shared_slot_and_failures();
    test_product_uninstall_rejects_pending_operation();
    puts("slots: protected P0/P1/P2/P3, durable commit, torn Flash and restart checks passed");
    return 0;
}
