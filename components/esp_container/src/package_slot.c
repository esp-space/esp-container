#include "package_slot_internal.h"

#include <limits.h>
#include <string.h>

typedef struct {
    econtainer_slot_read_fn read_fn;
    void *read_context;
    size_t package_size_bytes;
} econtainer_package_slot_reader_t;

static bool read_slot(void *context, size_t offset_bytes,
                      uint8_t *destination, size_t size_bytes)
{
    const econtainer_package_slot_reader_t *reader = context;
    return size_bytes > 0U && size_bytes <= 512U &&
           offset_bytes <= reader->package_size_bytes &&
           size_bytes <= reader->package_size_bytes - offset_bytes &&
           reader->read_fn(reader->read_context, offset_bytes, destination, size_bytes);
}

typedef struct {
    const econtainer_slot_references_t *references;
    unsigned binding_index;
} reference_package_t;

static bool read_reference_package(void *context, size_t offset_bytes,
                                   uint8_t *destination, size_t size_bytes)
{
    const reference_package_t *reader = context;
    return reader->references->read(reader->references->context, reader->binding_index,
                                    offset_bytes, destination, size_bytes);
}

static bool product_matches(const econtainer_package_slot_validation_t *validation,
                            const econtainer_package_info_t *info)
{
    const size_t expected_size = strlen(validation->expected_product_id);
    return expected_size == info->product_id_size_bytes &&
           info->product_id_offset_bytes <= info->manifest_size_bytes &&
           expected_size <= info->manifest_size_bytes - info->product_id_offset_bytes &&
           memcmp(validation->package_workspace->manifest + info->product_id_offset_bytes,
                  validation->expected_product_id, expected_size) == 0;
}

econtainer_slot_validation_result_t econtainer_package_slot_check(
    econtainer_package_slot_validation_t *validation,
    const econtainer_slot_package_t *package,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references)
{
    if (validation == NULL || validation->verified_info == NULL) {
        return ECONTAINER_SLOT_VALIDATION_UNTRUSTED;
    }
    memset(validation->verified_info, 0, sizeof(*validation->verified_info));
    if (validation->package_workspace != NULL) {
        memset(validation->package_workspace->manifest, 0,
               sizeof(validation->package_workspace->manifest));
        memset(validation->package_workspace->signature, 0,
               sizeof(validation->package_workspace->signature));
    }
    if (read_fn == NULL || package == NULL || references == NULL ||
        references->bindings == NULL || references->read == NULL ||
        validation->expected_product_id == NULL ||
        validation->expected_product_id[0] == '\0' ||
        validation->public_key_rsa_der == NULL || validation->expected_key_id == NULL ||
        validation->package_workspace == NULL || validation->wasm_workspace == NULL ||
        validation->max_event_queue_limit == 0U ||
        validation->max_instruction_budget == 0U ||
        validation->max_instruction_budget > INT32_MAX ||
        validation->max_host_call_timeout_ms == 0U ||
        package_size_bytes != package->package_size_bytes) {
        return ECONTAINER_SLOT_VALIDATION_UNTRUSTED;
    }

    econtainer_slot_validation_result_t result = ECONTAINER_SLOT_VALIDATION_UNTRUSTED;
    econtainer_package_info_t info;
    uint32_t version_offsets[ECONTAINER_SLOT_BINDING_COUNT] = {0};
    uint32_t version_sizes[ECONTAINER_SLOT_BINDING_COUNT] = {0};
    /* Reuse the same workspace sequentially, then leave the candidate's
     * complete manifest there for the existing metadata return contract.
     * A rollback reference belongs to its own firmware: verify signed bytes
     * and persisted ABI/schema, without applying the new firmware's grants. */
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &references->bindings[index];
        if (!binding->present || !binding->package_present) continue;
        if (memcmp(binding->package_sha256, package->package_sha256, 32U) == 0) {
            /* The slot engine checked every reference's actual digest under
             * this lock. The candidate's following full verification proves
             * identical bytes; persistent metadata must also agree. */
            if (binding->package_size_bytes != package->package_size_bytes ||
                binding->guest_abi_version != package->guest_abi_version ||
                binding->data_schema_version != package->data_schema_version) goto done;
            continue;
        }
        const reference_package_t reference = {references, index};
        const econtainer_package_result_t checked = econtainer_package_verify(
            read_reference_package, (void *)&reference, binding->package_size_bytes,
            ECONTAINER_PACKAGE_WASM_MAX_BYTES, validation->public_key_rsa_der,
            validation->public_key_size_bytes, validation->expected_key_id,
            validation->package_workspace, &info);
        if (checked != ECONTAINER_PACKAGE_OK) {
            if (checked == ECONTAINER_PACKAGE_READ_FAILED)
                result = ECONTAINER_SLOT_VALIDATION_IO_FAILED;
            goto done;
        }
        if (memcmp(info.package_sha256, binding->package_sha256, 32U) != 0 ||
            info.guest_abi_version != binding->guest_abi_version ||
            info.data_schema_version != binding->data_schema_version) goto done;
        /* The signed product identity must match before its version can
         * conflict. Another firmware's differently authorized product is
         * not silently reinterpreted as the candidate product. */
        if (product_matches(validation, &info) &&
            memcmp(info.package_sha256, package->package_sha256, 32U) != 0) {
            version_offsets[index] = (uint32_t)info.product_version_offset_bytes;
            version_sizes[index] = (uint32_t)info.product_version_size_bytes;
        }
    }

    const econtainer_package_slot_reader_t reader = {
        .read_fn = read_fn,
        .read_context = read_context,
        .package_size_bytes = package_size_bytes,
    };
    const econtainer_package_result_t package_result = econtainer_package_verify(
        read_slot, (void *)&reader, package_size_bytes, validation->max_wasm_bytes,
        validation->public_key_rsa_der, validation->public_key_size_bytes,
        validation->expected_key_id, validation->package_workspace, &info);
    if (package_result != ECONTAINER_PACKAGE_OK) {
        if (package_result == ECONTAINER_PACKAGE_READ_FAILED)
            result = ECONTAINER_SLOT_VALIDATION_IO_FAILED;
        goto done;
    }
    bool accepted = memcmp(info.package_sha256,
                           package->package_sha256, 32U) == 0 &&
                    info.guest_abi_version == package->guest_abi_version &&
                    info.data_schema_version == package->data_schema_version &&
                    product_matches(validation, &info) &&
                    info.event_queue_limit <= validation->max_event_queue_limit &&
                    info.instruction_budget <= validation->max_instruction_budget &&
                    info.host_call_timeout_ms <= validation->max_host_call_timeout_ms &&
                    info.storage_limit_bytes <= validation->max_storage_limit_bytes;
    if (accepted) {
        const econtainer_package_wasm_result_t wasm_result = econtainer_package_wasm_check(
            read_slot, (void *)&reader, &info, &validation->wasm_authorization,
            validation->wasm_workspace);
        if (wasm_result == ECONTAINER_PACKAGE_WASM_OK) {
            result = ECONTAINER_SLOT_VALIDATION_OK;
        } else if (wasm_result == ECONTAINER_PACKAGE_WASM_READ_FAILED) {
            result = ECONTAINER_SLOT_VALIDATION_IO_FAILED;
        }
    }
    if (result == ECONTAINER_SLOT_VALIDATION_OK) {
        for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
            if (version_sizes[index] == 0U ||
                version_sizes[index] != info.product_version_size_bytes) continue;
            bool equal = true;
            for (uint32_t offset = 0; offset < version_sizes[index];) {
                const size_t count = version_sizes[index] - offset > 512U ? 512U :
                                    (size_t)(version_sizes[index] - offset);
                /* v1's first member is manifest.json, following one 512B header. */
                if (!references->read(references->context, index,
                        512U + (size_t)version_offsets[index] + offset,
                        validation->package_workspace->block, count)) {
                    result = ECONTAINER_SLOT_VALIDATION_IO_FAILED;
                    goto done;
                }
                if (memcmp(validation->package_workspace->block,
                        validation->package_workspace->manifest +
                            info.product_version_offset_bytes + offset, count) != 0) {
                    equal = false;
                    break;
                }
                offset += (uint32_t)count;
            }
            if (equal) {
                result = ECONTAINER_SLOT_VALIDATION_UNTRUSTED;
                goto done;
            }
        }
    }
done:
    if (result == ECONTAINER_SLOT_VALIDATION_OK) {
        *validation->verified_info = info;
    } else {
        memset(validation->verified_info, 0, sizeof(*validation->verified_info));
        memset(validation->package_workspace->manifest, 0,
               sizeof(validation->package_workspace->manifest));
        memset(validation->package_workspace->signature, 0,
               sizeof(validation->package_workspace->signature));
    }
    return result;
}

econtainer_slot_validation_result_t econtainer_package_slot_validate(
    void *context, const econtainer_slot_operation_t *operation,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references)
{
    econtainer_slot_package_t package = {0};
    if (operation != NULL) {
        package.slot = operation->slot;
        memcpy(package.package_sha256, operation->package_sha256, 32);
        package.package_size_bytes = operation->package_size_bytes;
        package.guest_abi_version = operation->guest_abi_version;
        package.data_schema_version = operation->data_schema_version;
    }
    return econtainer_package_slot_check(context, operation != NULL ? &package : NULL,
                                          read_fn, read_context, package_size_bytes, references);
}

econtainer_slot_validation_result_t econtainer_package_slot_validate_binding(
    void *context, const econtainer_slot_binding_t *binding,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references)
{
    econtainer_slot_package_t package = {0};
    if (binding == NULL || !binding->present || !binding->package_present) {
        return ECONTAINER_SLOT_VALIDATION_UNTRUSTED;
    }
    package.slot = binding->slot;
    memcpy(package.package_sha256, binding->package_sha256, sizeof(package.package_sha256));
    package.package_size_bytes = binding->package_size_bytes;
    package.guest_abi_version = binding->guest_abi_version;
    package.data_schema_version = binding->data_schema_version;
    return econtainer_package_slot_check(context, &package,
                                          read_fn, read_context, package_size_bytes, references);
}
