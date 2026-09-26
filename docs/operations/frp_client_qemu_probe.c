// SPDX-License-Identifier: Apache-2.0
// QEMU-only, copied into an external ESP Base capacity probe. Never flash it.
#include <stdbool.h>
#include <stdint.h>

#include "esp_frp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *const TAG = "five-capacity-client";
static efrp_client_t *s_client;

static bool time_untrusted(void *context)
{
    (void)context;
    return false;
}

static unsigned free_bytes(void)
{
    return (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

static unsigned largest_bytes(void)
{
    return (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

static unsigned minimum_bytes(void)
{
    return (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

bool capacity_frp_client_create(void)
{
    // Placeholder bytes remain in RAM. No connection is started or attempted.
    static const uint8_t ca[] = "qemu-placeholder-ca";
    static const uint8_t token[] = "qemu-placeholder-token";
    const efrp_config_t config = {
        .server_hostname = "frp.example.invalid",
        .server_port = 7000,
        .ca_pem = ca,
        .ca_length = sizeof ca - 1,
        .token = token,
        .token_length = sizeof token - 1,
        .hostname = "qemu-c3",
        .user = "qemu",
        .client_id = "qemu-c3",
        .proxy_name = "qemu-c3",
        .local_ipv4 = {127, 0, 0, 1},
        .local_port = 8765,
        .time_is_trusted = time_untrusted,
    };
    const unsigned before_free = free_bytes();
    const unsigned before_largest = largest_bytes();
    const unsigned before_min = minimum_bytes();
    const efrp_result_t created = efrp_create(&config, &s_client);
    if (created == EFRP_OK) vTaskDelay(1);
    TaskHandle_t worker = created == EFRP_OK ? xTaskGetHandle("esp_frp") : NULL;
    const unsigned worker_stack_min_free = worker ?
        (unsigned)uxTaskGetStackHighWaterMark(worker) : 0;
    efrp_status_t status = {0};
    const efrp_result_t checked = created == EFRP_OK ?
        efrp_get_status(s_client, &status) : EFRP_INVALID_STATE;
    ESP_LOGI(TAG,
             "frp_client stage=create created=%d status=%d phase=%d handle=%d worker=%d worker_stack_min_free=%u before=%u/%u/%u with=%u/%u/%u delta_free=%d",
             (int)created, (int)checked, (int)status.phase, s_client != NULL,
             worker != NULL, worker_stack_min_free,
             before_free, before_largest, before_min,
             free_bytes(), largest_bytes(), minimum_bytes(),
             (int)before_free - (int)free_bytes());
    return created == EFRP_OK && checked == EFRP_OK &&
        status.phase == EFRP_PHASE_STOPPED && s_client != NULL && worker != NULL;
}

bool capacity_frp_client_destroy(void)
{
    const unsigned before_free = free_bytes();
    const unsigned before_largest = largest_bytes();
    const unsigned before_min = minimum_bytes();
    TaskHandle_t worker = s_client ? xTaskGetHandle("esp_frp") : NULL;
    const unsigned worker_stack_min_free = worker ?
        (unsigned)uxTaskGetStackHighWaterMark(worker) : 0;
    const efrp_result_t destroyed = efrp_destroy(&s_client, 5000);
    ESP_LOGI(TAG,
             "frp_client stage=destroy destroyed=%d handle=%d worker_stack_min_free=%u before=%u/%u/%u after=%u/%u/%u delta_free=%d",
             (int)destroyed, s_client != NULL, worker_stack_min_free,
             before_free, before_largest, before_min,
             free_bytes(), largest_bytes(), minimum_bytes(),
             (int)free_bytes() - (int)before_free);
    return destroyed == EFRP_OK && s_client == NULL;
}
