#include "runtime_internal.h"
#include "esp_container.h"

#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#ifdef ECONTAINER_TEST_RESOURCE_STATS
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/mman.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static const econtainer_runtime_limits_t limits = {
    .max_wasm_bytes = 512 * 1024,
    .max_memory_pages = 1,
    .stack_size_bytes = 4096,
    .max_event_bytes = 128,
    .init_instruction_budget = 1000,
    .event_instruction_budget = 1000,
    .stop_instruction_budget = 1000,
    .max_entry_duration_ms = 1000,
};

#ifdef ECONTAINER_TEST_RESOURCE_STATS
typedef struct {
    size_t malloc_bytes;
    mach_vm_size_t virtual_bytes;
    integer_t regions;
} resource_stats_t;

static bool sample_resources(resource_stats_t *sample)
{
    malloc_statistics_t statistics = {0};
    malloc_zone_statistics(malloc_default_zone(), &statistics);
    task_vm_info_data_t vm = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) !=
        KERN_SUCCESS) return false;
    sample->malloc_bytes = statistics.size_in_use;
    sample->virtual_bytes = vm.virtual_size;
    sample->regions = vm.region_count;
    return sample->malloc_bytes > 0 && sample->virtual_bytes > 0 && sample->regions > 0;
}

static bool resource_probes_calibrated(void)
{
    resource_stats_t before = {0}, allocated = {0};
    if (!sample_resources(&before)) return false;
    void *heap = malloc(65536);
    if (heap == NULL) return false;
    memset(heap, 0x5a, 65536);
    const bool heap_visible = sample_resources(&allocated) &&
                              allocated.malloc_bytes >= before.malloc_bytes + 65536;
    free(heap);
    if (!heap_visible || !sample_resources(&before)) return false;
    void *mapping = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED) return false;
    const bool mapping_visible = sample_resources(&allocated) &&
                                 allocated.virtual_bytes >= before.virtual_bytes + 65536;
    return munmap(mapping, 65536) == 0 && mapping_visible;
}

static bool resources_stable(const char *label, resource_stats_t after_ten,
                             resource_stats_t after_fifty, resource_stats_t after_hundred)
{
    fprintf(stderr, "%s resources 10/50/100: malloc=%zu/%zu/%zu "
            "virtual=%llu/%llu/%llu regions=%d/%d/%d\n", label,
            after_ten.malloc_bytes, after_fifty.malloc_bytes,
            after_hundred.malloc_bytes,
            (unsigned long long)after_ten.virtual_bytes,
            (unsigned long long)after_fifty.virtual_bytes,
            (unsigned long long)after_hundred.virtual_bytes,
            after_ten.regions, after_fifty.regions, after_hundred.regions);
    /* Existing mappings may split; retained heap/address space must not grow. */
    return after_ten.malloc_bytes == after_fifty.malloc_bytes &&
           after_fifty.malloc_bytes == after_hundred.malloc_bytes &&
           after_ten.virtual_bytes == after_fifty.virtual_bytes &&
           after_fifty.virtual_bytes == after_hundred.virtual_bytes &&
           after_hundred.regions <= after_fifty.regions;
}
#endif

static uint8_t *read_file(const char *path, size_t *size_bytes)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    const long length = ftell(file);
    if (length < 8 || length > 512 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *bytes = malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_bytes = (size_t)length;
    return bytes;
}

static bool test_counter(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_t *runtime = NULL;
    econtainer_runtime_t *other = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_LOADED);
    CHECK(econtainer_runtime_open(bytes, length, &limits, &other) == ECONTAINER_RUNTIME_BUSY);
    CHECK(other == NULL);
    memset(bytes, 0, length); /* WAMR must retain owned code/data after open. */
    free(bytes);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_INVALID_STATE);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_RUNNING);
    int32_t result = -1;
    const uint8_t first[] = {1, 2, 3};
    const uint8_t second[] = {4, 5};
    CHECK(econtainer_runtime_on_event(runtime, first, sizeof(first), &result) == ECONTAINER_RUNTIME_OK);
    CHECK(result == 3);
    CHECK(econtainer_runtime_on_event(runtime, second, sizeof(second), &result) == ECONTAINER_RUNTIME_OK);
    CHECK(result == 5);
    CHECK(econtainer_runtime_on_event(runtime, NULL, 1, &result) == ECONTAINER_RUNTIME_INVALID_INPUT);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_RUNNING);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_STOPPED);
    CHECK(econtainer_runtime_on_event(runtime, first, sizeof(first), &result) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    econtainer_runtime_close(&runtime);
    CHECK(runtime == NULL);
    econtainer_runtime_close(&runtime);
    return true;
}

#ifndef _WIN32
static bool has_nonempty_data_section(const uint8_t *wasm, size_t size_bytes)
{
    size_t cursor = 8;
    while (cursor < size_bytes) {
        const uint8_t id = wasm[cursor++];
        size_t body_size_bytes = 0;
        unsigned shift = 0;
        uint8_t byte = 0;
        do {
            if (cursor >= size_bytes || shift >= 35) return false;
            byte = wasm[cursor++];
            body_size_bytes |= (size_t)(byte & 0x7fU) << shift;
            shift += 7;
        } while ((byte & 0x80U) != 0);
        if (body_size_bytes > size_bytes - cursor) return false;
        if (id == 11) return body_size_bytes > 1;
        cursor += body_size_bytes;
    }
    return false;
}

static bool test_readonly_data_and_import(const char *path)
{
    size_t wasm_size_bytes = 0;
    uint8_t *wasm = read_file(path, &wasm_size_bytes);
    CHECK(wasm != NULL && has_nonempty_data_section(wasm, wasm_size_bytes));
    uint8_t *mapped = mmap(NULL, wasm_size_bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapped != MAP_FAILED);
    memcpy(mapped, wasm, wasm_size_bytes);
    free(wasm);
    CHECK(mprotect(mapped, wasm_size_bytes, PROT_READ) == 0);
    econtainer_runtime_limits_t authorized = limits;
    authorized.allowed_capabilities = ECONTAINER_CAP_MONOTONIC_TIME | ECONTAINER_CAP_LOG;
    authorized.max_log_bytes = 16;
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(mapped, wasm_size_bytes, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(munmap(mapped, wasm_size_bytes) == 0);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    uint8_t log[16] = {0};
    size_t log_size = 0;
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
          ECONTAINER_RUNTIME_OK && log_size == 4 && memcmp(log, "init", 4) == 0);
    const uint8_t event[] = {2};
    int32_t result = -1;
    CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
          ECONTAINER_RUNTIME_OK && result == -2);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
          ECONTAINER_RUNTIME_OK && log_size == 5 && memcmp(log, "first", 5) == 0);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
    return true;
}

static bool test_readonly_flash_sized_wasm(const char *path)
{
    size_t counter_size_bytes = 0;
    uint8_t *counter = read_file(path, &counter_size_bytes);
    CHECK(counter != NULL);
    const size_t wasm_size_bytes = 373U * 1024U;
    CHECK(counter_size_bytes + 12U < wasm_size_bytes);
    uint8_t *mapped = mmap(NULL, wasm_size_bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapped != MAP_FAILED);
    memcpy(mapped, counter, counter_size_bytes);
    free(counter);
    /* A custom section fills the Flash geometry upper bound. WAMR ignores
     * this section under the fixed Classic profile. */
    const uint32_t body_size_bytes = (uint32_t)(wasm_size_bytes - counter_size_bytes - 4U);
    size_t cursor = counter_size_bytes;
    mapped[cursor++] = 0;
    mapped[cursor++] = (uint8_t)((body_size_bytes & 0x7fU) | 0x80U);
    mapped[cursor++] = (uint8_t)(((body_size_bytes >> 7) & 0x7fU) | 0x80U);
    mapped[cursor++] = (uint8_t)(body_size_bytes >> 14);
    mapped[cursor++] = 7;
    memcpy(mapped + cursor, "padding", 7);
    cursor += 7;
    memset(mapped + cursor, 0, wasm_size_bytes - cursor);
    CHECK(econtainer_wasm_check(mapped, wasm_size_bytes) == ECONTAINER_WASM_OK);
    CHECK(mprotect(mapped, wasm_size_bytes, PROT_READ) == 0);
    econtainer_runtime_t *runtime = NULL;
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    resource_stats_t before = {0}, after = {0};
    CHECK(sample_resources(&before));
#endif
    CHECK(econtainer_runtime_open(mapped, wasm_size_bytes, &limits, &runtime) ==
          ECONTAINER_RUNTIME_OK);
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    CHECK(sample_resources(&after));
    fprintf(stderr, "373 KiB padded Wasm open malloc delta=%zu bytes\n",
            after.malloc_bytes - before.malloc_bytes);
#endif
    CHECK(munmap(mapped, wasm_size_bytes) == 0);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    int32_t result = -1;
    const uint8_t event[] = {1, 2, 3};
    CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
          ECONTAINER_RUNTIME_OK && result == 3);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
    return true;
}

static bool test_malformed_sections(const char *path)
{
    size_t wasm_size_bytes = 0;
    uint8_t *counter = read_file(path, &wasm_size_bytes);
    CHECK(counter != NULL);
    uint8_t *bad = malloc(wasm_size_bytes + 4U);
    CHECK(bad != NULL);
    memcpy(bad, counter, wasm_size_bytes);
    free(counter);
    econtainer_runtime_t *runtime = NULL;
    bad[wasm_size_bytes] = 0; /* custom section */
    bad[wasm_size_bytes + 1] = 2; /* body length */
    bad[wasm_size_bytes + 2] = 1; /* name length */
    bad[wasm_size_bytes + 3] = 0xff; /* invalid UTF-8 */
    CHECK(econtainer_wasm_check(bad, wasm_size_bytes + 4U) == ECONTAINER_WASM_OK);
    CHECK(econtainer_runtime_open(bad, wasm_size_bytes + 4U, &limits, &runtime) ==
          ECONTAINER_RUNTIME_BAD_WASM && runtime == NULL);
    bad[wasm_size_bytes + 1] = 5; /* body exceeds available bytes */
    CHECK(econtainer_runtime_open(bad, wasm_size_bytes + 4U, &limits, &runtime) ==
          ECONTAINER_RUNTIME_BAD_WASM && runtime == NULL);
    free(bad);
    return true;
}
#endif

static bool test_event_copy(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_OK);
    free(bytes);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    const uint8_t event[] = {2, 3, 5};
    int32_t result = -1;
    CHECK(econtainer_runtime_on_event(runtime, event, sizeof(event), &result) == ECONTAINER_RUNTIME_OK);
    CHECK(result == 10);
    CHECK(econtainer_runtime_on_event(runtime, NULL, 0, &result) == ECONTAINER_RUNTIME_OK);
    CHECK(result == -1); /* Guest result does not become a host runtime failure. */
    uint8_t oversized[129] = {0};
    CHECK(econtainer_runtime_on_event(runtime, oversized, sizeof(oversized), &result) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_RUNNING);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    econtainer_runtime_close(&runtime);
    return true;
}

static bool test_loop(const char *path, unsigned entry)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_OK);
    free(bytes);
    if (entry == 0) {
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_INSTRUCTION_LIMIT);
    }
    else {
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        if (entry == 1) {
            const uint8_t event[] = {1};
            int32_t result = -1;
            CHECK(econtainer_runtime_on_event(runtime, event, sizeof(event), &result) ==
                  ECONTAINER_RUNTIME_INSTRUCTION_LIMIT);
        }
        else {
            CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_INSTRUCTION_LIMIT);
        }
    }
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_INVALID_STATE);
    econtainer_runtime_close(&runtime);
    CHECK(runtime == NULL);
    return true;
}

static bool test_pure_guest_deadline(const char *path, const char *counter_path,
                                     unsigned entry)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_limits_t short_entry = limits;
    short_entry.max_entry_duration_ms = 20;
    short_entry.init_instruction_budget = INT32_MAX;
    short_entry.event_instruction_budget = INT32_MAX;
    short_entry.stop_instruction_budget = INT32_MAX;
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &short_entry, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    free(bytes);
    if (entry == 0) {
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_ENTRY_EXPIRED);
    }
    else {
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        if (entry == 1) {
            const uint8_t event[] = {1};
            int32_t guest_result = 123;
            CHECK(econtainer_runtime_on_event(runtime, event, sizeof event,
                                               &guest_result) ==
                  ECONTAINER_RUNTIME_ENTRY_EXPIRED);
            CHECK(guest_result == 123);
        }
        else {
            CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_ENTRY_EXPIRED);
        }
    }
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK &&
          runtime == NULL);

    bytes = read_file(counter_path, &length);
    CHECK(bytes != NULL);
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    free(bytes);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK &&
          runtime == NULL);
    return true;
}

typedef struct {
    atomic_bool entered;
    atomic_bool requested;
    uint64_t requested_us;
} cancellation_test_t;

static uint64_t test_monotonic_us(void)
{
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static bool test_cancel_requested(void *context)
{
    cancellation_test_t *cancel = context;
    atomic_store_explicit(&cancel->entered, true, memory_order_release);
    return atomic_load_explicit(&cancel->requested, memory_order_acquire);
}

static void *request_cancellation(void *context)
{
    cancellation_test_t *cancel = context;
    const uint64_t began_us = test_monotonic_us();
    const struct timespec poll = {.tv_nsec = 1000000};
    while (!atomic_load_explicit(&cancel->entered, memory_order_acquire) &&
           test_monotonic_us() - began_us < UINT64_C(500000))
        nanosleep(&poll, NULL);
    const struct timespec delay = {.tv_nsec = 10000000};
    nanosleep(&delay, NULL);
    cancel->requested_us = test_monotonic_us();
    atomic_store_explicit(&cancel->requested, true, memory_order_release);
    return NULL;
}

static bool test_async_cancellation(const char *path, unsigned entry,
                                     bool imports)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    cancellation_test_t cancel = {0};
    atomic_init(&cancel.entered, false);
    atomic_init(&cancel.requested, false);
    econtainer_runtime_limits_t cancellable = limits;
    cancellable.init_instruction_budget = INT32_MAX;
    cancellable.event_instruction_budget = INT32_MAX;
    cancellable.max_entry_duration_ms = 500;
    cancellable.cancel_requested = test_cancel_requested;
    cancellable.cancel_context = &cancel;
    if (imports) {
        cancellable.allowed_capabilities = ECONTAINER_CAP_MONOTONIC_TIME |
                                            ECONTAINER_CAP_LOG | ECONTAINER_CAP_TIMER;
        cancellable.max_log_bytes = 16;
        cancellable.max_timers = 1;
    }
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &cancellable, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    free(bytes);
    int32_t result = 123;
    if (entry != 0) {
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        if (imports) {
            const uint8_t timer[] = {'T'}, prior[] = {'P'};
            CHECK(econtainer_runtime_on_event(runtime, timer, sizeof timer, &result) ==
                  ECONTAINER_RUNTIME_OK);
            CHECK(econtainer_runtime_on_event(runtime, prior, sizeof prior, &result) ==
                  ECONTAINER_RUNTIME_OK);
        }
    }
    atomic_store_explicit(&cancel.entered, false, memory_order_release);
    pthread_t requester;
    CHECK(pthread_create(&requester, NULL, request_cancellation, &cancel) == 0);
    const uint8_t event[] = {imports ? 'R' : 1};
    result = 123;
    const econtainer_runtime_result_t status = entry == 0
        ? econtainer_runtime_init(runtime)
        : econtainer_runtime_on_event(runtime, event, sizeof event, &result);
    const uint64_t returned_us = test_monotonic_us();
    CHECK(pthread_join(requester, NULL) == 0);
    CHECK(status == ECONTAINER_RUNTIME_ENTRY_CANCELLED);
    CHECK(cancel.requested_us > 0 && returned_us >= cancel.requested_us &&
          returned_us - cancel.requested_us < UINT64_C(250000));
    CHECK(result == 123);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
    CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    if (imports) {
        uint8_t log[16] = {0};
        size_t log_size = 99;
        CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
              ECONTAINER_RUNTIME_OK && log_size == 5 &&
              memcmp(log, "prior", log_size) == 0);
        uint64_t deadline_ms = 99;
        econtainer_timer_event_t timer_event = {0};
        CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline_ms) ==
              ECONTAINER_RUNTIME_INVALID_STATE);
        CHECK(econtainer_runtime_poll_timer(runtime, &timer_event, &result) ==
              ECONTAINER_RUNTIME_INVALID_STATE);
    }
    /* A request still set must not bypass the actual guest stop. */
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_STOPPED);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    fprintf(stderr, "async cancel entry=%u imports=%u latency_us=%llu\n",
            entry, imports, (unsigned long long)(returned_us - cancel.requested_us));
    return true;
}

static bool test_repeated_cancel_release(const char *path)
{
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    resource_stats_t after_ten = {0}, after_fifty = {0}, after_hundred = {0};
#endif
    for (unsigned cycle = 1; cycle <= 100; ++cycle) {
        CHECK(test_async_cancellation(path, 1, false));
#ifdef ECONTAINER_TEST_RESOURCE_STATS
        if (cycle == 10) CHECK(sample_resources(&after_ten));
        if (cycle == 50) CHECK(sample_resources(&after_fifty));
        if (cycle == 100) CHECK(sample_resources(&after_hundred));
#endif
    }
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    CHECK(resources_stable("cancel", after_ten, after_fifty, after_hundred));
#endif
    return true;
}

static bool test_cancel_stop_failures(const char *loop_path, const char *fail_path)
{
    cancellation_test_t cancel = {0};
    atomic_init(&cancel.entered, false);
    atomic_init(&cancel.requested, false);
    econtainer_runtime_limits_t cancellable = limits;
    cancellable.cancel_requested = test_cancel_requested;
    cancellable.cancel_context = &cancel;
    cancellable.stop_instruction_budget = INT32_MAX;
    cancellable.max_entry_duration_ms = 20;
    const char *paths[] = {loop_path, fail_path};
    for (size_t index = 0; index < 2; ++index) {
        size_t length = 0;
        uint8_t *bytes = read_file(paths[index], &length);
        CHECK(bytes != NULL);
        econtainer_runtime_t *runtime = NULL;
        atomic_store(&cancel.requested, false);
        CHECK(econtainer_runtime_open(bytes, length, &cancellable, &runtime) ==
              ECONTAINER_RUNTIME_OK);
        free(bytes);
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        atomic_store(&cancel.requested, true);
        int32_t result = 123;
        const uint8_t event[] = {1};
        CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
              ECONTAINER_RUNTIME_ENTRY_CANCELLED && result == 123);
        CHECK(econtainer_runtime_stop(runtime) == (index == 0
              ? ECONTAINER_RUNTIME_ENTRY_EXPIRED : ECONTAINER_RUNTIME_GUEST_FAILURE));
        CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
        CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_INVALID_STATE);
        CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    }
    return true;
}

static bool test_maximum_event(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_limits_t pressure = limits;
    pressure.max_event_bytes = ECONTAINER_EVENT_BUFFER_BYTES;
    pressure.event_instruction_budget = 100000;
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &pressure, &runtime) == ECONTAINER_RUNTIME_OK);
    free(bytes);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    uint8_t event[ECONTAINER_EVENT_BUFFER_BYTES] = {0};
    event[0] = 7;
    event[sizeof(event) - 1] = 13;
    int32_t result = -1;
    CHECK(econtainer_runtime_on_event(runtime, event, sizeof(event), &result) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(result == 20);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_RUNNING);
    const uint8_t small_event[] = {2, 3, 5};
    CHECK(econtainer_runtime_on_event(runtime, small_event, sizeof(small_event), &result) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(result == 30);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    econtainer_runtime_close(&runtime);
    return true;
}

static bool test_standard_page(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_limits_t page_limits = limits;
    page_limits.max_event_bytes = ECONTAINER_EVENT_BUFFER_BYTES;
    page_limits.init_instruction_budget = 4000000;
    page_limits.event_instruction_budget = 100000;
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &page_limits, &runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    int32_t result = 0;
    uint8_t event[ECONTAINER_EVENT_BUFFER_BYTES];
    memset(event, 0x5a, sizeof(event));
    event[0] = 'E';
    event[sizeof(event) - 1] = 42;
    CHECK(econtainer_runtime_on_event(runtime, event, sizeof(event), &result) == ECONTAINER_RUNTIME_OK);
    CHECK(result == 42);
    const uint8_t commands[] = {'Z', 'P', 'G', 'P', 'L'};
    const int32_t results[] = {0, 1, -1, 1, 77};
    for (size_t index = 0; index < sizeof(commands); ++index) {
        CHECK(econtainer_runtime_on_event(runtime, &commands[index], 1, &result) == ECONTAINER_RUNTIME_OK);
        CHECK(result == results[index]);
    }
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
    const uint8_t invalid[] = {'O', 'S', 'U'};
    for (size_t index = 0; index < sizeof(invalid); ++index) {
        CHECK(econtainer_runtime_open(bytes, length, &page_limits, &runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        result = 123;
        CHECK(econtainer_runtime_on_event(runtime, &invalid[index], 1, &result) ==
              ECONTAINER_RUNTIME_ENGINE_FAILURE);
        CHECK(result == 123 && econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
        CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    }
    free(bytes);
    return true;
}

static bool test_event_buffer_abi(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    const char *name = "econtainer_event_buffer";
    size_t name_offset = length;
    for (size_t index = 0; index + strlen(name) + 2 <= length; ++index)
        if (memcmp(bytes + index, name, strlen(name)) == 0) { name_offset = index; break; }
    CHECK(name_offset != length);
    const size_t kind_offset = name_offset + strlen(name);
    CHECK(bytes[kind_offset] == 3 && bytes[kind_offset + 1] == 1);
    econtainer_runtime_t *runtime = NULL;
    /* Missing export, and exporting the mutable stack pointer, both fail closed. */
    bytes[name_offset] = 'x';
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_BAD_ABI);
    CHECK(runtime == NULL);
    bytes[name_offset] = 'e';
    bytes[kind_offset + 1] = 0;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_BAD_ABI);
    CHECK(runtime == NULL);
    bytes[kind_offset + 1] = 1;
    /* Fixed SDK emits the BSS address 4112 as this immutable global. */
    const uint8_t global[] = {0x7f, 0, 0x41, 0x90, 0x20, 0x0b};
    size_t value_offset = length;
    for (size_t index = 8; index + sizeof(global) <= length; ++index)
        if (memcmp(bytes + index, global, sizeof(global)) == 0) { value_offset = index + 3; break; }
    CHECK(value_offset != length);
    bytes[value_offset] = 0xff;
    bytes[value_offset + 1] = 0x7f; /* Valid padded i32.const -1. */
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_BAD_ABI);
    CHECK(runtime == NULL);
    bytes[value_offset] = 0x80;
    bytes[value_offset + 1] = 0;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_BAD_ABI);
    CHECK(runtime == NULL);
    free(bytes);
    return true;
}

static bool test_wrong_abi_and_release(const char *wrong_path, const char *counter_path,
                                       const char *two_page_path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(wrong_path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_BAD_ABI);
    CHECK(runtime == NULL);
    free(bytes);
    bytes = read_file(two_page_path, &length);
    CHECK(bytes != NULL);
    CHECK(econtainer_wasm_check(bytes, length) == ECONTAINER_WASM_OK);
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) ==
          ECONTAINER_RUNTIME_BAD_ABI);
    CHECK(runtime == NULL);
    free(bytes);
    bytes = read_file(counter_path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_limits_t too_small = limits;
    too_small.max_memory_pages = 2;
    CHECK(econtainer_runtime_open(bytes, length, &too_small, &runtime) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    CHECK(runtime == NULL);
    too_small = limits;
    too_small.init_instruction_budget = 0;
    CHECK(econtainer_runtime_open(bytes, length, &too_small, &runtime) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    CHECK(runtime == NULL);
    too_small = limits;
    too_small.max_entry_duration_ms = 0;
    CHECK(econtainer_runtime_open(bytes, length, &too_small, &runtime) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    CHECK(runtime == NULL);
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_OK);
    free(bytes);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    econtainer_runtime_close(&runtime);
    return true;
}

static bool test_repeated_release(const char *counter_path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(counter_path, &length);
    CHECK(bytes != NULL);
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    resource_stats_t after_ten = {0}, after_fifty = {0}, after_hundred = {0};
#endif
    for (unsigned index = 0; index < 100; ++index) {
        econtainer_runtime_t *runtime = NULL;
        CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        const uint8_t event[] = {1, 2, 3};
        int32_t result = -1;
        CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(result == 3);
        CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(runtime == NULL);
#ifdef ECONTAINER_TEST_RESOURCE_STATS
        if (index == 9U) CHECK(sample_resources(&after_ten));
        if (index == 49U) CHECK(sample_resources(&after_fifty));
        if (index == 99U) CHECK(sample_resources(&after_hundred));
#endif
    }
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    CHECK(resources_stable("counter", after_ten, after_fifty, after_hundred));
#endif
    free(bytes);
    return true;
}

static bool test_failure_reopen(const char *counter_path, const char *init_loop_path,
                                const char *event_loop_path, const char *stop_loop_path,
                                const char *stop_fail_path)
{
    const char *paths[] = {init_loop_path, event_loop_path, stop_loop_path, stop_fail_path};
    uint8_t *fault_bytes[4] = {0};
    size_t fault_sizes[4] = {0};
    for (unsigned index = 0; index < 4; ++index) {
        fault_bytes[index] = read_file(paths[index], &fault_sizes[index]);
        CHECK(fault_bytes[index] != NULL);
    }
    size_t counter_size = 0;
    uint8_t *counter = read_file(counter_path, &counter_size);
    CHECK(counter != NULL);
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    resource_stats_t after_ten = {0}, after_fifty = {0}, after_hundred = {0};
#endif
    for (unsigned index = 0; index < 100; ++index) {
        const unsigned fault = index % 4U;
        econtainer_runtime_t *runtime = NULL;
        CHECK(econtainer_runtime_open(fault_bytes[fault], fault_sizes[fault], &limits,
                                      &runtime) == ECONTAINER_RUNTIME_OK);
        if (fault == 0U) {
            CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_INSTRUCTION_LIMIT);
        }
        else {
            CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
            const uint8_t event[] = {1};
            int32_t result = -1;
            if (fault == 1U) {
                CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
                      ECONTAINER_RUNTIME_INSTRUCTION_LIMIT);
            }
            else {
                CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
                      ECONTAINER_RUNTIME_OK);
                CHECK(econtainer_runtime_stop(runtime) ==
                      (fault == 2U ? ECONTAINER_RUNTIME_INSTRUCTION_LIMIT :
                                     ECONTAINER_RUNTIME_GUEST_FAILURE));
            }
        }
        CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
        CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(runtime == NULL);
        CHECK(econtainer_runtime_open(counter, counter_size, &limits, &runtime) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        const uint8_t event[] = {1, 2, 3};
        int32_t result = -1;
        CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(result == 3);
        CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(runtime == NULL);
#ifdef ECONTAINER_TEST_RESOURCE_STATS
        if (index == 9U) CHECK(sample_resources(&after_ten));
        if (index == 49U) CHECK(sample_resources(&after_fifty));
        if (index == 99U) CHECK(sample_resources(&after_hundred));
#endif
    }
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    CHECK(resources_stable("failure/reopen", after_ten, after_fifty, after_hundred));
#endif
    free(counter);
    for (unsigned index = 0; index < 4; ++index) free(fault_bytes[index]);
    return true;
}

static bool test_repeated_native_release(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_limits_t authorized = limits;
    authorized.allowed_capabilities = ECONTAINER_CAP_MONOTONIC_TIME | ECONTAINER_CAP_LOG;
    authorized.max_log_bytes = 16;
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    resource_stats_t after_ten = {0}, after_fifty = {0}, after_hundred = {0};
#endif
    for (unsigned index = 0; index < 100; ++index) {
        econtainer_runtime_t *runtime = NULL;
        CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        uint8_t log[16] = {0};
        size_t log_size = 0;
        CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(log_size == 4 && memcmp(log, "init", 4) == 0);
        const uint8_t event[] = {1, 'a', 'b', 'c'};
        int32_t result = -1;
        CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(result == 0);
        CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(log_size == 3 && memcmp(log, "abc", 3) == 0);
        CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(runtime == NULL);
#ifdef ECONTAINER_TEST_RESOURCE_STATS
        if (index == 9U) CHECK(sample_resources(&after_ten));
        if (index == 49U) CHECK(sample_resources(&after_fifty));
        if (index == 99U) CHECK(sample_resources(&after_hundred));
#endif
    }
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    CHECK(resources_stable("native", after_ten, after_fifty, after_hundred));
#endif
    free(bytes);
    return true;
}

static bool test_host_api(const char *host_api_path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(host_api_path, &length);
    CHECK(bytes != NULL);
    uint32_t imports = 0;
    CHECK(econtainer_wasm_imported_capabilities(bytes, length, &imports));
    CHECK(imports == (ECONTAINER_CAP_MONOTONIC_TIME | ECONTAINER_CAP_LOG));
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) ==
          ECONTAINER_RUNTIME_NOT_AUTHORIZED);
    econtainer_runtime_limits_t authorized = limits;
    authorized.allowed_capabilities = ECONTAINER_CAP_MONOTONIC_TIME;
    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_NOT_AUTHORIZED);
    authorized.allowed_capabilities |= ECONTAINER_CAP_LOG;
    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    authorized.max_log_bytes = 16;
    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    uint8_t log[16] = {0};
    size_t log_size = 99;
    CHECK(econtainer_runtime_take_log(runtime, log, 2, &log_size) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    CHECK(log_size == 99);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof(log), &log_size) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(log_size == 4 && memcmp(log, "init", 4) == 0);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof(log), &log_size) ==
          ECONTAINER_RUNTIME_NO_LOG);
    int32_t result = 99;
    uint8_t first[] = {1, 'a', 'b', 'c'};
    CHECK(econtainer_runtime_on_event(runtime, first, sizeof(first), &result) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(result == 0);
    memset(first, 'x', sizeof(first));
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof(log), &log_size) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(log_size == 3 && memcmp(log, "abc", 3) == 0);
    const uint8_t twice[] = {2};
    CHECK(econtainer_runtime_on_event(runtime, twice, sizeof(twice), &result) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(result == -2);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof(log), &log_size) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(log_size == 5 && memcmp(log, "first", 5) == 0);
    const uint8_t too_long[] = {4};
    CHECK(econtainer_runtime_on_event(runtime, too_long, sizeof(too_long), &result) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(result == -1);
    const uint8_t time_event[] = {5};
    CHECK(econtainer_runtime_on_event(runtime, time_event, sizeof(time_event), &result) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(result == 0);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(runtime == NULL);

    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof(log), &log_size) ==
          ECONTAINER_RUNTIME_OK);
    const uint8_t invalid_pointer[] = {3};
    econtainer_runtime_result_t invalid_result = econtainer_runtime_on_event(
        runtime, invalid_pointer, sizeof(invalid_pointer), &result);
    CHECK(invalid_result == ECONTAINER_RUNTIME_ENGINE_FAILURE);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof(log), &log_size) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(log_size == 4 && memcmp(log, "init", 4) == 0);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);

    /* An exact module/function/signature is required before WAMR loading. */
    uint8_t *bad = malloc(length);
    CHECK(bad != NULL);
    memcpy(bad, bytes, length);
    bool found = false;
    for (size_t index = 0; index + 10 < length; ++index) {
        if (memcmp(bad + index, "econtainer", 10) == 0) {
            bad[index] = 'x';
            found = true;
            break;
        }
    }
    CHECK(found);
    CHECK(econtainer_runtime_open(bad, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_BAD_WASM);
    memcpy(bad, bytes, length);
    found = false;
    const uint8_t clock_type[] = {0x60, 0x00, 0x01, 0x7e};
    for (size_t index = 0; index + sizeof(clock_type) <= length; ++index) {
        if (memcmp(bad + index, clock_type, sizeof(clock_type)) == 0) {
            bad[index + 3] = 0x7f;
            found = true;
            break;
        }
    }
    CHECK(found);
    CHECK(econtainer_wasm_check(bad, length) == ECONTAINER_WASM_UNSUPPORTED);
    CHECK(econtainer_runtime_open(bad, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_BAD_WASM);
    free(bad);
    free(bytes);
    return true;
}

static bool test_timers(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    uint32_t imports = 0;
    CHECK(econtainer_wasm_imported_capabilities(bytes, length, &imports));
    CHECK(imports == ECONTAINER_CAP_TIMER);
    econtainer_runtime_limits_t authorized = limits;
    authorized.allowed_capabilities = ECONTAINER_CAP_TIMER;
    authorized.max_timers = 2;
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &limits, &runtime) ==
          ECONTAINER_RUNTIME_NOT_AUTHORIZED);
    authorized.max_timers = 0;
    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    authorized.max_timers = 2;
    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    uint64_t deadline = 99;
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline) ==
          ECONTAINER_RUNTIME_NO_TIMER && deadline == 99);
    const uint8_t cancel_now[] = {'B'};
    int32_t result = -1;
    CHECK(econtainer_runtime_on_event(runtime, cancel_now, sizeof cancel_now, &result) ==
          ECONTAINER_RUNTIME_OK && result == 0);
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline) ==
          ECONTAINER_RUNTIME_NO_TIMER);
    const uint8_t replace_canceled[] = {'D'};
    CHECK(econtainer_runtime_on_event(runtime, replace_canceled,
                                      sizeof replace_canceled, &result) ==
          ECONTAINER_RUNTIME_OK && result == 0);
    const uint8_t schedule[] = {'A'};
    CHECK(econtainer_runtime_on_event(runtime, schedule, sizeof schedule, &result) ==
          ECONTAINER_RUNTIME_OK && result == 0);
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline) ==
          ECONTAINER_RUNTIME_OK);
    econtainer_timer_event_t delivered = {0};
    CHECK(econtainer_runtime_poll_timer(runtime, &delivered, &result) ==
          ECONTAINER_RUNTIME_NO_TIMER);
    uint8_t forged[16] = {'E', 'C', 'T', 1};
    CHECK(econtainer_runtime_on_event(runtime, forged, sizeof forged, &result) ==
          ECONTAINER_RUNTIME_INVALID_INPUT);
    struct timespec wait = {.tv_sec = 0, .tv_nsec = 40000000};
    CHECK(nanosleep(&wait, NULL) == 0);
    CHECK(econtainer_runtime_poll_timer(runtime, &delivered, &result) ==
          ECONTAINER_RUNTIME_OK && result == 10 &&
          delivered.handle != 0 && delivered.skipped_periods == 0);
    const uint64_t one_shot = delivered.handle;
    CHECK(econtainer_runtime_poll_timer(runtime, &delivered, &result) ==
          ECONTAINER_RUNTIME_OK &&
          result == 20 + (int32_t)delivered.skipped_periods &&
          delivered.handle != 0 && delivered.handle != one_shot);
    const uint64_t periodic = delivered.handle;
    CHECK(econtainer_runtime_poll_timer(runtime, &delivered, &result) ==
          ECONTAINER_RUNTIME_NO_TIMER);
    wait.tv_nsec = 260000000;
    CHECK(nanosleep(&wait, NULL) == 0);
    CHECK(econtainer_runtime_poll_timer(runtime, &delivered, &result) ==
          ECONTAINER_RUNTIME_OK && delivered.handle == periodic &&
          delivered.skipped_periods >= 1 &&
          result == 20 + (int32_t)delivered.skipped_periods);
    uint8_t cancel[9] = {'C'};
    for (uint32_t index = 0; index < 8; ++index)
        cancel[1 + index] = (uint8_t)(one_shot >> (index * 8));
    CHECK(econtainer_runtime_on_event(runtime, cancel, sizeof cancel, &result) ==
          ECONTAINER_RUNTIME_OK && result == -1);
    for (uint32_t index = 0; index < 8; ++index)
        cancel[1 + index] = (uint8_t)(periodic >> (index * 8));
    CHECK(econtainer_runtime_on_event(runtime, cancel, sizeof cancel, &result) ==
          ECONTAINER_RUNTIME_OK && result == 0);
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline) ==
          ECONTAINER_RUNTIME_NO_TIMER);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_poll_timer(runtime, &delivered, &result) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
          ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_on_event(runtime, cancel, sizeof cancel, &result) ==
          ECONTAINER_RUNTIME_OK && result == -1);
    CHECK(econtainer_runtime_on_event(runtime, schedule, sizeof schedule, &result) ==
          ECONTAINER_RUNTIME_OK && result == 0);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
    free(bytes);
    return true;
}

static bool test_timer_handle_boundary(void)
{
    CHECK(econtainer_runtime_issue_timer_handle(NULL) == 0);
    uint64_t next = (UINT64_C(1) << 24) - 1U;
    const uint64_t old_limit = econtainer_runtime_issue_timer_handle(&next);
    const uint64_t after_old_limit = econtainer_runtime_issue_timer_handle(&next);
    CHECK(old_limit == (UINT64_C(1) << 24) - 1U);
    CHECK(after_old_limit == (UINT64_C(1) << 24));
    CHECK(next == after_old_limit + 1U);
    next = UINT64_MAX;
    CHECK(econtainer_runtime_issue_timer_handle(&next) == UINT64_MAX);
    CHECK(next == 0);
    CHECK(econtainer_runtime_issue_timer_handle(&next) == 0 && next == 0);
    return true;
}

static bool test_repeated_timer_release(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_limits_t authorized = limits;
    authorized.allowed_capabilities = ECONTAINER_CAP_TIMER;
    authorized.max_timers = 2;
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    resource_stats_t after_ten = {0}, after_fifty = {0}, after_hundred = {0};
#endif
    for (unsigned index = 0; index < 100; ++index) {
        econtainer_runtime_t *runtime = NULL;
        CHECK(econtainer_runtime_open(bytes, length, &authorized, &runtime) ==
              ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
        const uint8_t schedule[] = {'A'};
        int32_t result = -1;
        CHECK(econtainer_runtime_on_event(runtime, schedule, sizeof schedule, &result) ==
              ECONTAINER_RUNTIME_OK && result == 0);
        CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_OK);
        CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK);
#ifdef ECONTAINER_TEST_RESOURCE_STATS
        if (index == 9U) CHECK(sample_resources(&after_ten));
        if (index == 49U) CHECK(sample_resources(&after_fifty));
        if (index == 99U) CHECK(sample_resources(&after_hundred));
#endif
    }
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    CHECK(resources_stable("timer", after_ten, after_fifty, after_hundred));
#endif
    free(bytes);
    return true;
}

static bool test_expired_entry(const char *path)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    CHECK(bytes != NULL);
    econtainer_runtime_limits_t quick = limits;
    quick.allowed_capabilities = ECONTAINER_CAP_MONOTONIC_TIME |
                                 ECONTAINER_CAP_LOG | ECONTAINER_CAP_TIMER;
    quick.max_log_bytes = 16;
    quick.max_timers = 1;
    quick.max_entry_duration_ms = 20;
    quick.event_instruction_budget = INT32_MAX;
    econtainer_runtime_t *runtime = NULL;
    CHECK(econtainer_runtime_open(bytes, length, &quick, &runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    int32_t result = 123;
    const uint8_t event[] = {'D'};
    econtainer_runtime_result_t event_status = econtainer_runtime_on_event(
        runtime, event, sizeof event, &result);
    CHECK(event_status == ECONTAINER_RUNTIME_ENTRY_EXPIRED);
    CHECK(result == 123);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
    uint8_t log[16] = {0};
    size_t log_size = 99;
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
          ECONTAINER_RUNTIME_NO_LOG && log_size == 99);
    uint64_t deadline_ms = 0;
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline_ms) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    econtainer_timer_event_t timer_event = {0};
    CHECK(econtainer_runtime_poll_timer(runtime, &timer_event, &result) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    CHECK(econtainer_runtime_stop(runtime) == ECONTAINER_RUNTIME_INVALID_STATE);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);

    CHECK(econtainer_runtime_open(bytes, length, &quick, &runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    result = 456;
    const uint8_t returned_event[] = {'R'};
    CHECK(econtainer_runtime_on_event(runtime, returned_event,
                                      sizeof returned_event, &result) ==
          ECONTAINER_RUNTIME_ENTRY_EXPIRED);
    CHECK(result == 456);
    CHECK(econtainer_runtime_state(runtime) == ECONTAINER_RUNTIME_FAILED);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
          ECONTAINER_RUNTIME_NO_LOG && log_size == 99);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);

    CHECK(econtainer_runtime_open(bytes, length, &quick, &runtime) == ECONTAINER_RUNTIME_OK);
    CHECK(econtainer_runtime_init(runtime) == ECONTAINER_RUNTIME_OK);
    const uint8_t timer_event_bytes[] = {'T'};
    CHECK(econtainer_runtime_on_event(runtime, timer_event_bytes,
                                      sizeof timer_event_bytes, &result) == ECONTAINER_RUNTIME_OK);
    CHECK(result == 0);
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline_ms) == ECONTAINER_RUNTIME_OK);
    const uint8_t prior_event[] = {'P'};
    CHECK(econtainer_runtime_on_event(runtime, prior_event,
                                      sizeof prior_event, &result) == ECONTAINER_RUNTIME_OK);
    CHECK(result == 0);
    CHECK(econtainer_runtime_on_event(runtime, event, sizeof event, &result) ==
          ECONTAINER_RUNTIME_ENTRY_EXPIRED);
    CHECK(econtainer_runtime_next_timer_deadline(runtime, &deadline_ms) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    CHECK(econtainer_runtime_poll_timer(runtime, &timer_event, &result) ==
          ECONTAINER_RUNTIME_INVALID_STATE);
    CHECK(econtainer_runtime_take_log(runtime, log, sizeof log, &log_size) ==
          ECONTAINER_RUNTIME_OK && log_size == 5 &&
          memcmp(log, "prior", log_size) == 0);
    CHECK(econtainer_runtime_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    free(bytes);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 13) {
        fprintf(stderr, "usage: %s counter event-read init-loop event-loop stop-loop stop-fail wrong-signature host-api timer deadline two-page memory\n",
                argv[0]);
        return 2;
    }
#ifdef ECONTAINER_TEST_RESOURCE_STATS
    if (!resource_probes_calibrated()) {
        fprintf(stderr, "resource probe calibration failed\n");
        return 1;
    }
#endif
    const bool passed = test_timer_handle_boundary() &&
                        test_counter(argv[1]) && test_event_copy(argv[2]) &&
#ifndef _WIN32
                        test_readonly_data_and_import(argv[8]) &&
                        test_readonly_flash_sized_wasm(argv[1]) &&
                        test_malformed_sections(argv[1]) &&
#endif
                        test_maximum_event(argv[2]) && test_standard_page(argv[12]) &&
                        test_event_buffer_abi(argv[1]) &&
                        test_loop(argv[3], 0) && test_loop(argv[4], 1) &&
                        test_loop(argv[5], 2) &&
                        test_pure_guest_deadline(argv[3], argv[1], 0) &&
                        test_pure_guest_deadline(argv[4], argv[1], 1) &&
                        test_pure_guest_deadline(argv[5], argv[1], 2) &&
                        test_async_cancellation(argv[3], 0, false) &&
                        test_async_cancellation(argv[4], 1, false) &&
                        test_async_cancellation(argv[10], 1, true) &&
                        test_repeated_cancel_release(argv[4]) &&
                        test_cancel_stop_failures(argv[5], argv[6]) &&
                        test_wrong_abi_and_release(argv[7], argv[1], argv[11]) &&
                        test_repeated_release(argv[1]) &&
                        test_failure_reopen(argv[1], argv[3], argv[4], argv[5], argv[6]) &&
                        test_repeated_native_release(argv[8]) &&
                        test_host_api(argv[8]) && test_timers(argv[9]) &&
                        test_repeated_timer_release(argv[9]) &&
                        test_expired_entry(argv[10]);
    if (passed) {
        fprintf(stderr, "runtime instance: 100 counter, 100 native, 100 timer and 100 failure/reopen cycles passed\n");
    }
    return passed ? 0 : 1;
}
