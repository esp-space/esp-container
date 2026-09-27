#include "esp_container_slots_idf.h"
#include "nvs.h"

#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

enum { PACKAGE_BASE = 0x500000, PACKAGE_BYTES = 0x3000,
       NVS_BASE = 0x700000, NVS_BYTES = 0x4000, SECTOR_BYTES = 0x1000 };

struct test_semaphore { bool held; };
static struct test_semaphore storage_lock;
static esp_partition_t package_partition = {
    .type = ESP_PARTITION_TYPE_DATA,
    .subtype = ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
    .address = PACKAGE_BASE,
    .size = PACKAGE_BYTES,
    .erase_size = SECTOR_BYTES,
    .label = "product_pkg",
};
static esp_partition_t nvs_partition = {
    .type = ESP_PARTITION_TYPE_DATA,
    .subtype = ESP_PARTITION_SUBTYPE_DATA_NVS,
    .address = NVS_BASE,
    .size = NVS_BYTES,
    .erase_size = SECTOR_BYTES,
    .label = "product_nvs",
};
static bool package_available = true;
static uint8_t flash[PACKAGE_BYTES];
static uint8_t stored_blob[ECONTAINER_SLOT_BLOB_BYTES];
static uint8_t staged_blob[ECONTAINER_SLOT_BLOB_BYTES];
static size_t stored_size;
static bool namespace_exists, staged;
static bool fail_open, fail_get, fail_set, fail_commit, commit_then_fail;
static unsigned opens, commits, reads, writes, erases;
static nvs_handle_t open_handle;
static nvs_open_mode_t open_mode;
static bool fail_map, null_map, mapping_live;
static unsigned maps, unmaps;
static size_t mapped_offset, mapped_size;
static esp_partition_mmap_handle_t next_map_handle, live_map_handle;

static econtainer_slots_idf_config_t config(void)
{
    econtainer_slots_idf_config_t result = {
        .package_partition_label = "product_pkg",
        .package_partition_offset_bytes = PACKAGE_BASE,
        .package_partition_size_bytes = PACKAGE_BYTES,
        .slots = {{PACKAGE_BASE, SECTOR_BYTES},
                  {PACKAGE_BASE + SECTOR_BYTES, SECTOR_BYTES},
                  {PACKAGE_BASE + 2U * SECTOR_BYTES, SECTOR_BYTES}},
        .nvs_partition_label = "product_nvs",
        .nvs_partition_offset_bytes = NVS_BASE,
        .nvs_partition_size_bytes = NVS_BYTES,
        .nvs_namespace = "product_slots",
        .nvs_key = "state",
        .storage_lock = &storage_lock,
    };
    return result;
}

static void reset(void)
{
    package_partition.type = ESP_PARTITION_TYPE_DATA;
    package_partition.subtype = ESP_PARTITION_SUBTYPE_DATA_UNDEFINED;
    package_partition.readonly = false;
    package_partition.encrypted = false;
    nvs_partition.readonly = false;
    package_available = true;
    storage_lock.held = false;
    namespace_exists = staged = false;
    fail_open = fail_get = fail_set = fail_commit = commit_then_fail = false;
    stored_size = 0;
    opens = commits = reads = writes = erases = 0;
    open_handle = 0;
    fail_map = null_map = mapping_live = false;
    maps = unmaps = 0;
    mapped_offset = mapped_size = 0;
    next_map_handle = live_map_handle = 0;
    memset(flash, 0xff, sizeof flash);
    memset(stored_blob, 0, sizeof stored_blob);
    memset(staged_blob, 0, sizeof staged_blob);
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned ticks)
{
    assert(semaphore == &storage_lock && ticks == 0U);
    if (semaphore->held) return pdFALSE;
    semaphore->held = true;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &storage_lock && semaphore->held);
    assert(!mapping_live);
    semaphore->held = false;
    return pdTRUE;
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label)
{
    if (package_available && type == package_partition.type &&
        subtype == package_partition.subtype &&
        strcmp(label, package_partition.label) == 0) return &package_partition;
    if (type == nvs_partition.type && subtype == nvs_partition.subtype &&
        strcmp(label, nvs_partition.label) == 0) return &nvs_partition;
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *destination, size_t size)
{
    assert(partition == &package_partition && offset + size <= sizeof flash);
    ++reads;
    memcpy(destination, flash + offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset,
                                    size_t size)
{
    assert(!mapping_live);
    assert(partition == &package_partition && offset + size <= sizeof flash);
    assert(offset % SECTOR_BYTES == 0U && size % SECTOR_BYTES == 0U);
    ++erases;
    memset(flash + offset, 0xff, size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                              const void *source, size_t size)
{
    assert(!mapping_live);
    assert(partition == &package_partition && offset + size <= sizeof flash);
    ++writes;
    const uint8_t *bytes = source;
    for (size_t index = 0; index < size; ++index) {
        assert(flash[offset + index] == 0xff);
        flash[offset + index] = bytes[index];
    }
    return ESP_OK;
}

esp_err_t esp_partition_mmap(const esp_partition_t *partition, size_t offset,
                             size_t size, esp_partition_mmap_flag_t flags,
                             const void **out_ptr,
                             esp_partition_mmap_handle_t *out_handle)
{
    assert(storage_lock.held && !mapping_live);
    assert(partition == &package_partition && size > 0U && size <= sizeof flash);
    assert(offset <= sizeof flash - size && out_ptr != NULL && out_handle != NULL);
    assert(flags == (ESP_PARTITION_MMAP_DATA | ESP_PARTITION_MMAP_BLOCKS_WRITE));
    ++maps; mapped_offset = offset; mapped_size = size;
    /* Failure may modify SDK outputs; it never establishes a mapping. */
    *out_ptr = flash + offset; *out_handle = next_map_handle;
    if (fail_map) return ESP_FAIL;
    mapping_live = true; live_map_handle = next_map_handle;
    if (null_map) *out_ptr = NULL;
    return ESP_OK;
}

void esp_partition_munmap(esp_partition_mmap_handle_t handle)
{
    assert(storage_lock.held && mapping_live && handle == live_map_handle);
    mapping_live = false; ++unmaps;
}

esp_err_t nvs_open_from_partition(const char *partition_name,
                                  const char *namespace_name,
                                  nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(partition_name, "product_nvs") == 0);
    assert(strcmp(namespace_name, "product_slots") == 0);
    assert(open_handle == 0 && handle != NULL);
    if (fail_open) return ESP_FAIL;
    if (mode == NVS_READONLY && !namespace_exists) return ESP_ERR_NVS_NOT_FOUND;
    open_handle = ++opens;
    open_mode = mode;
    *handle = open_handle;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *size)
{
    assert(handle == open_handle && open_mode == NVS_READONLY &&
           strcmp(key, "state") == 0 && size != NULL);
    if (fail_get) return ESP_FAIL;
    if (stored_size == 0U) return ESP_ERR_NVS_NOT_FOUND;
    if (out != NULL) {
        assert(*size >= stored_size);
        memcpy(out, stored_blob, stored_size);
    }
    *size = stored_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data,
                       size_t size)
{
    assert(handle == open_handle && open_mode == NVS_READWRITE &&
           strcmp(key, "state") == 0 && size == ECONTAINER_SLOT_BLOB_BYTES);
    if (fail_set) return ESP_FAIL;
    memcpy(staged_blob, data, size);
    staged = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(!mapping_live);
    assert(handle == open_handle && open_mode == NVS_READWRITE && staged);
    ++commits;
    if (!fail_commit || commit_then_fail) {
        memcpy(stored_blob, staged_blob, sizeof stored_blob);
        stored_size = sizeof stored_blob;
        namespace_exists = true;
    }
    return fail_commit ? ESP_FAIL : ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == open_handle);
    open_handle = 0;
    staged = false;
}

static void test_binding_guards(void)
{
    econtainer_slots_idf_provider_t provider;
    econtainer_slots_idf_config_t selected = config();
    package_available = false;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    assert(provider.io.flash_erase == NULL);
    package_available = true;
    package_partition.subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    package_partition.subtype = ESP_PARTITION_SUBTYPE_DATA_UNDEFINED;
    package_partition.readonly = true;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    package_partition.readonly = false;
    nvs_partition.subtype = ESP_PARTITION_SUBTYPE_DATA_UNDEFINED;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    nvs_partition.subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    nvs_partition.readonly = true;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    nvs_partition.readonly = false;
    selected.package_partition_offset_bytes++;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    selected = config();
    selected.slots[2].offset_bytes = PACKAGE_BASE + 2U * SECTOR_BYTES + 1U;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    selected = config();
    selected.nvs_partition_size_bytes++;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    selected = config();
    nvs_partition.address = PACKAGE_BASE + SECTOR_BYTES;
    selected.nvs_partition_offset_bytes = nvs_partition.address;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    nvs_partition.address = NVS_BASE;
    selected = config();
    selected.nvs_namespace = "TooLongOrWrong";
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    selected = config();
    selected.storage_lock = NULL;
    assert(!econtainer_slots_idf_bind(&provider, &selected));
    assert(erases == 0 && writes == 0 && commits == 0);
}

static void test_provider_io(void)
{
    econtainer_slots_idf_config_t selected = config();
    econtainer_slots_idf_provider_t provider;
    assert(econtainer_slots_idf_bind(&provider, &selected));
    assert(provider.geometry.write_unit_bytes == 4U);
    assert(provider.io.lock(provider.io.context));
    assert(!provider.io.lock(provider.io.context));
    uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES] = {0};
    assert(provider.io.read_blob(provider.io.context, blob) == ECONTAINER_SLOT_BLOB_NOT_FOUND);
    assert(provider.io.flash_erase(provider.io.context, PACKAGE_BASE, SECTOR_BYTES));
    assert(!provider.io.flash_erase(provider.io.context, PACKAGE_BASE + 1U, SECTOR_BYTES));
    const uint8_t bytes[16] = {1, 2, 3, 4};
    assert(provider.io.flash_write(provider.io.context, PACKAGE_BASE, bytes, sizeof bytes));
    assert(!provider.io.flash_write(provider.io.context, PACKAGE_BASE + PACKAGE_BYTES, bytes, sizeof bytes));
    assert(!provider.io.flash_write(provider.io.context, PACKAGE_BASE + 1U, bytes, sizeof bytes));
    assert(!provider.io.flash_write(provider.io.context, PACKAGE_BASE, bytes, SIZE_MAX));
    assert(!provider.io.flash_write(provider.io.context, PACKAGE_BASE, bytes, SIZE_MAX - 3U));
    uint8_t observed[16] = {0};
    assert(provider.io.flash_read(provider.io.context, PACKAGE_BASE, observed, sizeof observed));
    assert(memcmp(observed, bytes, sizeof bytes) == 0);
    assert(!provider.io.flash_read(provider.io.context, NVS_BASE, observed, sizeof observed));
    assert(!provider.io.flash_read(provider.io.context, PACKAGE_BASE, observed, SIZE_MAX));
    memset(blob, 0x55, sizeof blob);
    assert(provider.io.write_blob(provider.io.context, blob));
    assert(commits == 1 && opens == 1);
    memset(blob, 0, sizeof blob);
    assert(provider.io.read_blob(provider.io.context, blob) == ECONTAINER_SLOT_BLOB_FOUND);
    assert(blob[0] == 0x55 && opens == 2);
    stored_size = 20U;
    assert(provider.io.read_blob(provider.io.context, blob) == ECONTAINER_SLOT_BLOB_READ_FAILED);
    stored_size = ECONTAINER_SLOT_BLOB_BYTES;
    fail_get = true;
    assert(provider.io.read_blob(provider.io.context, blob) == ECONTAINER_SLOT_BLOB_READ_FAILED);
    fail_get = false;
    fail_commit = commit_then_fail = true;
    assert(!provider.io.write_blob(provider.io.context, blob));
    assert(commits == 2 && stored_size == ECONTAINER_SLOT_BLOB_BYTES);
    provider.io.unlock(provider.io.context);
    assert(!storage_lock.held && writes == 1 && erases == 1 && reads == 1);
}

static void test_provider_mapping(void)
{
    econtainer_slots_idf_config_t selected = config();
    econtainer_slots_idf_provider_t provider;
    assert(econtainer_slots_idf_bind(&provider, &selected));
    assert(provider.io.flash_map != NULL && provider.io.flash_unmap != NULL);
    assert(provider.io.lock(provider.io.context));
    for (size_t i = 0; i < sizeof flash; ++i) flash[i] = (uint8_t)(i * 29U);
    const uint8_t *mapped = NULL; uintptr_t handle = UINTPTR_MAX;
    const size_t relative_offset = SECTOR_BYTES + 3U, size = SECTOR_BYTES + 5U;
    assert(provider.io.flash_map(provider.io.context,
        PACKAGE_BASE + (uint32_t)relative_offset, size, &mapped, &handle));
    assert(mapping_live && handle == 0U && mapped == flash + relative_offset);
    assert(mapped_offset == relative_offset && mapped_size == size);
    assert(memcmp(mapped, flash + relative_offset, size) == 0);
    provider.io.flash_unmap(provider.io.context, handle);
    assert(!mapping_live && maps == 1 && unmaps == 1);

    next_map_handle = UINT32_MAX;
    assert(provider.io.flash_map(provider.io.context,
        PACKAGE_BASE + PACKAGE_BYTES - 1U, 1U, &mapped, &handle));
    assert(mapped == flash + PACKAGE_BYTES - 1U && mapped_size == 1U && handle == UINT32_MAX);
    provider.io.flash_unmap(provider.io.context, handle);
    assert(!mapping_live && maps == 2 && unmaps == 2);

    const struct { uint32_t offset; size_t size; } invalid[] = {
        {PACKAGE_BASE, 0}, {PACKAGE_BASE - 1U, 1},
        {PACKAGE_BASE + PACKAGE_BYTES, 1}, {PACKAGE_BASE + 1U, PACKAGE_BYTES},
        {PACKAGE_BASE, SIZE_MAX}, {PACKAGE_BASE, SIZE_MAX - 3U},
        {UINT32_MAX, 2},
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        mapped = flash; handle = UINTPTR_MAX;
        assert(!provider.io.flash_map(provider.io.context, invalid[i].offset,
            invalid[i].size, &mapped, &handle));
        assert(mapped == NULL && handle == 0U && maps == 2 && unmaps == 2);
    }
    handle = UINTPTR_MAX;
    assert(!provider.io.flash_map(provider.io.context, PACKAGE_BASE, 1U, NULL, &handle));
    assert(handle == 0U);
    mapped = flash;
    assert(!provider.io.flash_map(provider.io.context, PACKAGE_BASE, 1U, &mapped, NULL));
    assert(mapped == NULL);
    mapped = flash; handle = UINTPTR_MAX;
    assert(!provider.io.flash_map(NULL, PACKAGE_BASE, 1U, &mapped, &handle));
    assert(mapped == NULL && handle == 0U && maps == 2);

    fail_map = true; next_map_handle = 123U;
    mapped = flash; handle = UINTPTR_MAX;
    assert(!provider.io.flash_map(provider.io.context, PACKAGE_BASE + 7U, 29U, &mapped, &handle));
    assert(mapped == NULL && handle == 0U && !mapping_live && maps == 3 && unmaps == 2);
    fail_map = false; null_map = true;
    mapped = flash; handle = UINTPTR_MAX;
    assert(!provider.io.flash_map(provider.io.context, PACKAGE_BASE, 1U, &mapped, &handle));
    assert(mapped == NULL && handle == 0U && !mapping_live && maps == 4 && unmaps == 3);
    provider.io.unlock(provider.io.context);
    assert(!storage_lock.held && erases == 0 && writes == 0 && commits == 0);
}

static void test_slot_engine_with_idf_provider(void)
{
    econtainer_slots_idf_config_t selected = config();
    econtainer_slots_idf_provider_t provider;
    assert(econtainer_slots_idf_bind(&provider, &selected));
    econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT] = {0};
    bindings[0].present = true;
    memset(bindings[0].firmware_sha256, 0x11, sizeof bindings[0].firmware_sha256);
    econtainer_slot_firmware_set_t firmware_set = {0};
    firmware_set.bootable_count = 1;
    memset(firmware_set.bootable_firmware_sha256[0], 0x11, 32);
    memset(firmware_set.running_firmware_sha256, 0x11, 32);
    econtainer_slot_firmware_set_t incorrect = firmware_set;
    memset(incorrect.bootable_firmware_sha256[0], 0x22, 32);
    memset(incorrect.running_firmware_sha256, 0x22, 32);
    assert(econtainer_slots_initialize(&provider.io, &provider.geometry,
                                       &incorrect, bindings) == ECONTAINER_SLOTS_INVALID);
    assert(commits == 0 && erases == 0);
    assert(econtainer_slots_initialize(&provider.io, &provider.geometry,
                                       &firmware_set, bindings) == ECONTAINER_SLOTS_OK);
    assert(commits == 1 && stored_size == ECONTAINER_SLOT_BLOB_BYTES && !storage_lock.held);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&provider.io, &provider.geometry, &state) == ECONTAINER_SLOTS_OK);
    assert(state.sequence == 1U && state.bindings[0].present && state.phase == ECONTAINER_SLOT_IDLE);
    econtainer_slot_boot_decision_t decision;
    assert(econtainer_slots_reconcile(&provider.io, &provider.geometry,
                                      &firmware_set, &state, &decision) == ECONTAINER_SLOTS_OK);
    assert(decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    assert(erases == 0 && writes == 0);
}

static bool stopped_confirmed_instance(void *context,
                                       const econtainer_slot_binding_t *binding)
{
    const uint8_t *expected_digest = context;
    return storage_lock.held && binding->present && binding->package_present &&
           binding->slot == 1U && binding->firmware_sha256[0] == 0x22 &&
           memcmp(binding->package_sha256, expected_digest, 32) == 0;
}

static econtainer_slot_firmware_set_t two_firmware_set(uint8_t running)
{
    econtainer_slot_firmware_set_t result = {0};
    result.bootable_count = 2;
    memset(result.bootable_firmware_sha256[0], 0x11, 32);
    memset(result.bootable_firmware_sha256[1], 0x22, 32);
    memset(result.running_firmware_sha256, running, 32);
    return result;
}

static void initialize_two_package_bindings(econtainer_slots_idf_provider_t *provider)
{
    econtainer_slots_idf_config_t selected = config();
    assert(econtainer_slots_idf_bind(provider, &selected));
    econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT] = {0};
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        uint8_t *package = flash + index * SECTOR_BYTES;
        for (unsigned byte = 0; byte < 64U; ++byte) {
            package[byte] = (uint8_t)(index * 61U + byte * 3U);
        }
        bindings[index].present = true;
        memset(bindings[index].firmware_sha256, index == 0U ? 0x11 : 0x22, 32);
        bindings[index].package_present = true;
        bindings[index].slot = (uint8_t)index;
        assert(SHA256(package, 64U, bindings[index].package_sha256) != NULL);
        bindings[index].package_size_bytes = 64U;
        bindings[index].guest_abi_version = 2U;
        bindings[index].data_schema_version = 1U;
    }
    const econtainer_slot_firmware_set_t set = two_firmware_set(0x22);
    assert(econtainer_slots_initialize(&provider->io, &provider->geometry,
                                       &set, bindings) == ECONTAINER_SLOTS_OK);
}

static void test_provider_product_uninstall_and_rollback(void)
{
    econtainer_slots_idf_provider_t provider;
    initialize_two_package_bindings(&provider);
    const econtainer_slot_firmware_set_t running = two_firmware_set(0x22);
    const econtainer_slot_firmware_set_t fallback = two_firmware_set(0x11);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&provider.io, &provider.geometry, &state) == ECONTAINER_SLOTS_OK);
    uint8_t old_flash[PACKAGE_BYTES];
    memcpy(old_flash, flash, sizeof old_flash);
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES] = {0x43};
    uint8_t digest[32];
    assert(SHA256(flash + SECTOR_BYTES, 64U, digest) != NULL);
    const unsigned original_commits = commits;
    const unsigned original_erases = erases, original_writes = writes;
    assert(econtainer_slots_uninstall(&provider.io, &provider.geometry,
        state.sequence, &running, operation_id, digest,
        stopped_confirmed_instance, digest, &state) == ECONTAINER_SLOTS_OK);
    assert(commits == original_commits + 1U &&
           erases == original_erases && writes == original_writes &&
           memcmp(flash, old_flash, sizeof flash) == 0);
    assert(!state.bindings[1].package_present && state.bindings[0].package_present &&
           state.operation.kind == ECONTAINER_SLOT_NO_PACKAGE &&
           !state.operation.firmware_transition);

    /* A new provider handle after a simulated restart observes the committed
     * empty running binding and the intact rollback package. */
    econtainer_slots_idf_config_t selected = config();
    econtainer_slots_idf_provider_t restarted;
    assert(econtainer_slots_idf_bind(&restarted, &selected));
    assert(econtainer_slots_load(&restarted.io, &restarted.geometry,
                                 &state) == ECONTAINER_SLOTS_OK);
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    assert(econtainer_slots_reconcile(&restarted.io, &restarted.geometry,
        &running, &state, &decision) == ECONTAINER_SLOTS_OK &&
        decision == ECONTAINER_SLOT_BOOT_CONFIRMED);
    assert(econtainer_slots_reconcile(&restarted.io, &restarted.geometry,
        &fallback, &state, &decision) == ECONTAINER_SLOTS_OK &&
        decision == ECONTAINER_SLOT_BOOT_CONFIRMED &&
        state.bindings[0].package_present && state.bindings[0].slot == 0U);
    assert(memcmp(flash, old_flash, sizeof flash) == 0 &&
           erases == original_erases && writes == original_writes);
}

static void test_provider_uninstall_commit_uncertainty(void)
{
    econtainer_slots_idf_provider_t provider;
    initialize_two_package_bindings(&provider);
    const econtainer_slot_firmware_set_t running = two_firmware_set(0x22);
    econtainer_slots_state_t state;
    assert(econtainer_slots_load(&provider.io, &provider.geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t original_sequence = state.sequence;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES] = {0x44};
    uint8_t digest[32];
    assert(SHA256(flash + SECTOR_BYTES, 64U, digest) != NULL);
    fail_commit = true;
    assert(econtainer_slots_uninstall(&provider.io, &provider.geometry,
        original_sequence, &running, operation_id, digest,
        stopped_confirmed_instance, digest, &state) == ECONTAINER_SLOTS_IO_FAILED);
    assert(econtainer_slots_load(&provider.io, &provider.geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.sequence == original_sequence && state.bindings[1].package_present);
    commit_then_fail = true;
    assert(econtainer_slots_uninstall(&provider.io, &provider.geometry,
        original_sequence, &running, operation_id, digest,
        stopped_confirmed_instance, digest, &state) == ECONTAINER_SLOTS_UNCERTAIN);
    fail_commit = commit_then_fail = false;
    econtainer_slots_idf_config_t selected = config();
    econtainer_slots_idf_provider_t restarted;
    assert(econtainer_slots_idf_bind(&restarted, &selected));
    assert(econtainer_slots_load(&restarted.io, &restarted.geometry,
                                 &state) == ECONTAINER_SLOTS_OK);
    assert(state.sequence == original_sequence + 1U && !state.bindings[1].package_present &&
           state.bindings[0].package_present &&
           memcmp(state.operation.operation_id, operation_id, sizeof operation_id) == 0);
    assert(erases == 0U && writes == 0U);
}

int main(void)
{
    reset();
    test_binding_guards();
    reset();
    test_provider_io();
    reset();
    test_provider_mapping();
    reset();
    test_slot_engine_with_idf_provider();
    reset();
    test_provider_product_uninstall_and_rollback();
    reset();
    test_provider_uninstall_commit_uncertainty();
    reset();
    package_partition.encrypted = true;
    econtainer_slots_idf_config_t selected = config();
    econtainer_slots_idf_provider_t provider;
    assert(econtainer_slots_idf_bind(&provider, &selected));
    assert(provider.geometry.write_unit_bytes == 16U);
    puts("  IDF provider partition/map guards, exact offsets, unmap, Flash/NVS commit and slot engine passed");
}
