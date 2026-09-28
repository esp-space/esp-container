#ifndef ESP_CONTAINER_SLOTS_IDF_H
#define ESP_CONTAINER_SLOTS_IDF_H

#include "esp_container_slots.h"

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The product supplies exact, already approved partition-table facts. This
 * provider neither creates a partition nor initializes/erases NVS. */
typedef struct {
    const char *package_partition_label;
    uint32_t package_partition_offset_bytes;
    uint32_t package_partition_size_bytes;
    econtainer_slot_region_t slots[ECONTAINER_SLOT_COUNT];
    const char *nvs_partition_label;
    uint32_t nvs_partition_offset_bytes;
    uint32_t nvs_partition_size_bytes;
    const char *nvs_namespace;
    const char *nvs_key;
    SemaphoreHandle_t storage_lock;
    /* Physical I/O lease supplied by the product assembly. The slot lock
     * above is independent; a mapping retains this lease until unmap. */
    bool (*acquire_flash_io)(void *context);
    void (*release_flash_io)(void *context);
    void *flash_io_context;
} econtainer_slots_idf_config_t;

/* Keep this object at a stable address while its io callbacks are in use. */
typedef struct {
    econtainer_slots_io_t io;
    econtainer_slots_geometry_t geometry;
    const esp_partition_t *package_partition;
    SemaphoreHandle_t storage_lock;
    bool (*acquire_flash_io)(void *context);
    void (*release_flash_io)(void *context);
    void *flash_io_context;
    char nvs_partition_label[16];
    char nvs_namespace[16];
    char nvs_key[16];
} econtainer_slots_idf_provider_t;

/* Returns false with an empty provider unless the real table contains a
 * writable dedicated data/undefined package partition and exact NVS target. */
bool econtainer_slots_idf_bind(econtainer_slots_idf_provider_t *provider,
                               const econtainer_slots_idf_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
