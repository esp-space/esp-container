#include "esp_container_slots_idf.h"

#include <stdint.h>
#include <string.h>

#include "nvs.h"

static bool name_valid(const char *value)
{
    if (value == NULL || value[0] < 'a' || value[0] > 'z') return false;
    for (size_t index = 1; index <= 15U; ++index) {
        const char character = value[index];
        if (character == '\0') return true;
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_')) return false;
    }
    return false;
}

static bool range_valid(const econtainer_slots_idf_provider_t *provider,
                        uint32_t offset_bytes, size_t size_bytes)
{
    const esp_partition_t *partition = provider->package_partition;
    return size_bytes > 0U && size_bytes <= partition->size &&
           offset_bytes >= partition->address &&
           offset_bytes - partition->address <= partition->size - size_bytes;
}

static bool provider_lock(void *context)
{
    econtainer_slots_idf_provider_t *provider = context;
    return xSemaphoreTake(provider->storage_lock, 0) == pdTRUE;
}

static void provider_unlock(void *context)
{
    econtainer_slots_idf_provider_t *provider = context;
    (void)xSemaphoreGive(provider->storage_lock);
}

static econtainer_slot_blob_result_t read_blob_locked(
    void *context, uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES])
{
    econtainer_slots_idf_provider_t *provider = context;
    nvs_handle_t handle;
    esp_err_t result = nvs_open_from_partition(provider->nvs_partition_label,
                                                provider->nvs_namespace,
                                                NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ECONTAINER_SLOT_BLOB_NOT_FOUND;
    if (result != ESP_OK) return ECONTAINER_SLOT_BLOB_READ_FAILED;
    size_t size_bytes = 0;
    result = nvs_get_blob(handle, provider->nvs_key, NULL, &size_bytes);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ECONTAINER_SLOT_BLOB_NOT_FOUND;
    }
    if (result != ESP_OK || size_bytes != ECONTAINER_SLOT_BLOB_BYTES) {
        nvs_close(handle);
        return ECONTAINER_SLOT_BLOB_READ_FAILED;
    }
    result = nvs_get_blob(handle, provider->nvs_key, blob, &size_bytes);
    nvs_close(handle);
    return result == ESP_OK && size_bytes == ECONTAINER_SLOT_BLOB_BYTES ?
           ECONTAINER_SLOT_BLOB_FOUND : ECONTAINER_SLOT_BLOB_READ_FAILED;
}

static econtainer_slot_blob_result_t provider_read_blob(
    void *context, uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES])
{
    econtainer_slots_idf_provider_t *provider = context;
    if (!provider->acquire_flash_io(provider->flash_io_context))
        return ECONTAINER_SLOT_BLOB_READ_FAILED;
    const econtainer_slot_blob_result_t result = read_blob_locked(context, blob);
    return provider->release_flash_io(provider->flash_io_context) ? result :
           ECONTAINER_SLOT_BLOB_READ_FAILED;
}

static bool write_blob_locked(void *context,
                                const uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES])
{
    econtainer_slots_idf_provider_t *provider = context;
    nvs_handle_t handle;
    if (nvs_open_from_partition(provider->nvs_partition_label,
                                provider->nvs_namespace,
                                NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(handle, provider->nvs_key, blob,
                                    ECONTAINER_SLOT_BLOB_BYTES);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

static bool provider_write_blob(void *context,
                                const uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES])
{
    econtainer_slots_idf_provider_t *provider = context;
    if (!provider->acquire_flash_io(provider->flash_io_context)) return false;
    const bool result = write_blob_locked(context, blob);
    return provider->release_flash_io(provider->flash_io_context) && result;
}

static bool provider_flash_read(void *context, uint32_t offset_bytes,
                                uint8_t *destination, size_t size_bytes)
{
    econtainer_slots_idf_provider_t *provider = context;
    if (destination == NULL || !range_valid(provider, offset_bytes, size_bytes) ||
        !provider->acquire_flash_io(provider->flash_io_context)) return false;
    const bool result = esp_partition_read(provider->package_partition,
        offset_bytes - provider->package_partition->address, destination, size_bytes) == ESP_OK;
    return provider->release_flash_io(provider->flash_io_context) && result;
}

static bool provider_flash_map(void *context, uint32_t offset_bytes,
                               size_t size_bytes, const uint8_t **mapped,
                               uintptr_t *handle)
{
    if (mapped != NULL) *mapped = NULL;
    if (handle != NULL) *handle = 0U;
    econtainer_slots_idf_provider_t *provider = context;
    if (mapped == NULL || handle == NULL || provider == NULL ||
        provider->package_partition == NULL ||
        !range_valid(provider, offset_bytes, size_bytes)) return false;

    const void *bytes = NULL;
    esp_partition_mmap_handle_t sdk_handle = 0;
    /* IDF adjusts the returned pointer to this exact, possibly unaligned
     * partition offset. Hold the caller's slot lock through unmap; this flag
     * also prevents cache-disabling Flash writes during the short load. */
    if (!provider->acquire_flash_io(provider->flash_io_context)) return false;
    if (esp_partition_mmap(provider->package_partition,
                            offset_bytes - provider->package_partition->address,
                            size_bytes,
                            ESP_PARTITION_MMAP_DATA | ESP_PARTITION_MMAP_BLOCKS_WRITE,
                            &bytes, &sdk_handle) != ESP_OK) {
        (void)provider->release_flash_io(provider->flash_io_context);
        return false;
    }
    if (bytes == NULL) {
        esp_partition_munmap(sdk_handle);
        (void)provider->release_flash_io(provider->flash_io_context);
        return false;
    }
    *mapped = bytes;
    *handle = (uintptr_t)sdk_handle;
    return true;
}

static bool provider_flash_unmap(void *context, uintptr_t handle)
{
    econtainer_slots_idf_provider_t *provider = context;
    esp_partition_munmap((esp_partition_mmap_handle_t)handle);
    return provider->release_flash_io(provider->flash_io_context);
}

static bool provider_flash_erase(void *context, uint32_t offset_bytes,
                                 uint32_t size_bytes)
{
    econtainer_slots_idf_provider_t *provider = context;
    const uint32_t erase_size = provider->geometry.erase_unit_bytes;
    if (!range_valid(provider, offset_bytes, size_bytes) ||
        offset_bytes % erase_size != 0U || size_bytes % erase_size != 0U ||
        !provider->acquire_flash_io(provider->flash_io_context)) return false;
    const bool result = esp_partition_erase_range(provider->package_partition,
        offset_bytes - provider->package_partition->address, size_bytes) == ESP_OK;
    return provider->release_flash_io(provider->flash_io_context) && result;
}

static bool provider_flash_write(void *context, uint32_t offset_bytes,
                                 const uint8_t *source, size_t size_bytes)
{
    econtainer_slots_idf_provider_t *provider = context;
    const uint32_t write_size = provider->geometry.write_unit_bytes;
    if (source == NULL || !range_valid(provider, offset_bytes, size_bytes) ||
        offset_bytes % write_size != 0U || size_bytes % write_size != 0U ||
        !provider->acquire_flash_io(provider->flash_io_context)) return false;
    const bool result = esp_partition_write(provider->package_partition,
        offset_bytes - provider->package_partition->address, source, size_bytes) == ESP_OK;
    return provider->release_flash_io(provider->flash_io_context) && result;
}

bool econtainer_slots_idf_bind(econtainer_slots_idf_provider_t *provider,
                               const econtainer_slots_idf_config_t *config)
{
    if (provider == NULL) return false;
    memset(provider, 0, sizeof *provider);
    if (config == NULL || config->storage_lock == NULL ||
        config->acquire_flash_io == NULL || config->release_flash_io == NULL ||
        !name_valid(config->package_partition_label) ||
        !name_valid(config->nvs_partition_label) ||
        !name_valid(config->nvs_namespace) || !name_valid(config->nvs_key) ||
        config->package_partition_offset_bytes == 0U ||
        config->package_partition_size_bytes == 0U ||
        config->nvs_partition_size_bytes == 0U) return false;

    const esp_partition_t *package = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
        config->package_partition_label);
    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
        config->nvs_partition_label);
    if (package == NULL || nvs == NULL || package == nvs ||
        package->readonly || nvs->readonly ||
        package->address != config->package_partition_offset_bytes ||
        package->size != config->package_partition_size_bytes ||
        nvs->address != config->nvs_partition_offset_bytes ||
        nvs->size != config->nvs_partition_size_bytes ||
        ((uint64_t)package->address < (uint64_t)nvs->address + nvs->size &&
         (uint64_t)nvs->address < (uint64_t)package->address + package->size) ||
        package->erase_size == 0U) return false;

    econtainer_slots_geometry_t geometry = {
        .partition_offset_bytes = package->address,
        .partition_size_bytes = package->size,
        .erase_unit_bytes = package->erase_size,
        .write_unit_bytes = package->encrypted ? 16U : 4U,
    };
    memcpy(geometry.slots, config->slots, sizeof geometry.slots);
    if (!econtainer_slots_geometry_valid(&geometry)) return false;

    provider->geometry = geometry;
    provider->package_partition = package;
    provider->storage_lock = config->storage_lock;
    provider->acquire_flash_io = config->acquire_flash_io;
    provider->release_flash_io = config->release_flash_io;
    provider->flash_io_context = config->flash_io_context;
    memcpy(provider->nvs_partition_label, config->nvs_partition_label,
           strlen(config->nvs_partition_label) + 1U);
    memcpy(provider->nvs_namespace, config->nvs_namespace,
           strlen(config->nvs_namespace) + 1U);
    memcpy(provider->nvs_key, config->nvs_key, strlen(config->nvs_key) + 1U);
    provider->io = (econtainer_slots_io_t){
        .lock = provider_lock,
        .unlock = provider_unlock,
        .read_blob = provider_read_blob,
        .write_blob = provider_write_blob,
        .flash_read = provider_flash_read,
        .flash_map = provider_flash_map,
        .flash_unmap = provider_flash_unmap,
        .flash_erase = provider_flash_erase,
        .flash_write = provider_flash_write,
        .context = provider,
    };
    return true;
}
