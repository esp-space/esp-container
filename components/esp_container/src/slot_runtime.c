#include "slot_runtime_internal.h"

#include "package_slot_internal.h"

#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t size_bytes;
} mapped_package_t;

typedef struct {
    const econtainer_slots_io_t *io;
    const econtainer_package_slot_validation_t *validation;
    const econtainer_runtime_limits_t *platform_limits;
    econtainer_runtime_t **out;
    econtainer_runtime_result_t runtime_result;
    uint32_t event_queue_limit;
    uint8_t package_sha256[32];
    size_t product_id_offset_bytes;
    size_t product_id_size_bytes;
    size_t product_version_offset_bytes;
    size_t product_version_size_bytes;
    uint32_t guest_abi_version;
    uint32_t data_schema_version;
} slot_runtime_open_t;

static bool read_mapped(void *context, size_t offset_bytes,
                        uint8_t *destination, size_t size_bytes)
{
    const mapped_package_t *package = context;
    if (size_bytes == 0 || size_bytes > 512 || offset_bytes > package->size_bytes ||
        size_bytes > package->size_bytes - offset_bytes) return false;
    memcpy(destination, package->bytes + offset_bytes, size_bytes);
    return true;
}

static uint32_t minimum(uint32_t first, uint32_t second)
{
    return first < second ? first : second;
}

static econtainer_slots_result_t open_selected(void *context,
    const econtainer_slot_package_t *package, uint32_t offset_bytes)
{
    slot_runtime_open_t *open = context;
    const econtainer_slots_io_t *io = open->io;
    const uint8_t *mapped = NULL;
    uintptr_t handle = 0;
    if (!io->flash_map(io->context, offset_bytes, package->package_size_bytes,
                        &mapped, &handle)) return ECONTAINER_SLOTS_IO_FAILED;
    econtainer_slots_result_t result = ECONTAINER_SLOTS_IO_FAILED;
    if (mapped != NULL) {
        const mapped_package_t reader = {mapped, package->package_size_bytes};
        econtainer_package_info_t info;
        econtainer_package_slot_validation_t validation = *open->validation;
        validation.verified_info = &info;
        const econtainer_slot_validation_result_t admitted = econtainer_package_slot_check(
            &validation, package, read_mapped, (void *)&reader, reader.size_bytes);
        if (admitted == ECONTAINER_SLOT_VALIDATION_IO_FAILED) {
            result = ECONTAINER_SLOTS_IO_FAILED;
        } else if (admitted != ECONTAINER_SLOT_VALIDATION_OK) {
            result = ECONTAINER_SLOTS_UNTRUSTED;
        } else if (info.wasm_offset_bytes > reader.size_bytes ||
                   info.wasm_size_bytes > reader.size_bytes - info.wasm_offset_bytes) {
            result = ECONTAINER_SLOTS_UNTRUSTED;
        } else {
            econtainer_runtime_limits_t limits = *open->platform_limits;
            limits.max_wasm_bytes = minimum(limits.max_wasm_bytes,
                                             (uint32_t)info.wasm_size_bytes);
            limits.max_memory_pages = minimum(limits.max_memory_pages,
                                                info.memory_limit_bytes / 65536U);
            limits.stack_size_bytes = minimum(limits.stack_size_bytes, info.stack_limit_bytes);
            limits.allowed_capabilities &= validation.wasm_authorization.allowed_capabilities &
                                           info.requested_capabilities;
            if ((limits.allowed_capabilities & ECONTAINER_CAP_LOG) == 0) limits.max_log_bytes = 0;
            if ((limits.allowed_capabilities & ECONTAINER_CAP_TIMER) == 0) limits.max_timers = 0;
            const int32_t budget = (int32_t)info.instruction_budget;
            if (limits.init_instruction_budget > budget) limits.init_instruction_budget = budget;
            if (limits.event_instruction_budget > budget) limits.event_instruction_budget = budget;
            if (limits.stop_instruction_budget > budget) limits.stop_instruction_budget = budget;
            open->runtime_result = econtainer_runtime_open(mapped + info.wasm_offset_bytes,
                info.wasm_size_bytes, &limits, open->out);
            if (open->runtime_result == ECONTAINER_RUNTIME_OK && *open->out != NULL) {
                open->event_queue_limit = info.event_queue_limit;
                open->product_id_offset_bytes = info.product_id_offset_bytes;
                open->product_id_size_bytes = info.product_id_size_bytes;
                open->product_version_offset_bytes = info.product_version_offset_bytes;
                open->product_version_size_bytes = info.product_version_size_bytes;
                open->guest_abi_version = info.guest_abi_version;
                open->data_schema_version = info.data_schema_version;
                memcpy(open->package_sha256, info.package_sha256,
                       sizeof open->package_sha256);
            }
            result = ECONTAINER_SLOTS_OK;
        }
    }
    if (!io->flash_unmap(io->context, handle)) {
        if (*open->out != NULL) {
            open->runtime_result = econtainer_runtime_close(open->out);
            if (open->runtime_result == ECONTAINER_RUNTIME_OK) {
                open->runtime_result = ECONTAINER_RUNTIME_INVALID_STATE;
            }
        }
        return ECONTAINER_SLOTS_IO_FAILED;
    }
    return result;
}

econtainer_slot_runtime_result_t econtainer_slot_runtime_open(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_selection_request_t *request,
    const econtainer_package_slot_validation_t *validation,
    const econtainer_runtime_limits_t *platform_limits,
    econtainer_runtime_t **out)
{
    econtainer_slot_runtime_result_t result = {
        .slots = ECONTAINER_SLOTS_INVALID,
        .runtime = ECONTAINER_RUNTIME_INVALID_STATE,
    };
    if (io == NULL || io->flash_map == NULL || io->flash_unmap == NULL ||
        validation == NULL || platform_limits == NULL || out == NULL || *out != NULL) {
        return result;
    }
    slot_runtime_open_t open = {.io = io, .validation = validation,
        .platform_limits = platform_limits, .out = out,
        .runtime_result = ECONTAINER_RUNTIME_INVALID_STATE};
    result.slots = econtainer_slots_with_selected_package(io, geometry, request,
                                                           open_selected, &open);
    result.runtime = open.runtime_result;
    if (result.slots == ECONTAINER_SLOTS_OK &&
        result.runtime == ECONTAINER_RUNTIME_OK && *out != NULL) {
        result.event_queue_limit = open.event_queue_limit;
        result.product_id_offset_bytes = open.product_id_offset_bytes;
        result.product_id_size_bytes = open.product_id_size_bytes;
        result.product_version_offset_bytes = open.product_version_offset_bytes;
        result.product_version_size_bytes = open.product_version_size_bytes;
        result.guest_abi_version = open.guest_abi_version;
        result.data_schema_version = open.data_schema_version;
        memcpy(result.package_sha256, open.package_sha256,
               sizeof result.package_sha256);
    }
    return result;
}
