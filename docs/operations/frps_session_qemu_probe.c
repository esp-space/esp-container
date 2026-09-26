// SPDX-License-Identifier: Apache-2.0
// QEMU-only public FRP client lifecycle while Base and ABI 2 guest stay live.
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "esp_frp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qemu_frps_config.h"

extern const uint8_t frps_ca_pem_start[] asm("_binary_frps_ca_pem_start");
extern const uint8_t frps_ca_pem_end[] asm("_binary_frps_ca_pem_end");

static const char *const TAG = "five-capacity-frps";
void capacity_openeth_report_heap(const char *phase, int error);

static bool trusted_time(void *context)
{
    (void)context;
    return time(NULL) >= QEMU_FRPS_EPOCH;
}

static void report_status(const char *phase, efrp_result_t result,
                          const efrp_status_t *status)
{
    ESP_LOGI(TAG,
             "frps phase=%s api=%d client_phase=%d failure_phase=%d error=%d attempts=%u ready=%u pongs=%u tls_error=%d verify=%u system_error=%d free=%u largest=%u min=%u",
             phase, (int)result, status ? (int)status->phase : -1,
             status ? (int)status->failure_phase : -1,
             status ? (int)status->error : -1,
             status ? (unsigned)status->attempts : 0,
             status ? (unsigned)status->ready_sessions : 0,
             status ? (unsigned)status->pongs : 0,
             status ? status->tls_error : 0,
             status ? (unsigned)status->tls_verify_flags : 0,
             status ? status->system_error : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    capacity_openeth_report_heap(phase, (int)result);
}

bool capacity_frps_session_probe(void)
{
    const size_t ca_total = (size_t)(frps_ca_pem_end - frps_ca_pem_start);
    if (ca_total < 2 || frps_ca_pem_start[ca_total - 1] != 0) {
        report_status("ca_invalid", EFRP_INVALID_ARGUMENT, NULL);
        return false;
    }
    const struct timeval simulated = {.tv_sec = QEMU_FRPS_EPOCH, .tv_usec = 0};
    if (settimeofday(&simulated, NULL) != 0 || !trusted_time(NULL)) {
        report_status("clock_invalid", EFRP_TIME_UNTRUSTED, NULL);
        return false;
    }
    report_status("before_create", EFRP_OK, NULL);
    static const uint8_t token[] = "qemu-frps-test-token";
    const efrp_config_t config = {
        .server_hostname = "10.0.2.2",
        .server_port = QEMU_FRPS_PORT,
        .ca_pem = frps_ca_pem_start,
        .ca_length = ca_total - 1,
        .token = token,
        .token_length = sizeof token - 1,
        .hostname = "qemu-c3",
        .user = "qemu",
        .client_id = "qemu-c3",
        .proxy_name = "qemu-c3",
        .local_ipv4 = {127, 0, 0, 1},
        .local_port = 8765,
        .time_is_trusted = trusted_time,
    };
    efrp_client_t *client = NULL;
    efrp_result_t result = efrp_create(&config, &client);
    efrp_status_t status = {0};
    if (client) (void)efrp_get_status(client, &status);
    report_status("created", result, client ? &status : NULL);
    if (result != EFRP_OK) return false;
    result = efrp_start(client);
    (void)efrp_get_status(client, &status);
    report_status("start_queued", result, &status);
    if (result == EFRP_OK) {
        efrp_phase_t observed = status.phase;
        for (unsigned waited_ms = 0; waited_ms < 20000; waited_ms += 20) {
            vTaskDelay(pdMS_TO_TICKS(20));
            result = efrp_get_status(client, &status);
            if (result != EFRP_OK) break;
            if (status.phase != observed) {
                observed = status.phase;
                report_status("phase_changed", result, &status);
            }
            if (status.phase == EFRP_PHASE_READY || status.phase == EFRP_PHASE_FAILED) break;
        }
    }
    const bool ready = result == EFRP_OK && status.phase == EFRP_PHASE_READY &&
        status.ready_sessions == 1 && status.pongs >= 1;
    report_status("result", result, &status);
    const efrp_result_t destroyed = efrp_destroy(&client, 5000);
    report_status("destroy", destroyed, NULL);
    return ready && destroyed == EFRP_OK && client == NULL;
}
