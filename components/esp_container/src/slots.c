#include "esp_container_slots.h"
#include "slots_internal.h"

#include "package_crypto.h"

#include <limits.h>
#include <string.h>

enum {
    SLOT_HEADER_BYTES = 12,
    SLOT_BINDING_BYTES = 80,
    SLOT_OPERATION_BYTES = 112,
    SLOT_CRC_OFFSET = ECONTAINER_SLOT_BLOB_BYTES - 4,
    SLOT_IO_BYTES = 256,
};

_Static_assert(SLOT_HEADER_BYTES + ECONTAINER_SLOT_BINDING_COUNT * SLOT_BINDING_BYTES +
               SLOT_OPERATION_BYTES + 4 == ECONTAINER_SLOT_BLOB_BYTES,
               "slot blob layout must remain exact");

typedef struct {
    const econtainer_slots_io_t *io;
    uint32_t offset_bytes;
    uint32_t size_bytes;
} slot_reader_t;

static bool all_zero(const uint8_t *bytes, size_t count)
{
    uint8_t combined = 0;
    for (size_t index = 0; index < count; ++index) {
        combined |= bytes[index];
    }
    return combined == 0;
}

static void store_u32(uint8_t *destination, uint32_t value)
{
    for (unsigned index = 0; index < 4U; ++index) {
        destination[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint32_t load_u32(const uint8_t *source)
{
    uint32_t value = 0;
    for (unsigned index = 0; index < 4U; ++index) {
        value |= (uint32_t)source[index] << (index * 8U);
    }
    return value;
}

static uint32_t blob_crc32(const uint8_t *bytes, size_t count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < count; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

bool econtainer_slots_geometry_valid(const econtainer_slots_geometry_t *geometry)
{
    if (geometry == NULL || geometry->partition_size_bytes == 0U ||
        geometry->erase_unit_bytes == 0U || geometry->write_unit_bytes == 0U ||
        geometry->write_unit_bytes > SLOT_IO_BYTES ||
        SLOT_IO_BYTES % geometry->write_unit_bytes != 0U ||
        geometry->erase_unit_bytes % geometry->write_unit_bytes != 0U ||
        (geometry->erase_unit_bytes & (geometry->erase_unit_bytes - 1U)) != 0U ||
        (geometry->write_unit_bytes & (geometry->write_unit_bytes - 1U)) != 0U ||
        (uint64_t)geometry->partition_offset_bytes + geometry->partition_size_bytes > UINT32_MAX) {
        return false;
    }
    const uint64_t partition_end =
        (uint64_t)geometry->partition_offset_bytes + geometry->partition_size_bytes;
    for (unsigned index = 0; index < ECONTAINER_SLOT_COUNT; ++index) {
        const econtainer_slot_region_t *slot = &geometry->slots[index];
        if (slot->size_bytes == 0U || slot->offset_bytes < geometry->partition_offset_bytes ||
            slot->offset_bytes % geometry->erase_unit_bytes != 0U ||
            slot->size_bytes % geometry->erase_unit_bytes != 0U ||
            (uint64_t)slot->offset_bytes + slot->size_bytes > partition_end) {
            return false;
        }
        for (unsigned other = 0; other < index; ++other) {
            const econtainer_slot_region_t *previous = &geometry->slots[other];
            if ((uint64_t)slot->offset_bytes <
                    (uint64_t)previous->offset_bytes + previous->size_bytes &&
                (uint64_t)previous->offset_bytes <
                    (uint64_t)slot->offset_bytes + slot->size_bytes) {
                return false;
            }
        }
    }
    return true;
}

static bool io_valid(const econtainer_slots_io_t *io)
{
    return io != NULL && io->lock != NULL && io->unlock != NULL &&
           io->read_blob != NULL && io->write_blob != NULL &&
           io->flash_read != NULL && io->flash_erase != NULL &&
           io->flash_write != NULL;
}

static bool binding_valid(const econtainer_slot_binding_t *binding,
                          const econtainer_slots_geometry_t *geometry)
{
    if (!binding->present) {
        return !binding->package_present && binding->slot == 0U &&
               all_zero(binding->firmware_sha256, 32) &&
               all_zero(binding->package_sha256, 32) &&
               binding->package_size_bytes == 0U && binding->guest_abi_version == 0U &&
               binding->data_schema_version == 0U;
    }
    if (all_zero(binding->firmware_sha256, 32)) {
        return false;
    }
    if (!binding->package_present) {
        return binding->slot == 0U && all_zero(binding->package_sha256, 32) &&
               binding->package_size_bytes == 0U && binding->guest_abi_version == 0U &&
               binding->data_schema_version == 0U;
    }
    return binding->slot < ECONTAINER_SLOT_COUNT &&
           !all_zero(binding->package_sha256, 32) &&
           binding->package_size_bytes > 0U &&
           binding->package_size_bytes <= geometry->slots[binding->slot].size_bytes &&
           binding->guest_abi_version > 0U && binding->data_schema_version > 0U;
}

static bool operation_valid(const econtainer_slots_state_t *state,
                            const econtainer_slots_geometry_t *geometry)
{
    const econtainer_slot_operation_t *operation = &state->operation;
    if (state->phase == ECONTAINER_SLOT_IDLE) {
        const econtainer_slot_operation_t empty = {0};
        return memcmp(operation, &empty, sizeof(empty)) == 0;
    }
    if (state->phase < ECONTAINER_SLOT_WRITING || state->phase > ECONTAINER_SLOT_ABORTED ||
        all_zero(operation->operation_id, sizeof(operation->operation_id)) ||
        all_zero(operation->target_firmware_sha256, 32) ||
        operation->kind > ECONTAINER_SLOT_NO_PACKAGE ||
        (operation->kind != ECONTAINER_SLOT_PACKAGE_WRITE &&
         !operation->firmware_transition &&
         !(operation->kind == ECONTAINER_SLOT_NO_PACKAGE &&
           state->phase == ECONTAINER_SLOT_CONFIRMED))) {
        return false;
    }
    const bool has_package = operation->kind != ECONTAINER_SLOT_NO_PACKAGE;
    if (has_package) {
        if (all_zero(operation->package_sha256, 32) ||
            operation->slot >= ECONTAINER_SLOT_COUNT ||
            operation->package_size_bytes == 0U ||
            operation->package_size_bytes > geometry->slots[operation->slot].size_bytes ||
            operation->guest_abi_version == 0U || operation->data_schema_version == 0U) {
            return false;
        }
    } else if (operation->slot != 0U ||
               !all_zero(operation->package_sha256, 32) ||
               operation->package_size_bytes != 0U ||
               operation->guest_abi_version != 0U ||
               operation->data_schema_version != 0U) {
        return false;
    }
    if (state->phase == ECONTAINER_SLOT_WRITING &&
        operation->kind != ECONTAINER_SLOT_PACKAGE_WRITE) return false;
    const econtainer_slot_binding_t *target = NULL;
    bool reused_confirmed = false;
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &state->bindings[index];
        if (binding->present && memcmp(binding->firmware_sha256,
                                       operation->target_firmware_sha256, 32) == 0) {
            target = binding;
        }
        if (operation->kind == ECONTAINER_SLOT_PACKAGE_WRITE &&
            state->phase >= ECONTAINER_SLOT_WRITING &&
            state->phase <= ECONTAINER_SLOT_HEALTH_VERIFIED &&
            binding->package_present && binding->slot == operation->slot) {
            return false;
        }
        if (binding->present && binding->package_present && has_package &&
            binding->slot == operation->slot &&
            binding->package_size_bytes == operation->package_size_bytes &&
            binding->guest_abi_version == operation->guest_abi_version &&
            binding->data_schema_version == operation->data_schema_version &&
            memcmp(binding->package_sha256, operation->package_sha256, 32) == 0) {
            reused_confirmed = true;
        }
    }
    if (target == NULL ||
        (operation->firmware_transition &&
         state->phase != ECONTAINER_SLOT_CONFIRMED && target->package_present) ||
        (operation->kind == ECONTAINER_SLOT_PACKAGE_REUSE && !reused_confirmed)) return false;
    if (state->phase == ECONTAINER_SLOT_WRITING || state->phase == ECONTAINER_SLOT_PREPARED) {
        return all_zero(operation->trial_boot_id, sizeof(operation->trial_boot_id));
    }
    if (state->phase == ECONTAINER_SLOT_CONFIRMED &&
        operation->kind == ECONTAINER_SLOT_NO_PACKAGE &&
        !operation->firmware_transition) {
        return all_zero(operation->trial_boot_id, sizeof(operation->trial_boot_id));
    }
    if (state->phase == ECONTAINER_SLOT_TRIAL_STARTED ||
        state->phase == ECONTAINER_SLOT_HEALTH_VERIFIED ||
        state->phase == ECONTAINER_SLOT_CONFIRMED) {
        return !all_zero(operation->trial_boot_id, sizeof(operation->trial_boot_id));
    }
    return true;
}

static bool state_valid(const econtainer_slots_state_t *state,
                        const econtainer_slots_geometry_t *geometry)
{
    if (state->sequence == 0U ||
        !binding_valid(&state->bindings[0], geometry) ||
        !binding_valid(&state->bindings[1], geometry) ||
        (!state->bindings[0].present && !state->bindings[1].present)) {
        return false;
    }
    const econtainer_slot_binding_t *first = &state->bindings[0];
    const econtainer_slot_binding_t *second = &state->bindings[1];
    if (first->present && second->present &&
        memcmp(first->firmware_sha256, second->firmware_sha256, 32) == 0) {
        return false;
    }
    if (first->package_present && second->package_present && first->slot == second->slot &&
        (first->package_size_bytes != second->package_size_bytes ||
         memcmp(first->package_sha256, second->package_sha256, 32) != 0 ||
         first->guest_abi_version != second->guest_abi_version ||
         first->data_schema_version != second->data_schema_version)) {
        return false;
    }
    if (!operation_valid(state, geometry)) {
        return false;
    }
    if (state->phase == ECONTAINER_SLOT_CONFIRMED) {
        bool matched = false;
        for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
            const econtainer_slot_binding_t *binding = &state->bindings[index];
            if (binding->present &&
                memcmp(binding->firmware_sha256,
                       state->operation.target_firmware_sha256, 32) == 0 &&
                state->operation.kind == ECONTAINER_SLOT_NO_PACKAGE &&
                !binding->package_present) {
                matched = true;
            } else if (binding->present && binding->package_present &&
                memcmp(binding->firmware_sha256,
                       state->operation.target_firmware_sha256, 32) == 0 &&
                binding->slot == state->operation.slot &&
                binding->package_size_bytes == state->operation.package_size_bytes &&
                memcmp(binding->package_sha256, state->operation.package_sha256, 32) == 0 &&
                binding->guest_abi_version == state->operation.guest_abi_version &&
                binding->data_schema_version == state->operation.data_schema_version) {
                matched = true;
            }
        }
        return matched;
    }
    return true;
}

static void encode_state(const econtainer_slots_state_t *state,
                         uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES])
{
    memset(blob, 0, ECONTAINER_SLOT_BLOB_BYTES);
    memcpy(blob, "ECS2", 4);
    store_u32(blob + 4, 2U);
    store_u32(blob + 8, state->sequence);
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        uint8_t *destination = blob + SLOT_HEADER_BYTES + index * SLOT_BINDING_BYTES;
        const econtainer_slot_binding_t *binding = &state->bindings[index];
        destination[0] = binding->present ? 1U : 0U;
        destination[1] = binding->package_present ? 1U : 0U;
        destination[2] = binding->slot;
        memcpy(destination + 4, binding->firmware_sha256, 32);
        memcpy(destination + 36, binding->package_sha256, 32);
        store_u32(destination + 68, binding->package_size_bytes);
        store_u32(destination + 72, binding->guest_abi_version);
        store_u32(destination + 76, binding->data_schema_version);
    }
    uint8_t *destination = blob + SLOT_HEADER_BYTES +
                           ECONTAINER_SLOT_BINDING_COUNT * SLOT_BINDING_BYTES;
    const econtainer_slot_operation_t *operation = &state->operation;
    destination[0] = (uint8_t)state->phase;
    destination[1] = operation->slot;
    destination[2] = (uint8_t)operation->kind;
    destination[3] = operation->firmware_transition ? 1U : 0U;
    memcpy(destination + 4, operation->operation_id, sizeof(operation->operation_id));
    memcpy(destination + 20, operation->target_firmware_sha256, 32);
    memcpy(destination + 52, operation->package_sha256, 32);
    store_u32(destination + 84, operation->package_size_bytes);
    store_u32(destination + 88, operation->guest_abi_version);
    store_u32(destination + 92, operation->data_schema_version);
    memcpy(destination + 96, operation->trial_boot_id, sizeof(operation->trial_boot_id));
    store_u32(blob + SLOT_CRC_OFFSET, blob_crc32(blob, SLOT_CRC_OFFSET));
}

static bool decode_state(const uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES],
                         const econtainer_slots_geometry_t *geometry,
                         econtainer_slots_state_t *state)
{
    if (memcmp(blob, "ECS2", 4) != 0 || load_u32(blob + 4) != 2U ||
        load_u32(blob + SLOT_CRC_OFFSET) != blob_crc32(blob, SLOT_CRC_OFFSET)) {
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->sequence = load_u32(blob + 8);
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const uint8_t *source = blob + SLOT_HEADER_BYTES + index * SLOT_BINDING_BYTES;
        econtainer_slot_binding_t *binding = &state->bindings[index];
        if (source[0] > 1U || source[1] > 1U || source[3] != 0U) {
            return false;
        }
        binding->present = source[0] != 0U;
        binding->package_present = source[1] != 0U;
        binding->slot = source[2];
        memcpy(binding->firmware_sha256, source + 4, 32);
        memcpy(binding->package_sha256, source + 36, 32);
        binding->package_size_bytes = load_u32(source + 68);
        binding->guest_abi_version = load_u32(source + 72);
        binding->data_schema_version = load_u32(source + 76);
    }
    const uint8_t *source = blob + SLOT_HEADER_BYTES +
                            ECONTAINER_SLOT_BINDING_COUNT * SLOT_BINDING_BYTES;
    if (source[2] > ECONTAINER_SLOT_NO_PACKAGE || source[3] > 1U) {
        return false;
    }
    econtainer_slot_operation_t *operation = &state->operation;
    state->phase = (econtainer_slot_phase_t)source[0];
    operation->slot = source[1];
    operation->kind = (econtainer_slot_operation_kind_t)source[2];
    operation->firmware_transition = source[3] != 0U;
    memcpy(operation->operation_id, source + 4, sizeof(operation->operation_id));
    memcpy(operation->target_firmware_sha256, source + 20, 32);
    memcpy(operation->package_sha256, source + 52, 32);
    operation->package_size_bytes = load_u32(source + 84);
    operation->guest_abi_version = load_u32(source + 88);
    operation->data_schema_version = load_u32(source + 92);
    memcpy(operation->trial_boot_id, source + 96, sizeof(operation->trial_boot_id));
    return state_valid(state, geometry);
}

static econtainer_slots_result_t load_locked(const econtainer_slots_io_t *io,
                                              const econtainer_slots_geometry_t *geometry,
                                              econtainer_slots_state_t *state)
{
    uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES];
    const econtainer_slot_blob_result_t result = io->read_blob(io->context, blob);
    if (result == ECONTAINER_SLOT_BLOB_NOT_FOUND) {
        return ECONTAINER_SLOTS_EMPTY;
    }
    if (result != ECONTAINER_SLOT_BLOB_FOUND) {
        return ECONTAINER_SLOTS_IO_FAILED;
    }
    return decode_state(blob, geometry, state) ? ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_INVALID;
}

static econtainer_slots_result_t persist_locked(const econtainer_slots_io_t *io,
                                                 const econtainer_slots_state_t *before,
                                                 const econtainer_slots_state_t *after,
                                                 econtainer_slots_state_t *state)
{
    uint8_t desired[ECONTAINER_SLOT_BLOB_BYTES];
    uint8_t observed[ECONTAINER_SLOT_BLOB_BYTES];
    uint8_t old[ECONTAINER_SLOT_BLOB_BYTES];
    encode_state(after, desired);
    if (before != NULL) {
        encode_state(before, old);
    }
    const bool committed = io->write_blob(io->context, desired);
    if (io->read_blob(io->context, observed) != ECONTAINER_SLOT_BLOB_FOUND) {
        return ECONTAINER_SLOTS_UNCERTAIN;
    }
    if (memcmp(observed, desired, sizeof(desired)) == 0) {
        if (!committed) {
            return ECONTAINER_SLOTS_UNCERTAIN;
        }
        if (state != NULL) {
            *state = *after;
        }
        return ECONTAINER_SLOTS_OK;
    }
    if (before != NULL && memcmp(observed, old, sizeof(old)) == 0) {
        return ECONTAINER_SLOTS_IO_FAILED;
    }
    return ECONTAINER_SLOTS_UNCERTAIN;
}

static econtainer_slots_result_t commit_next(const econtainer_slots_io_t *io,
                                              const econtainer_slots_geometry_t *geometry,
                                              const econtainer_slots_state_t *before,
                                              econtainer_slots_state_t *next,
                                              econtainer_slots_state_t *state)
{
    if (before->sequence == UINT32_MAX) {
        return ECONTAINER_SLOTS_INVALID;
    }
    next->sequence = before->sequence + 1U;
    if (!state_valid(next, geometry)) {
        return ECONTAINER_SLOTS_INVALID;
    }
    return persist_locked(io, before, next, state);
}

static bool flash_relative_read(void *context, size_t relative_offset_bytes,
                                uint8_t *destination, size_t size_bytes)
{
    const slot_reader_t *reader = context;
    if (relative_offset_bytes > reader->size_bytes ||
        size_bytes > (size_t)reader->size_bytes - relative_offset_bytes ||
        relative_offset_bytes > UINT32_MAX - reader->offset_bytes) {
        return false;
    }
    return reader->io->flash_read(reader->io->context,
                                  reader->offset_bytes + (uint32_t)relative_offset_bytes,
                                  destination, size_bytes);
}

static econtainer_slots_result_t hash_flash(const econtainer_slots_io_t *io,
                                             const econtainer_slots_geometry_t *geometry,
                                             uint8_t slot, uint32_t size_bytes,
                                             const uint8_t expected_sha256[32])
{
    if (slot >= ECONTAINER_SLOT_COUNT || size_bytes == 0U ||
        size_bytes > geometry->slots[slot].size_bytes) {
        return ECONTAINER_SLOTS_INVALID;
    }
    econtainer_package_digest_t digest;
    if (!econtainer_package_digest_start(&digest)) {
        return ECONTAINER_SLOTS_IO_FAILED;
    }
    uint8_t buffer[SLOT_IO_BYTES];
    uint32_t offset = 0;
    slot_reader_t reader = {io, geometry->slots[slot].offset_bytes, size_bytes};
    while (offset < size_bytes) {
        const size_t count = (size_bytes - offset) > SLOT_IO_BYTES ?
                             SLOT_IO_BYTES : (size_t)(size_bytes - offset);
        if (!flash_relative_read(&reader, offset, buffer, count) ||
            !econtainer_package_digest_update(&digest, buffer, count)) {
            econtainer_package_digest_abort(&digest);
            return ECONTAINER_SLOTS_IO_FAILED;
        }
        offset += (uint32_t)count;
    }
    uint8_t actual[32];
    if (!econtainer_package_digest_finish(&digest, actual)) {
        return ECONTAINER_SLOTS_IO_FAILED;
    }
    return memcmp(actual, expected_sha256, 32) == 0 ?
           ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_UNTRUSTED;
}

typedef struct {
    const econtainer_slots_io_t *io;
    const econtainer_slots_geometry_t *geometry;
    const econtainer_slot_binding_t *bindings;
} reference_reader_t;

static bool read_reference(void *context, unsigned binding_index,
                           size_t offset_bytes, uint8_t *destination, size_t size_bytes)
{
    const reference_reader_t *reader = context;
    if (binding_index >= ECONTAINER_SLOT_BINDING_COUNT || destination == NULL ||
        size_bytes == 0U || size_bytes > 512U) return false;
    const econtainer_slot_binding_t *binding = &reader->bindings[binding_index];
    if (!binding->present || !binding->package_present ||
        binding->slot >= ECONTAINER_SLOT_COUNT) return false;
    const slot_reader_t package = {reader->io,
        reader->geometry->slots[binding->slot].offset_bytes,
        binding->package_size_bytes};
    return flash_relative_read((void *)&package, offset_bytes, destination, size_bytes);
}

static econtainer_slots_result_t check_references(const econtainer_slots_io_t *io,
                                                   const econtainer_slots_geometry_t *geometry,
                                                   const econtainer_slots_state_t *state,
                                                   bool check_candidate)
{
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &state->bindings[index];
        if (binding->package_present) {
            const econtainer_slots_result_t result = hash_flash(io, geometry,
                binding->slot, binding->package_size_bytes, binding->package_sha256);
            if (result != ECONTAINER_SLOTS_OK) {
                return result;
            }
        }
    }
    if (check_candidate && state->operation.kind != ECONTAINER_SLOT_NO_PACKAGE &&
        state->phase >= ECONTAINER_SLOT_PREPARED &&
        state->phase <= ECONTAINER_SLOT_HEALTH_VERIFIED) {
        return hash_flash(io, geometry, state->operation.slot,
                          state->operation.package_size_bytes,
                          state->operation.package_sha256);
    }
    return ECONTAINER_SLOTS_OK;
}

static int binding_for_firmware(const econtainer_slots_state_t *state,
                                const uint8_t firmware_sha256[32])
{
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (state->bindings[index].present &&
            memcmp(state->bindings[index].firmware_sha256, firmware_sha256, 32) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static bool firmware_set_valid(const econtainer_slot_firmware_set_t *firmware_set)
{
    if (firmware_set == NULL || firmware_set->bootable_count == 0U ||
        firmware_set->bootable_count > ECONTAINER_SLOT_BINDING_COUNT ||
        all_zero(firmware_set->running_firmware_sha256, 32)) {
        return false;
    }
    bool running_found = false;
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const uint8_t *digest = firmware_set->bootable_firmware_sha256[index];
        if (index >= firmware_set->bootable_count) {
            if (!all_zero(digest, 32)) {
                return false;
            }
            continue;
        }
        if (all_zero(digest, 32)) {
            return false;
        }
        for (unsigned previous = 0; previous < index; ++previous) {
            if (memcmp(digest, firmware_set->bootable_firmware_sha256[previous], 32) == 0) {
                return false;
            }
        }
        running_found |= memcmp(digest, firmware_set->running_firmware_sha256, 32) == 0;
    }
    return running_found;
}

static bool firmware_set_matches(const econtainer_slots_state_t *state,
                                 const econtainer_slot_firmware_set_t *firmware_set)
{
    unsigned binding_count = 0;
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (state->bindings[index].present) {
            ++binding_count;
            bool found = false;
            for (unsigned member = 0; member < firmware_set->bootable_count; ++member) {
                found |= memcmp(state->bindings[index].firmware_sha256,
                                firmware_set->bootable_firmware_sha256[member], 32) == 0;
            }
            if (!found) {
                return false;
            }
        }
    }
    return binding_count == firmware_set->bootable_count;
}

static econtainer_slots_result_t begin_locked(const econtainer_slots_io_t *io,
                                               const econtainer_slots_geometry_t *geometry,
                                               uint32_t expected_sequence,
                                               econtainer_slots_state_t *current)
{
    const econtainer_slots_result_t result = load_locked(io, geometry, current);
    if (result != ECONTAINER_SLOTS_OK) {
        return result;
    }
    return current->sequence == expected_sequence ? ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
}

econtainer_slots_result_t econtainer_slots_with_selected_package(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_selection_request_t *request,
    econtainer_slot_selected_fn selected_fn, void *context)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        request == NULL || !firmware_set_valid(&request->firmware_set) ||
        selected_fn == NULL ||
        (request->selection != ECONTAINER_SLOT_SELECT_CONFIRMED &&
         request->selection != ECONTAINER_SLOT_SELECT_TRIAL)) {
        return ECONTAINER_SLOTS_INVALID;
    }
    const bool trial = request->selection == ECONTAINER_SLOT_SELECT_TRIAL;
    if (trial ? (all_zero(request->operation_id, sizeof(request->operation_id)) ||
                 all_zero(request->boot_id, sizeof(request->boot_id)))
              : (!all_zero(request->operation_id, sizeof(request->operation_id)) ||
                 !all_zero(request->boot_id, sizeof(request->boot_id)))) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) return ECONTAINER_SLOTS_BUSY;
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry,
                                                     request->expected_sequence, &current);
    econtainer_slot_package_t package = {0};
    if (result == ECONTAINER_SLOTS_OK &&
        !firmware_set_matches(&current, &request->firmware_set)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK &&
        current.operation.firmware_transition &&
        current.phase == ECONTAINER_SLOT_CONFIRMED &&
        memcmp(current.operation.target_firmware_sha256,
               request->firmware_set.running_firmware_sha256, 32) != 0) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK && !trial &&
        current.phase >= ECONTAINER_SLOT_WRITING &&
        current.phase <= ECONTAINER_SLOT_HEALTH_VERIFIED &&
        memcmp(current.operation.target_firmware_sha256,
               request->firmware_set.running_firmware_sha256, 32) == 0) {
        const int running_index = binding_for_firmware(&current,
            request->firmware_set.running_firmware_sha256);
        if (running_index < 0 || !current.bindings[running_index].package_present) {
            result = ECONTAINER_SLOTS_CONFLICT;
        }
    }
    if (result == ECONTAINER_SLOTS_OK && trial) {
        const econtainer_slot_operation_t *operation = &current.operation;
        if (current.phase != ECONTAINER_SLOT_TRIAL_STARTED ||
            memcmp(operation->target_firmware_sha256,
                   request->firmware_set.running_firmware_sha256, 32) != 0 ||
            memcmp(operation->operation_id, request->operation_id,
                   sizeof(request->operation_id)) != 0 ||
            memcmp(operation->trial_boot_id, request->boot_id, sizeof(request->boot_id)) != 0) {
            result = ECONTAINER_SLOTS_CONFLICT;
        } else if (operation->kind == ECONTAINER_SLOT_NO_PACKAGE) {
            result = ECONTAINER_SLOTS_EMPTY;
        } else {
            package.slot = operation->slot;
            memcpy(package.package_sha256, operation->package_sha256, 32);
            package.package_size_bytes = operation->package_size_bytes;
            package.guest_abi_version = operation->guest_abi_version;
            package.data_schema_version = operation->data_schema_version;
        }
    } else if (result == ECONTAINER_SLOTS_OK) {
        const int index = binding_for_firmware(&current,
            request->firmware_set.running_firmware_sha256);
        if (index < 0) {
            result = ECONTAINER_SLOTS_CONFLICT;
        } else {
            const econtainer_slot_binding_t *binding = &current.bindings[index];
            if (!binding->package_present) {
                result = ECONTAINER_SLOTS_EMPTY;
            } else {
                package.slot = binding->slot;
                memcpy(package.package_sha256, binding->package_sha256, 32);
                package.package_size_bytes = binding->package_size_bytes;
                package.guest_abi_version = binding->guest_abi_version;
                package.data_schema_version = binding->data_schema_version;
            }
        }
    }
    /* Keep both bootable firmware references protected. An invalid pending
     * candidate must not prevent recovery of the separately verified confirmed
     * package. The selected callback verifies the selected package completely. */
    if (result == ECONTAINER_SLOTS_OK) result = check_references(io, geometry, &current, false);
    if (result == ECONTAINER_SLOTS_OK) {
        const reference_reader_t reader = {io, geometry, current.bindings};
        const econtainer_slot_references_t references = {
            current.bindings, read_reference, (void *)&reader};
        result = selected_fn(context, &package, geometry->slots[package.slot].offset_bytes,
                             &references);
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_initialize(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set,
    const econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT])
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        !firmware_set_valid(firmware_set) || bindings == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = load_locked(io, geometry, &current);
    if (result == ECONTAINER_SLOTS_EMPTY) {
        econtainer_slots_state_t initial = {0};
        initial.sequence = 1U;
        memcpy(initial.bindings, bindings, sizeof(initial.bindings));
        if (!state_valid(&initial, geometry) ||
            !firmware_set_matches(&initial, firmware_set)) {
            result = ECONTAINER_SLOTS_INVALID;
        } else {
            result = check_references(io, geometry, &initial, false);
            if (result == ECONTAINER_SLOTS_OK) {
                result = persist_locked(io, NULL, &initial, NULL);
            }
        }
    } else if (result == ECONTAINER_SLOTS_OK) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_load(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) || state == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    memset(state, 0, sizeof(*state));
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    const econtainer_slots_result_t result = load_locked(io, geometry, state);
    io->unlock(io->context);
    if (result != ECONTAINER_SLOTS_OK) {
        memset(state, 0, sizeof(*state));
    }
    return result;
}

econtainer_slots_result_t econtainer_slots_stage_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const econtainer_slot_firmware_set_t *prepared_set,
    const econtainer_slot_operation_t *operation,
    econtainer_slot_validate_binding_fn validate_fn, void *validate_context,
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        !firmware_set_valid(prepared_set) || prepared_set->bootable_count != 2U ||
        operation == NULL || state == NULL ||
        operation->kind > ECONTAINER_SLOT_NO_PACKAGE ||
        operation->firmware_transition ||
        all_zero(operation->operation_id, sizeof(operation->operation_id)) ||
        !all_zero(operation->trial_boot_id, sizeof(operation->trial_boot_id)) ||
        (operation->kind == ECONTAINER_SLOT_PACKAGE_REUSE && validate_fn == NULL)) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) return ECONTAINER_SLOTS_BUSY;
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    const int running_index = result == ECONTAINER_SLOTS_OK ?
        binding_for_firmware(&current, prepared_set->running_firmware_sha256) : -1;
    if (result == ECONTAINER_SLOTS_OK &&
        (running_index < 0 ||
         (current.phase != ECONTAINER_SLOT_IDLE &&
          current.phase != ECONTAINER_SLOT_CONFIRMED &&
          current.phase != ECONTAINER_SLOT_ABORTED) ||
         (current.phase == ECONTAINER_SLOT_CONFIRMED &&
          current.operation.firmware_transition &&
          memcmp(current.operation.target_firmware_sha256,
                 prepared_set->running_firmware_sha256, 32) != 0))) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK && current.phase != ECONTAINER_SLOT_IDLE &&
        memcmp(current.operation.operation_id, operation->operation_id,
               sizeof(operation->operation_id)) == 0) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    econtainer_slots_state_t next = {0};
    if (result == ECONTAINER_SLOTS_OK) {
        const uint8_t *candidate_sha256 = NULL;
        for (unsigned index = 0; index < prepared_set->bootable_count; ++index) {
            const uint8_t *digest = prepared_set->bootable_firmware_sha256[index];
            if (memcmp(digest, prepared_set->running_firmware_sha256, 32) != 0) {
                candidate_sha256 = digest;
            }
        }
        if (candidate_sha256 == NULL ||
            memcmp(operation->target_firmware_sha256, candidate_sha256, 32) != 0 ||
            binding_for_firmware(&current, candidate_sha256) >= 0) {
            result = ECONTAINER_SLOTS_CONFLICT;
        } else {
            next = current;
            next.bindings[1 - running_index] = (econtainer_slot_binding_t){0};
            next.bindings[1 - running_index].present = true;
            memcpy(next.bindings[1 - running_index].firmware_sha256,
                   candidate_sha256, 32);
            next.operation = *operation;
            next.operation.firmware_transition = true;
            next.phase = operation->kind == ECONTAINER_SLOT_PACKAGE_WRITE ?
                         ECONTAINER_SLOT_WRITING : ECONTAINER_SLOT_PREPARED;
            if (!firmware_set_matches(&next, prepared_set)) {
                result = ECONTAINER_SLOTS_CONFLICT;
            }
        }
    }
    if (result == ECONTAINER_SLOTS_OK &&
        operation->kind == ECONTAINER_SLOT_PACKAGE_WRITE) {
        if (operation->slot != 0U || operation->package_size_bytes == 0U ||
            all_zero(operation->package_sha256, 32) ||
            operation->guest_abi_version == 0U ||
            operation->data_schema_version == 0U) {
            result = ECONTAINER_SLOTS_INVALID;
        } else {
            unsigned selected = ECONTAINER_SLOT_COUNT;
            for (unsigned index = 0; index < ECONTAINER_SLOT_COUNT; ++index) {
                const econtainer_slot_binding_t *running = &current.bindings[running_index];
                if ((!running->package_present || running->slot != index) &&
                    operation->package_size_bytes <= geometry->slots[index].size_bytes) {
                    selected = index;
                    break;
                }
            }
            if (selected == ECONTAINER_SLOT_COUNT) {
                result = ECONTAINER_SLOTS_NO_SPACE;
            } else {
                next.operation.slot = (uint8_t)selected;
            }
        }
    }
    if (result == ECONTAINER_SLOTS_OK &&
        operation->kind == ECONTAINER_SLOT_PACKAGE_REUSE) {
        const econtainer_slot_binding_t *running = &current.bindings[running_index];
        if (!running->package_present || operation->slot != running->slot ||
            operation->package_size_bytes != running->package_size_bytes ||
            operation->guest_abi_version != running->guest_abi_version ||
            operation->data_schema_version != running->data_schema_version ||
            memcmp(operation->package_sha256, running->package_sha256, 32) != 0) {
            result = ECONTAINER_SLOTS_CONFLICT;
        }
    }
    if (result == ECONTAINER_SLOTS_OK && !state_valid(&next, geometry)) {
        result = ECONTAINER_SLOTS_INVALID;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        /* The signed replacement image has retired the old inactive firmware.
         * Its package is no longer a protected reference. */
        result = check_references(io, geometry, &next, false);
    }
    if (result == ECONTAINER_SLOTS_OK &&
        operation->kind == ECONTAINER_SLOT_PACKAGE_REUSE) {
        econtainer_slot_binding_t proposed = current.bindings[running_index];
        memcpy(proposed.firmware_sha256, operation->target_firmware_sha256, 32);
        const slot_reader_t reader = {io,
            geometry->slots[operation->slot].offset_bytes,
            operation->package_size_bytes};
        const reference_reader_t references_reader = {io, geometry, next.bindings};
        const econtainer_slot_references_t references = {
            next.bindings, read_reference, (void *)&references_reader};
        const econtainer_slot_validation_result_t validation = validate_fn(
            validate_context, &proposed, flash_relative_read, (void *)&reader,
            operation->package_size_bytes, &references);
        result = validation == ECONTAINER_SLOT_VALIDATION_OK ? ECONTAINER_SLOTS_OK :
                 validation == ECONTAINER_SLOT_VALIDATION_IO_FAILED ?
                 ECONTAINER_SLOTS_IO_FAILED : ECONTAINER_SLOTS_UNTRUSTED;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = commit_next(io, geometry, &current, &next, state);
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_retire_inactive_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const econtainer_slot_firmware_set_t *actual_set,
    const uint8_t retired_firmware_sha256[32],
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        !firmware_set_valid(actual_set) || actual_set->bootable_count != 1U ||
        retired_firmware_sha256 == NULL || all_zero(retired_firmware_sha256, 32) ||
        memcmp(retired_firmware_sha256, actual_set->running_firmware_sha256, 32) == 0 ||
        state == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) return ECONTAINER_SLOTS_BUSY;
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    const int running_index = result == ECONTAINER_SLOTS_OK ?
        binding_for_firmware(&current, actual_set->running_firmware_sha256) : -1;
    if (result == ECONTAINER_SLOTS_OK &&
        (running_index < 0 ||
         (current.phase != ECONTAINER_SLOT_IDLE &&
          current.phase != ECONTAINER_SLOT_CONFIRMED))) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        const econtainer_slot_binding_t *inactive = &current.bindings[1 - running_index];
        if (inactive->present &&
            memcmp(inactive->firmware_sha256, retired_firmware_sha256, 32) != 0) {
            result = ECONTAINER_SLOTS_CONFLICT;
        } else if (!inactive->present &&
                   (current.phase != ECONTAINER_SLOT_IDLE ||
                    !firmware_set_matches(&current, actual_set))) {
            result = ECONTAINER_SLOTS_CONFLICT;
        }
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, false);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        if (!current.bindings[1 - running_index].present) {
            *state = current;
        } else {
            econtainer_slots_state_t next = current;
            next.bindings[1 - running_index] = (econtainer_slot_binding_t){0};
            next.phase = ECONTAINER_SLOT_IDLE;
            next.operation = (econtainer_slot_operation_t){0};
            result = firmware_set_matches(&next, actual_set) ?
                     commit_next(io, geometry, &current, &next, state) :
                     ECONTAINER_SLOTS_CONFLICT;
        }
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_drop_aborted_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence,
    const econtainer_slot_firmware_set_t *actual_set,
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        !firmware_set_valid(actual_set) || actual_set->bootable_count != 1U ||
        state == NULL) return ECONTAINER_SLOTS_INVALID;
    if (!io->lock(io->context)) return ECONTAINER_SLOTS_BUSY;
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    const int running_index = result == ECONTAINER_SLOTS_OK ?
        binding_for_firmware(&current, actual_set->running_firmware_sha256) : -1;
    if (result == ECONTAINER_SLOTS_OK &&
        (running_index < 0 || current.phase != ECONTAINER_SLOT_ABORTED ||
         !current.operation.firmware_transition ||
         !current.bindings[1 - running_index].present ||
         current.bindings[1 - running_index].package_present ||
         memcmp(current.operation.target_firmware_sha256,
                current.bindings[1 - running_index].firmware_sha256, 32) != 0)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, false);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t next = current;
        next.bindings[1 - running_index] = (econtainer_slot_binding_t){0};
        next.phase = ECONTAINER_SLOT_IDLE;
        next.operation = (econtainer_slot_operation_t){0};
        result = firmware_set_matches(&next, actual_set) ?
                 commit_next(io, geometry, &current, &next, state) :
                 ECONTAINER_SLOTS_CONFLICT;
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_reconcile(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision)
{
    if (decision != NULL) {
        *decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    }
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        !firmware_set_valid(firmware_set) || state == NULL || decision == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_result_t result = load_locked(io, geometry, state);
    if (result == ECONTAINER_SLOTS_OK &&
        !firmware_set_matches(state, firmware_set)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, state, false);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        const bool running_target =
            memcmp(state->operation.target_firmware_sha256,
                   firmware_set->running_firmware_sha256, 32) == 0;
        if (state->operation.kind != ECONTAINER_SLOT_NO_PACKAGE &&
            state->phase >= ECONTAINER_SLOT_PREPARED &&
            state->phase <= ECONTAINER_SLOT_HEALTH_VERIFIED) {
            result = hash_flash(io, geometry, state->operation.slot,
                                state->operation.package_size_bytes,
                                state->operation.package_sha256);
            if (result != ECONTAINER_SLOTS_OK &&
                !(state->operation.firmware_transition && running_target)) {
                *decision = ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID;
            }
        }
        if (result == ECONTAINER_SLOTS_OK) {
            if (state->operation.firmware_transition && running_target &&
                state->phase == ECONTAINER_SLOT_PREPARED) {
                *decision = ECONTAINER_SLOT_BOOT_START_TRIAL;
            } else if (state->operation.firmware_transition && running_target &&
                       (state->phase == ECONTAINER_SLOT_WRITING ||
                        state->phase == ECONTAINER_SLOT_TRIAL_STARTED ||
                        state->phase == ECONTAINER_SLOT_HEALTH_VERIFIED ||
                        state->phase == ECONTAINER_SLOT_ABORTED)) {
                result = ECONTAINER_SLOTS_CONFLICT;
            } else if (state->operation.firmware_transition && !running_target &&
                       state->phase == ECONTAINER_SLOT_CONFIRMED) {
                result = ECONTAINER_SLOTS_CONFLICT;
            } else {
                *decision = state->phase >= ECONTAINER_SLOT_WRITING &&
                            state->phase <= ECONTAINER_SLOT_HEALTH_VERIFIED ?
                            ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED :
                            ECONTAINER_SLOT_BOOT_CONFIRMED;
            }
        }
    } else {
        memset(state, 0, sizeof(*state));
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_reserve(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *firmware_set,
    const econtainer_slot_operation_t *operation, econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        !firmware_set_valid(firmware_set) || operation == NULL || state == NULL ||
        operation->kind != ECONTAINER_SLOT_PACKAGE_WRITE ||
        operation->firmware_transition ||
        all_zero(operation->operation_id, sizeof(operation->operation_id)) ||
        all_zero(operation->package_sha256, 32) ||
        memcmp(operation->target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0 ||
        operation->slot != 0U || operation->package_size_bytes == 0U ||
        operation->guest_abi_version == 0U || operation->data_schema_version == 0U ||
        !all_zero(operation->trial_boot_id, sizeof(operation->trial_boot_id))) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    if (result == ECONTAINER_SLOTS_OK &&
        !firmware_set_matches(&current, firmware_set)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK &&
        current.operation.firmware_transition &&
        current.phase == ECONTAINER_SLOT_CONFIRMED &&
        memcmp(current.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32) != 0) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK &&
        current.operation.firmware_transition &&
        current.phase == ECONTAINER_SLOT_ABORTED) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        const int index = binding_for_firmware(&current,
            firmware_set->running_firmware_sha256);
        if (index < 0 ||
            (current.bindings[index].package_present &&
             current.bindings[index].data_schema_version != operation->data_schema_version)) {
            result = ECONTAINER_SLOTS_CONFLICT;
        }
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, true);
    }
    if (result == ECONTAINER_SLOTS_OK && current.phase != ECONTAINER_SLOT_IDLE &&
        memcmp(current.operation.operation_id, operation->operation_id,
               sizeof(operation->operation_id)) == 0) {
        result = memcmp(current.operation.target_firmware_sha256,
                        operation->target_firmware_sha256, 32) == 0 &&
                 memcmp(current.operation.package_sha256, operation->package_sha256, 32) == 0 &&
                 current.operation.package_size_bytes == operation->package_size_bytes &&
                 current.operation.guest_abi_version == operation->guest_abi_version &&
                 current.operation.data_schema_version == operation->data_schema_version ?
                 ECONTAINER_SLOTS_BUSY : ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK && current.phase >= ECONTAINER_SLOT_WRITING &&
        current.phase <= ECONTAINER_SLOT_HEALTH_VERIFIED) {
        result = ECONTAINER_SLOTS_BUSY;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        bool protected_slot[ECONTAINER_SLOT_COUNT] = {false};
        for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
            if (current.bindings[index].package_present) {
                protected_slot[current.bindings[index].slot] = true;
            }
        }
        unsigned selected = ECONTAINER_SLOT_COUNT;
        for (unsigned index = 0; index < ECONTAINER_SLOT_COUNT; ++index) {
            if (!protected_slot[index] &&
                operation->package_size_bytes <= geometry->slots[index].size_bytes) {
                selected = index;
                break;
            }
        }
        if (selected == ECONTAINER_SLOT_COUNT) {
            result = ECONTAINER_SLOTS_NO_SPACE;
        } else {
            econtainer_slots_state_t next = current;
            next.phase = ECONTAINER_SLOT_WRITING;
            next.operation = *operation;
            next.operation.slot = (uint8_t)selected;
            result = commit_next(io, geometry, &current, &next, state);
        }
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_write_and_prepare(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, econtainer_slot_source_fn source_fn,
    void *source_context, econtainer_slot_validate_fn validate_fn,
    void *validate_context, econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        source_fn == NULL || validate_fn == NULL || state == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    if (result == ECONTAINER_SLOTS_OK && current.phase != ECONTAINER_SLOT_WRITING) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK &&
        current.operation.kind != ECONTAINER_SLOT_PACKAGE_WRITE) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, false);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        const econtainer_slot_region_t *slot = &geometry->slots[current.operation.slot];
        if (!io->flash_erase(io->context, slot->offset_bytes, slot->size_bytes)) {
            result = ECONTAINER_SLOTS_IO_FAILED;
        } else {
            uint8_t buffer[SLOT_IO_BYTES];
            uint32_t offset = 0;
            while (offset < current.operation.package_size_bytes) {
                const size_t actual = current.operation.package_size_bytes - offset > SLOT_IO_BYTES ?
                                      SLOT_IO_BYTES :
                                      (size_t)(current.operation.package_size_bytes - offset);
                const size_t aligned = (actual + geometry->write_unit_bytes - 1U) /
                                       geometry->write_unit_bytes * geometry->write_unit_bytes;
                memset(buffer, 0xff, sizeof(buffer));
                if (!source_fn(source_context, offset, buffer, actual) ||
                    !io->flash_write(io->context, slot->offset_bytes + offset,
                                     buffer, aligned)) {
                    result = ECONTAINER_SLOTS_IO_FAILED;
                    break;
                }
                offset += (uint32_t)actual;
            }
        }
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = hash_flash(io, geometry, current.operation.slot,
                            current.operation.package_size_bytes,
                            current.operation.package_sha256);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        const slot_reader_t reader = {io,
            geometry->slots[current.operation.slot].offset_bytes,
            current.operation.package_size_bytes};
        const reference_reader_t references_reader = {io, geometry, current.bindings};
        const econtainer_slot_references_t references = {
            current.bindings, read_reference, (void *)&references_reader};
        const econtainer_slot_validation_result_t validation = validate_fn(
            validate_context, &current.operation, flash_relative_read,
            (void *)&reader, current.operation.package_size_bytes, &references);
        if (validation == ECONTAINER_SLOT_VALIDATION_IO_FAILED) {
            result = ECONTAINER_SLOTS_IO_FAILED;
        } else if (validation != ECONTAINER_SLOT_VALIDATION_OK) {
            result = ECONTAINER_SLOTS_UNTRUSTED;
        }
    }
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t next = current;
        next.phase = ECONTAINER_SLOT_PREPARED;
        result = commit_next(io, geometry, &current, &next, state);
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_uninstall(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *firmware_set,
    const uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES],
    const uint8_t expected_package_sha256[32],
    econtainer_slot_instance_stopped_fn instance_stopped_fn, void *instance_context,
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        !firmware_set_valid(firmware_set) || operation_id == NULL ||
        all_zero(operation_id, ECONTAINER_SLOT_OPERATION_ID_BYTES) ||
        expected_package_sha256 == NULL || all_zero(expected_package_sha256, 32) ||
        instance_stopped_fn == NULL || state == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) return ECONTAINER_SLOTS_BUSY;
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    if (result == ECONTAINER_SLOTS_OK &&
        !firmware_set_matches(&current, firmware_set)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK &&
        current.phase != ECONTAINER_SLOT_IDLE &&
        current.phase != ECONTAINER_SLOT_CONFIRMED &&
        current.phase != ECONTAINER_SLOT_ABORTED) {
        result = ECONTAINER_SLOTS_BUSY;
    }
    if (result == ECONTAINER_SLOTS_OK &&
        current.operation.firmware_transition &&
        ((current.phase == ECONTAINER_SLOT_CONFIRMED &&
          memcmp(current.operation.target_firmware_sha256,
                 firmware_set->running_firmware_sha256, 32) != 0) ||
         current.phase == ECONTAINER_SLOT_ABORTED)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK && current.phase != ECONTAINER_SLOT_IDLE &&
        memcmp(current.operation.operation_id, operation_id,
               ECONTAINER_SLOT_OPERATION_ID_BYTES) == 0) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    const int index = result == ECONTAINER_SLOTS_OK ?
        binding_for_firmware(&current, firmware_set->running_firmware_sha256) : -1;
    if (result == ECONTAINER_SLOTS_OK) {
        if (index < 0 || !current.bindings[index].package_present ||
            memcmp(current.bindings[index].package_sha256,
                   expected_package_sha256, 32) != 0) {
            result = ECONTAINER_SLOTS_CONFLICT;
        }
    }
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t next = current;
        econtainer_slot_binding_t *binding = &next.bindings[index];
        binding->package_present = false;
        binding->slot = 0;
        memset(binding->package_sha256, 0, sizeof(binding->package_sha256));
        binding->package_size_bytes = 0;
        binding->guest_abi_version = 0;
        binding->data_schema_version = 0;
        next.phase = ECONTAINER_SLOT_CONFIRMED;
        next.operation = (econtainer_slot_operation_t){0};
        memcpy(next.operation.operation_id, operation_id,
               ECONTAINER_SLOT_OPERATION_ID_BYTES);
        memcpy(next.operation.target_firmware_sha256,
               firmware_set->running_firmware_sha256, 32);
        next.operation.kind = ECONTAINER_SLOT_NO_PACKAGE;
        result = check_references(io, geometry, &next, false);
        if (result == ECONTAINER_SLOTS_OK &&
            !instance_stopped_fn(instance_context, &current.bindings[index])) {
            result = ECONTAINER_SLOTS_BUSY;
        }
        if (result == ECONTAINER_SLOTS_OK) {
            result = commit_next(io, geometry, &current, &next, state);
        }
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_begin_trial(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t running_firmware_sha256[32],
    const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        running_firmware_sha256 == NULL || boot_id == NULL || state == NULL ||
        all_zero(boot_id, ECONTAINER_SLOT_BOOT_ID_BYTES)) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    if (result == ECONTAINER_SLOTS_OK &&
        (current.phase != ECONTAINER_SLOT_PREPARED ||
         memcmp(current.operation.target_firmware_sha256,
                running_firmware_sha256, 32) != 0)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, true);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t next = current;
        next.phase = ECONTAINER_SLOT_TRIAL_STARTED;
        memcpy(next.operation.trial_boot_id, boot_id, ECONTAINER_SLOT_BOOT_ID_BYTES);
        result = commit_next(io, geometry, &current, &next, state);
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_mark_healthy(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        boot_id == NULL || state == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    if (result == ECONTAINER_SLOTS_OK &&
        (current.phase != ECONTAINER_SLOT_TRIAL_STARTED ||
         memcmp(current.operation.trial_boot_id, boot_id,
                ECONTAINER_SLOT_BOOT_ID_BYTES) != 0)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, true);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t next = current;
        next.phase = ECONTAINER_SLOT_HEALTH_VERIFIED;
        result = commit_next(io, geometry, &current, &next, state);
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_confirm(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t running_firmware_sha256[32],
    const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        running_firmware_sha256 == NULL || boot_id == NULL || state == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    int index = -1;
    if (result == ECONTAINER_SLOTS_OK) {
        index = binding_for_firmware(&current, running_firmware_sha256);
        if (current.phase != ECONTAINER_SLOT_HEALTH_VERIFIED || index < 0 ||
            memcmp(current.operation.target_firmware_sha256,
                   running_firmware_sha256, 32) != 0 ||
            memcmp(current.operation.trial_boot_id, boot_id,
                   ECONTAINER_SLOT_BOOT_ID_BYTES) != 0) {
            result = ECONTAINER_SLOTS_CONFLICT;
        }
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, true);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t next = current;
        econtainer_slot_binding_t *binding = &next.bindings[index];
        if (current.operation.kind != ECONTAINER_SLOT_NO_PACKAGE) {
            binding->package_present = true;
            binding->slot = current.operation.slot;
            binding->package_size_bytes = current.operation.package_size_bytes;
            memcpy(binding->package_sha256, current.operation.package_sha256, 32);
            binding->guest_abi_version = current.operation.guest_abi_version;
            binding->data_schema_version = current.operation.data_schema_version;
        }
        next.phase = ECONTAINER_SLOT_CONFIRMED;
        result = commit_next(io, geometry, &current, &next, state);
    }
    io->unlock(io->context);
    return result;
}

econtainer_slots_result_t econtainer_slots_abandon(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES],
    econtainer_slot_trial_stopped_fn trial_stopped_fn, void *trial_context,
    econtainer_slots_state_t *state)
{
    if (!io_valid(io) || !econtainer_slots_geometry_valid(geometry) ||
        boot_id == NULL || state == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!io->lock(io->context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    econtainer_slots_state_t current;
    econtainer_slots_result_t result = begin_locked(io, geometry, expected_sequence, &current);
    if (result == ECONTAINER_SLOTS_OK &&
        (current.phase < ECONTAINER_SLOT_WRITING ||
         current.phase > ECONTAINER_SLOT_HEALTH_VERIFIED)) {
        result = ECONTAINER_SLOTS_CONFLICT;
    }
    if (result == ECONTAINER_SLOTS_OK &&
        current.phase >= ECONTAINER_SLOT_TRIAL_STARTED &&
        memcmp(current.operation.trial_boot_id, boot_id,
               ECONTAINER_SLOT_BOOT_ID_BYTES) == 0) {
        if (trial_stopped_fn == NULL ||
            !trial_stopped_fn(trial_context, current.operation.operation_id)) {
            result = ECONTAINER_SLOTS_BUSY;
        }
    }
    if (result == ECONTAINER_SLOTS_OK) {
        result = check_references(io, geometry, &current, false);
    }
    if (result == ECONTAINER_SLOTS_OK) {
        econtainer_slots_state_t next = current;
        next.phase = ECONTAINER_SLOT_ABORTED;
        result = commit_next(io, geometry, &current, &next, state);
    }
    io->unlock(io->context);
    return result;
}
