#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_container.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bh_platform.h"
#include "wasm_export.h"

#if WASM_ENABLE_INTERP != 1 || WASM_ENABLE_FAST_INTERP != 0 || \
    WASM_ENABLE_AOT != 0 || WASM_ENABLE_INSTRUCTION_METERING != 1 || \
    WASM_ENABLE_LIBC_WASI != 0 || WASM_ENABLE_LIB_PTHREAD != 0 || \
    WASM_ENABLE_BULK_MEMORY != 0 || \
    WASM_ENABLE_CLASSIC_WALL_CLOCK_LIMIT != 1
#error "The runtime probe requires the bounded WAMR Classic profile"
#endif

static const char *const TAG = "container-probe";

/* Freestanding standard Wasm v1 fixtures: () -> i32, no imports/start. */
static const uint8_t return_zero[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x00, 0x0b,
};
static const uint8_t endless_loop[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x0b, 0x01, 0x07, 0x6c, 0x6f, 0x6f, 0x70, 0x69, 0x6e, 0x67, 0x00, 0x00,
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b,
};

static void report_heap(const char *phase)
{
    ESP_LOGI(TAG, "%s free=%u largest=%u", phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

typedef enum {
    EXPECT_RETURN,
    EXPECT_INSTRUCTION_LIMIT,
    EXPECT_WALL_CLOCK_DEADLINE,
} probe_expectation_t;

static bool exercise(const uint8_t *bytes, uint32_t size, const char *export_name,
                     probe_expectation_t expectation)
{
    char error[128] = {0};
    /* WAMR may rewrite its mutable input while loading. Each call owns a fresh
     * copy, so the same fixture can exercise both budget failure paths. */
    uint8_t *module_bytes = malloc(size);
    if (module_bytes == NULL) {
        ESP_LOGE(TAG, "module copy allocation failed for %s", export_name);
        return false;
    }
    memcpy(module_bytes, bytes, size);
    if (econtainer_wasm_check(module_bytes, size) != ECONTAINER_WASM_OK) {
        ESP_LOGE(TAG, "module scan rejected %s", export_name);
        free(module_bytes);
        return false;
    }
    wasm_module_t module = wasm_runtime_load(module_bytes, size, error, sizeof(error));
    if (!module) {
        ESP_LOGE(TAG, "load %s: %s", export_name, error);
        free(module_bytes);
        return false;
    }
    wasm_module_inst_t instance = wasm_runtime_instantiate(module, 4096, 0, error, sizeof(error));
    if (!instance) {
        ESP_LOGE(TAG, "instantiate %s: %s", export_name, error);
        wasm_runtime_unload(module);
        free(module_bytes);
        return false;
    }
    wasm_exec_env_t environment = wasm_runtime_create_exec_env(instance, 4096);
    wasm_function_inst_t function = wasm_runtime_lookup_function(instance, export_name);
    bool passed = false;
    if (environment && function) {
        uint32_t result[1] = {UINT32_MAX};
        const bool deadline_probe = expectation == EXPECT_WALL_CLOCK_DEADLINE;
        wasm_runtime_set_instruction_count_limit(
            environment, deadline_probe ? INT32_MAX : 1000);
        const int64_t began_us = deadline_probe ? esp_timer_get_time() : 0;
        if (deadline_probe && (began_us < 0 || began_us > INT64_MAX - 20000)) {
            ESP_LOGE(TAG, "monotonic clock cannot represent the probe deadline");
        } else {
            if (deadline_probe) {
                wasm_runtime_set_classic_wall_clock_deadline_us(
                    environment, (uint64_t)began_us + 20000U);
            }
            const bool call_ok = wasm_runtime_call_wasm(environment, function, 0, result);
            if (deadline_probe) {
                wasm_runtime_set_classic_wall_clock_deadline_us(environment, 0);
            }
            const int64_t elapsed_us = deadline_probe ? esp_timer_get_time() - began_us : 0;
            const char *exception = wasm_runtime_get_exception(instance);
            switch (expectation) {
            case EXPECT_RETURN:
                passed = call_ok && result[0] == 0 && exception == NULL;
                break;
            case EXPECT_INSTRUCTION_LIMIT:
                passed = !call_ok && exception != NULL &&
                         strcmp(exception, "Exception: instruction limit exceeded") == 0;
                break;
            case EXPECT_WALL_CLOCK_DEADLINE:
                passed = !call_ok && exception != NULL &&
                         strcmp(exception, "Exception: wall clock deadline exceeded") == 0;
                break;
            }
            ESP_LOGI(TAG, "%s mode=%d call_ok=%d result=%u elapsed_us=%lld exception=%s",
                     export_name, (int)expectation, (int)call_ok, (unsigned)result[0],
                     (long long)elapsed_us, exception ? exception : "none");
        }
    }
    if (environment) {
        wasm_runtime_destroy_exec_env(environment);
    }
    wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(module);
    free(module_bytes);
    return passed;
}

static void *probe_thread(void *unused)
{
    (void)unused;
    RuntimeInitArgs args;
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = (void *)os_malloc;
    args.mem_alloc_option.allocator.realloc_func = (void *)os_realloc;
    args.mem_alloc_option.allocator.free_func = (void *)os_free;
    report_heap("before");
    if (!wasm_runtime_full_init(&args)) {
        ESP_LOGE(TAG, "WAMR initialization failed");
        return NULL;
    }
    const bool normal = exercise(return_zero, sizeof(return_zero), "run", EXPECT_RETURN);
    const bool bounded = exercise(endless_loop, sizeof(endless_loop), "looping",
                                  EXPECT_INSTRUCTION_LIMIT);
    const bool deadline = exercise(endless_loop, sizeof(endless_loop), "looping",
                                   EXPECT_WALL_CLOCK_DEADLINE);
    wasm_runtime_destroy();
    report_heap("after");
    ESP_LOGI(TAG, "normal=%d instruction_limit=%d wall_clock_deadline=%d",
             (int)normal, (int)bounded, (int)deadline);
    return NULL;
}

void app_main(void)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        ESP_LOGE(TAG, "probe thread attributes initialization failed");
        return;
    }
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_JOINABLE) != 0 ||
        pthread_attr_setstacksize(&attributes, 8192) != 0) {
        ESP_LOGE(TAG, "probe thread configuration failed");
        pthread_attr_destroy(&attributes);
        return;
    }

    pthread_t thread;
    const int create_result = pthread_create(&thread, &attributes, probe_thread, NULL);
    pthread_attr_destroy(&attributes);
    if (create_result != 0) {
        ESP_LOGE(TAG, "probe thread creation failed: %d", create_result);
        return;
    }
    const int join_result = pthread_join(thread, NULL);
    if (join_result != 0) {
        ESP_LOGE(TAG, "probe thread join failed: %d", join_result);
    }
}
