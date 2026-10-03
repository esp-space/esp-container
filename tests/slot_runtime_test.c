#include "esp_container_product.h"

#include <assert.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>


#ifdef ECONTAINER_TEST_HOST_CLOCK
/* Only the separately compiled runtime test library uses this clock symbol.
 * WAMR's interpreter continues to use its real platform clock. */
static uint64_t clock_step_us;
static uint64_t clock_now_us;
static unsigned clock_reads;
static unsigned clock_fail_at;
static unsigned clock_backwards_at;

static void set_test_clock(uint64_t step_us, unsigned fail_at, unsigned backwards_at)
{
    struct timespec real;
    assert(clock_gettime(CLOCK_MONOTONIC, &real) == 0);
    clock_now_us = (uint64_t)real.tv_sec * 1000000U + (uint64_t)real.tv_nsec / 1000U;
    clock_step_us = step_us;
    clock_reads = 0;
    clock_fail_at = fail_at;
    clock_backwards_at = backwards_at;
}

int econtainer_test_clock_gettime(clockid_t kind, struct timespec *out)
{
    if (clock_step_us == 0 && clock_fail_at == 0 && clock_backwards_at == 0)
        return clock_gettime(kind, out);
    assert(kind == CLOCK_MONOTONIC);
    ++clock_reads;
    if (clock_reads == clock_fail_at) return -1;
    if (clock_reads == clock_backwards_at) clock_now_us -= 1000000U;
    else clock_now_us += clock_step_us;
    out->tv_sec = (time_t)(clock_now_us / 1000000U);
    out->tv_nsec = (long)((clock_now_us % 1000000U) * 1000U);
    return 0;
}
#endif

enum { FLASH_BASE = 0x10000, SLOT_BYTES = 32768, FLASH_BYTES = 3 * SLOT_BYTES };
typedef struct { uint8_t *bytes; size_t size; } file_t;
typedef struct {
    uint8_t flash[FLASH_BYTES];
    uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES];
    bool present;
    bool locked;
    pthread_mutex_t lock;
    pthread_mutex_t gate;
    pthread_cond_t condition;
    unsigned stage;
    unsigned acknowledged;
    bool race;
    bool map_fail;
    bool map_null;
    bool unmap_fail;
    bool read_fail;
    const file_t *wrong_mapping;
    uint8_t *mapping;
    size_t mapping_size;
    unsigned mapped;
    unsigned unmapped;
    unsigned erases;
    unsigned writes;
} store_t;

static const econtainer_slots_geometry_t geometry = {
    .partition_offset_bytes = FLASH_BASE, .partition_size_bytes = FLASH_BYTES,
    .erase_unit_bytes = 4096, .write_unit_bytes = 4,
    .slots = {{FLASH_BASE, SLOT_BYTES}, {FLASH_BASE + SLOT_BYTES, SLOT_BYTES},
              {FLASH_BASE + 2 * SLOT_BYTES, SLOT_BYTES}},
};

static bool lock_store(void *context)
{
    store_t *store = context;
    if (pthread_mutex_trylock(&store->lock) != 0) return false;
    assert(!store->locked);
    store->locked = true;
    return true;
}

static void unlock_store(void *context)
{
    store_t *store = context;
    assert(store->locked && store->mapping == NULL);
    store->locked = false;
    assert(pthread_mutex_unlock(&store->lock) == 0);
}

static econtainer_slot_blob_result_t read_blob(void *context, uint8_t *blob)
{
    store_t *store = context;
    assert(store->locked);
    if (!store->present) return ECONTAINER_SLOT_BLOB_NOT_FOUND;
    memcpy(blob, store->blob, sizeof(store->blob));
    return ECONTAINER_SLOT_BLOB_FOUND;
}

static bool write_blob(void *context, const uint8_t *blob)
{
    store_t *store = context;
    assert(store->locked && store->mapping == NULL);
    memcpy(store->blob, blob, sizeof(store->blob));
    store->present = true;
    return true;
}

static bool in_range(uint32_t offset, size_t size)
{
    return size > 0 && offset >= FLASH_BASE &&
           (uint64_t)offset + size <= FLASH_BASE + FLASH_BYTES;
}

static bool read_flash(void *context, uint32_t offset, uint8_t *bytes, size_t size)
{
    store_t *store = context;
    assert(store->locked && in_range(offset, size));
    if (store->read_fail) return false;
    memcpy(bytes, store->flash + (offset - FLASH_BASE), size);
    return true;
}

static bool erase_flash(void *context, uint32_t offset, uint32_t size)
{
    store_t *store = context;
    assert(store->locked && store->mapping == NULL && in_range(offset, size));
    ++store->erases;
    memset(store->flash + (offset - FLASH_BASE), 0xff, size);
    return true;
}

static bool write_flash(void *context, uint32_t offset, const uint8_t *bytes, size_t size)
{
    store_t *store = context;
    assert(store->locked && store->mapping == NULL && in_range(offset, size));
    ++store->writes;
    for (size_t index = 0; index < size; ++index) {
        uint8_t *destination = store->flash + (offset - FLASH_BASE) + index;
        assert((*destination & bytes[index]) == bytes[index]);
        *destination &= bytes[index];
    }
    return true;
}

static void race_point(store_t *store)
{
    if (!store->race) return;
    assert(pthread_mutex_lock(&store->gate) == 0);
    ++store->stage;
    assert(pthread_cond_broadcast(&store->condition) == 0);
    while (store->acknowledged != store->stage)
        assert(pthread_cond_wait(&store->condition, &store->gate) == 0);
    assert(pthread_mutex_unlock(&store->gate) == 0);
}

static bool map_flash(void *context, uint32_t offset, size_t size,
                       const uint8_t **mapped, uintptr_t *handle)
{
    store_t *store = context;
    assert(store->locked && store->mapping == NULL && in_range(offset, size));
    *mapped = NULL;
    *handle = 0;
    if (store->map_fail) return false;
    ++store->mapped;
    if (store->map_null) return true;
    store->mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(store->mapping != MAP_FAILED);
    store->mapping_size = size;
    if (store->wrong_mapping != NULL) {
        assert(store->wrong_mapping->size == size);
        memcpy(store->mapping, store->wrong_mapping->bytes, size);
    } else {
        memcpy(store->mapping, store->flash + (offset - FLASH_BASE), size);
    }
    assert(mprotect(store->mapping, size, PROT_READ) == 0);
    *mapped = store->mapping;
    /* Zero is a valid successful mapping handle. */
    race_point(store);
    return true;
}

static bool unmap_flash(void *context, uintptr_t handle)
{
    store_t *store = context;
    assert(store->locked && handle == 0);
    /* A second writer attempts entry after the real WAMR open completed. */
    race_point(store);
    ++store->unmapped;
    if (store->mapping != NULL) assert(munmap(store->mapping, store->mapping_size) == 0);
    store->mapping = NULL;
    store->mapping_size = 0;
    return !store->unmap_fail;
}

static bool read_source(void *context, size_t offset, uint8_t *bytes, size_t size)
{
    const file_t *file = context;
    if (offset > file->size || size > file->size - offset) return false;
    memcpy(bytes, file->bytes + offset, size);
    return true;
}

static file_t read_file(const char *directory, const char *name)
{
    char path[1024];
    const int count = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(count > 0 && (size_t)count < sizeof(path));
    FILE *input = fopen(path, "rb");
    assert(input != NULL && fseek(input, 0, SEEK_END) == 0);
    const long length = ftell(input);
    assert(length > 0 && fseek(input, 0, SEEK_SET) == 0);
    file_t file = {malloc((size_t)length), (size_t)length};
    assert(file.bytes != NULL && fread(file.bytes, 1, file.size, input) == file.size);
    assert(fclose(input) == 0);
    return file;
}

typedef struct {
    store_t store;
    econtainer_slots_io_t io;
    econtainer_slot_firmware_set_t firmware;
    econtainer_slots_state_t state;
    econtainer_package_workspace_t package_workspace;
    econtainer_wasm_workspace_t wasm_workspace;
    econtainer_package_info_t info;
    econtainer_package_slot_validation_t validation;
    econtainer_runtime_limits_t limits;
    uint8_t boot[ECONTAINER_SLOT_BOOT_ID_BYTES];
    uint8_t next_operation;
    uint8_t counter_package_sha256[4][32];
    file_t counter_versions[4];
} fixture_t;

static void initialize(fixture_t *fixture, const file_t *key)
{
    memset(fixture, 0, sizeof(*fixture));
    store_t *store = &fixture->store;
    assert(pthread_mutex_init(&store->lock, NULL) == 0);
    assert(pthread_mutex_init(&store->gate, NULL) == 0);
    assert(pthread_cond_init(&store->condition, NULL) == 0);
    memset(store->flash, 0xff, sizeof(store->flash));
    fixture->io = (econtainer_slots_io_t){
        .lock = lock_store, .unlock = unlock_store, .read_blob = read_blob,
        .write_blob = write_blob, .flash_read = read_flash, .flash_erase = erase_flash,
        .flash_write = write_flash, .flash_map = map_flash, .flash_unmap = unmap_flash,
        .context = store,
    };
    fixture->firmware.bootable_count = 2;
    memset(fixture->firmware.bootable_firmware_sha256[0], 0x11, 32);
    memset(fixture->firmware.bootable_firmware_sha256[1], 0x22, 32);
    memcpy(fixture->firmware.running_firmware_sha256,
           fixture->firmware.bootable_firmware_sha256[0], 32);
    econtainer_slot_binding_t bindings[2] = {{.present = true}, {.present = true}};
    for (unsigned index = 0; index < 2; ++index)
        memcpy(bindings[index].firmware_sha256,
               fixture->firmware.bootable_firmware_sha256[index], 32);
    assert(econtainer_slots_initialize(&fixture->io, &geometry, &fixture->firmware,
                                       bindings) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_load(&fixture->io, &geometry, &fixture->state) == ECONTAINER_SLOTS_OK);
    fixture->validation = (econtainer_package_slot_validation_t){
        .expected_product_id = "counter", .public_key_rsa_der = key->bytes,
        .public_key_size_bytes = key->size, .expected_key_id = "test-key",
        .max_wasm_bytes = 16384,
        .wasm_authorization = {.allowed_capabilities = ECONTAINER_CAP_ALL,
            .max_memory_bytes = 65536, .max_stack_bytes = 16384},
        .max_event_queue_limit = 8, .max_instruction_budget = 100000,
        .max_host_call_timeout_ms = 100, .max_storage_limit_bytes = 0,
        .package_workspace = &fixture->package_workspace,
        .wasm_workspace = &fixture->wasm_workspace, .verified_info = &fixture->info,
    };
    fixture->limits = (econtainer_runtime_limits_t){
        .max_wasm_bytes = 16384, .max_memory_pages = 1, .stack_size_bytes = 16384,
        .max_event_bytes = 128,
        .allowed_capabilities = ECONTAINER_CAP_ALL, .max_log_bytes = 16, .max_timers = 1,
        .init_instruction_budget = 200000, .event_instruction_budget = 200000,
        .stop_instruction_budget = 200000, .max_entry_duration_ms = 1000,
        .max_host_call_timeout_ms = 500,
    };
    fixture->boot[0] = 0x55;
}

static econtainer_slot_selection_request_t request_for(fixture_t *fixture, bool trial)
{
    econtainer_slot_selection_request_t request = {
        .expected_sequence = fixture->state.sequence, .firmware_set = fixture->firmware,
        .selection = trial ? ECONTAINER_SLOT_SELECT_TRIAL : ECONTAINER_SLOT_SELECT_CONFIRMED,
    };
    if (trial) {
        memcpy(request.operation_id, fixture->state.operation.operation_id, sizeof(request.operation_id));
        memcpy(request.boot_id, fixture->boot, sizeof(request.boot_id));
    }
    return request;
}

static econtainer_slot_runtime_result_t open_request(fixture_t *fixture,
    const econtainer_slot_selection_request_t *request, econtainer_runtime_t **runtime)
{
    const econtainer_slot_runtime_result_t result = econtainer_product_open(
        &fixture->io, &geometry, request, &fixture->validation, &fixture->limits, runtime);
    if (result.slots != ECONTAINER_SLOTS_OK || result.runtime != ECONTAINER_RUNTIME_OK) {
        assert(result.product_id_offset_bytes == 0U);
        assert(result.product_id_size_bytes == 0U);
        assert(result.product_version_offset_bytes == 0U);
        assert(result.product_version_size_bytes == 0U);
        assert(result.guest_abi_version == 0U);
        assert(result.data_schema_version == 0U);
    }
    return result;
}

static void expect_rejection(fixture_t *fixture,
    const econtainer_slot_selection_request_t *request, econtainer_slots_result_t expected)
{
    econtainer_runtime_t *runtime = NULL;
    const econtainer_slot_runtime_result_t result = open_request(fixture, request, &runtime);
    assert(result.slots == expected && result.runtime == ECONTAINER_RUNTIME_INVALID_STATE);
    assert(result.event_queue_limit == 0U);
    const uint8_t empty_digest[32] = {0};
    assert(memcmp(result.package_sha256, empty_digest, sizeof empty_digest) == 0);
    assert(runtime == NULL && !fixture->store.locked && fixture->store.mapping == NULL);
    assert(fixture->store.mapped == fixture->store.unmapped);
}

static void prepare(fixture_t *fixture, const file_t *package)
{
    econtainer_slot_operation_t operation = {0};
    operation.operation_id[0] = ++fixture->next_operation;
    memcpy(operation.target_firmware_sha256, fixture->firmware.running_firmware_sha256, 32);
    assert(SHA256(package->bytes, package->size, operation.package_sha256) != NULL);
    operation.package_size_bytes = (uint32_t)package->size;
    operation.guest_abi_version = 2;
    operation.data_schema_version = 1;
    assert(econtainer_slots_reserve(&fixture->io, &geometry, fixture->state.sequence,
        &fixture->firmware, &operation, &fixture->state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_write_and_prepare(&fixture->io, &geometry, fixture->state.sequence,
        read_source, (void *)package, econtainer_package_slot_validate,
        &fixture->validation, &fixture->state) == ECONTAINER_SLOTS_OK);
}

static void begin_trial(fixture_t *fixture)
{
    assert(econtainer_slots_begin_trial(&fixture->io, &geometry, fixture->state.sequence,
        fixture->firmware.running_firmware_sha256, fixture->boot,
        &fixture->state) == ECONTAINER_SLOTS_OK);
}

static void confirm(fixture_t *fixture)
{
    assert(econtainer_slots_mark_healthy(&fixture->io, &geometry, fixture->state.sequence,
        fixture->boot, &fixture->state) == ECONTAINER_SLOTS_OK);
    const econtainer_slot_selection_request_t healthy = request_for(fixture, true);
    expect_rejection(fixture, &healthy, ECONTAINER_SLOTS_CONFLICT);
    assert(econtainer_slots_confirm(&fixture->io, &geometry, fixture->state.sequence,
        fixture->firmware.running_firmware_sha256, fixture->boot,
        &fixture->state) == ECONTAINER_SLOTS_OK);
}

static void run_counter(fixture_t *fixture, bool trial, int32_t expected_result)
{
    const econtainer_slot_selection_request_t request = request_for(fixture, trial);
    econtainer_runtime_t *runtime = NULL;
    /* Installation's old info is neither required nor an input proof. */
    fixture->validation.verified_info = NULL;
    const econtainer_slot_runtime_result_t result = open_request(fixture, &request, &runtime);
    fixture->validation.verified_info = &fixture->info;
    assert(result.slots == ECONTAINER_SLOTS_OK && result.runtime == ECONTAINER_RUNTIME_OK);
    const uint8_t *selected_digest = trial ? fixture->state.operation.package_sha256 : NULL;
    if (!trial) {
        for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
            const econtainer_slot_binding_t *binding = &fixture->state.bindings[index];
            if (binding->present && memcmp(binding->firmware_sha256,
                    fixture->firmware.running_firmware_sha256, 32) == 0) {
                selected_digest = binding->package_sha256;
                break;
            }
        }
    }
    assert(selected_digest != NULL && result.event_queue_limit == 8U &&
           memcmp(result.package_sha256, selected_digest, 32) == 0);
    assert(runtime != NULL && fixture->store.mapping == NULL && !fixture->store.locked);
    assert(result.guest_abi_version == 2U && result.data_schema_version == 1U);
    assert(result.product_id_size_bytes == 7U &&
           result.product_id_offset_bytes <= ECONTAINER_PACKAGE_MANIFEST_MAX_BYTES - 7U);
    assert(memcmp(fixture->package_workspace.manifest + result.product_id_offset_bytes,
                  "counter", 7U) == 0);
    bool version_matched = false;
    for (unsigned index = 0; index < 4; ++index) {
        if (memcmp(result.package_sha256, fixture->counter_package_sha256[index], 32) != 0) continue;
        const file_t *version = &fixture->counter_versions[index];
        assert(result.product_version_size_bytes == version->size &&
               result.product_version_offset_bytes <= ECONTAINER_PACKAGE_MANIFEST_MAX_BYTES - version->size);
        assert(memcmp(fixture->package_workspace.manifest + result.product_version_offset_bytes,
                      version->bytes, version->size) == 0);
        version_matched = true;
    }
    assert(version_matched);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    const uint8_t event[] = {1, 2, 3};
    int32_t value = -1;
    assert(econtainer_product_on_event(runtime, event, sizeof(event), &value) == ECONTAINER_RUNTIME_OK);
    assert(value == expected_result);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
}

static bool stopped(void *context, const uint8_t *operation_id)
{
    fixture_t *fixture = context;
    assert(fixture->store.locked);
    return memcmp(operation_id, fixture->state.operation.operation_id,
                  ECONTAINER_SLOT_OPERATION_ID_BYTES) == 0;
}

static void abandon(fixture_t *fixture)
{
    assert(econtainer_slots_abandon(&fixture->io, &geometry, fixture->state.sequence,
        fixture->boot, stopped, fixture, &fixture->state) == ECONTAINER_SLOTS_OK);
}

static void *competing_writer(void *context)
{
    fixture_t *fixture = context;
    store_t *store = &fixture->store;
    for (unsigned stage = 1; stage <= 2; ++stage) {
        assert(pthread_mutex_lock(&store->gate) == 0);
        while (store->stage != stage)
            assert(pthread_cond_wait(&store->condition, &store->gate) == 0);
        econtainer_slots_state_t unused;
        econtainer_slot_operation_t operation = fixture->state.operation;
        operation.slot = 0;
        memset(operation.trial_boot_id, 0, sizeof(operation.trial_boot_id));
        assert(econtainer_slots_reserve(&fixture->io, &geometry, fixture->state.sequence,
            &fixture->firmware, &operation, &unused) == ECONTAINER_SLOTS_BUSY);
        assert(econtainer_slots_write_and_prepare(&fixture->io, &geometry, fixture->state.sequence,
            read_source, NULL, econtainer_package_slot_validate, &fixture->validation,
            &unused) == ECONTAINER_SLOTS_BUSY);
        store->acknowledged = stage;
        assert(pthread_cond_broadcast(&store->condition) == 0);
        assert(pthread_mutex_unlock(&store->gate) == 0);
    }
    return NULL;
}

static void message_event(econtainer_runtime_t *runtime, const uint8_t *event,
                          size_t size_bytes, int32_t expected_result)
{
    int32_t result = INT32_MIN;
    assert(econtainer_product_on_event(runtime, event, size_bytes, &result) ==
           ECONTAINER_RUNTIME_OK && result == expected_result);
}

static void run_message_counter(fixture_t *fixture, const file_t *package)
{
    fixture->limits.max_timers = 1;
    prepare(fixture, package);
    begin_trial(fixture);
    const econtainer_slot_selection_request_t request = request_for(fixture, true);
    econtainer_runtime_t *runtime = NULL;
    const econtainer_slot_runtime_result_t opened = open_request(fixture, &request, &runtime);
    assert(opened.slots == ECONTAINER_SLOTS_OK && opened.runtime == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    const uint8_t state[] = {4}, count[] = {5}, pause[] = {2}, resume[] = {3};
    const uint8_t message[] = {1, 2, 3}, another[] = {1, 4};
    const uint8_t malformed[] = {2, 0}, unknown[] = {6}, empty_message[] = {1};
    message_event(runtime, state, sizeof state, 0);
    message_event(runtime, message, sizeof message, 2);
    message_event(runtime, another, sizeof another, 3);
    message_event(runtime, state, sizeof state, 1);
    uint64_t deadline_ms = 0;
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) == ECONTAINER_RUNTIME_OK);
    message_event(runtime, malformed, sizeof malformed, -1);
    message_event(runtime, empty_message, sizeof empty_message, -1);
    message_event(runtime, unknown, sizeof unknown, -1);
    message_event(runtime, count, sizeof count, 3);
    message_event(runtime, pause, sizeof pause, 3);
    message_event(runtime, state, sizeof state, 2);
    message_event(runtime, message, sizeof message, -2);
    message_event(runtime, count, sizeof count, 3);
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) ==
           ECONTAINER_RUNTIME_NO_TIMER);
    message_event(runtime, resume, sizeof resume, 3);
    message_event(runtime, state, sizeof state, 0);
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) ==
           ECONTAINER_RUNTIME_NO_TIMER);
    message_event(runtime, message, sizeof message, 5);
    econtainer_timer_event_t timer_event = {0};
    int32_t result = INT32_MIN;
    econtainer_runtime_result_t polled = ECONTAINER_RUNTIME_NO_TIMER;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000};
    for (unsigned attempt = 0; attempt < 100U && polled == ECONTAINER_RUNTIME_NO_TIMER;
         ++attempt) {
        assert(nanosleep(&delay, NULL) == 0);
        polled = econtainer_product_poll_timer(runtime, &timer_event, &result);
    }
    assert(polled == ECONTAINER_RUNTIME_OK && result == 5 && timer_event.handle != 0);
    message_event(runtime, state, sizeof state, 0);
    message_event(runtime, count, sizeof count, 5);
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) ==
           ECONTAINER_RUNTIME_NO_TIMER);
    message_event(runtime, message, sizeof message, 7);
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_poll_timer(runtime, &timer_event, &result) ==
           ECONTAINER_RUNTIME_INVALID_STATE);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    /* Reopen the same signed trial in the same firmware/boot: sample RAM starts
     * empty, with no timer or count carried over from the prior instance. */
    const econtainer_slot_runtime_result_t reopened = open_request(fixture, &request, &runtime);
    assert(reopened.slots == ECONTAINER_SLOTS_OK && reopened.runtime == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    message_event(runtime, count, sizeof count, 0);
    message_event(runtime, state, sizeof state, 0);
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) ==
           ECONTAINER_RUNTIME_NO_TIMER);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    abandon(fixture);
    run_counter(fixture, false, 6);
    puts("message_counter: signed message/state/one-timer behavior, stop/reopen and old package restore passed");
}


#ifdef ECONTAINER_TEST_HOST_CLOCK
static econtainer_runtime_t *open_host_calls(fixture_t *fixture)
{
    const econtainer_slot_selection_request_t request = request_for(fixture, true);
    econtainer_runtime_t *runtime = NULL;
    const econtainer_slot_runtime_result_t opened = open_request(fixture, &request, &runtime);
    assert(opened.slots == ECONTAINER_SLOTS_OK && opened.runtime == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    return runtime;
}

static void host_event(econtainer_runtime_t *runtime, uint8_t event, int32_t expected)
{
    int32_t result = 1234;
    assert(econtainer_product_on_event(runtime, &event, 1, &result) == ECONTAINER_RUNTIME_OK);
    if (result != expected) {
        fprintf(stderr, "host event %u: expected %d, got %d\n", event, expected, result);
        exit(1);
    }
}

static void check_host_expiration(fixture_t *fixture, uint8_t event, bool prior_log,
                                  bool prior_timer)
{
    econtainer_runtime_t *runtime = open_host_calls(fixture);
    if (prior_log) host_event(runtime, 1, 0);
    if (prior_timer) host_event(runtime, 4, 0);
    /* Signed 100 ms beats the independent 500 ms platform grant. Even the
     * clock import's two reads stay below the separate 1000 ms entry budget. */
    set_test_clock(150000U, 0, 0);
    int32_t result = 1234;
    const econtainer_runtime_result_t status =
        econtainer_product_on_event(runtime, &event, 1, &result);
    set_test_clock(0, 0, 0);
    assert(status == ECONTAINER_RUNTIME_HOST_CALL_EXPIRED && result == 1234);
    assert(econtainer_product_on_event(runtime, &event, 1, &result) == ECONTAINER_RUNTIME_INVALID_STATE);
    uint64_t deadline = 0;
    assert(econtainer_product_next_timer_deadline(runtime, &deadline) == ECONTAINER_RUNTIME_INVALID_STATE);
    uint8_t log[16]; size_t size = 0;
    const econtainer_runtime_result_t logged = econtainer_product_take_log(runtime, log, sizeof log, &size);
    assert(logged == (prior_log ? ECONTAINER_RUNTIME_OK : ECONTAINER_RUNTIME_NO_LOG));
    if (prior_log) assert(size == 3 && memcmp(log, "new", 3) == 0);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    runtime = open_host_calls(fixture);
    assert(econtainer_product_next_timer_deadline(runtime, &deadline) == ECONTAINER_RUNTIME_NO_TIMER);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
}

static bool cancel_after_native(void *context)
{
    (void)context;
    return clock_reads >= 3;
}

static void run_host_call_deadlines(fixture_t *fixture, const file_t *package)
{
    fixture->limits.max_host_call_timeout_ms = 500;
    prepare(fixture, package);
    begin_trial(fixture);
    econtainer_runtime_t *normal = open_host_calls(fixture);
    host_event(normal, 2, -1);
    host_event(normal, 5, -1);
    host_event(normal, 7, -1);
    host_event(normal, 1, 0);
    host_event(normal, 1, -2);
    uint8_t normal_log[16]; size_t normal_size = 0;
    assert(econtainer_product_take_log(normal, normal_log, sizeof normal_log, &normal_size) == ECONTAINER_RUNTIME_OK);
    assert(normal_size == 3 && memcmp(normal_log, "new", 3) == 0);
    host_event(normal, 4, 0);
    host_event(normal, 4, -1);
    host_event(normal, 6, 0);
    host_event(normal, 6, -1);
    host_event(normal, 8, 0);
    assert(econtainer_product_stop(normal) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&normal) == ECONTAINER_RUNTIME_OK);
    /* Four imports, including invalid and busy return branches. */
    check_host_expiration(fixture, 1, false, false);
    check_host_expiration(fixture, 2, false, false);
    check_host_expiration(fixture, 1, true, false);
    check_host_expiration(fixture, 4, false, false);
    check_host_expiration(fixture, 5, false, false);
    check_host_expiration(fixture, 4, false, true);
    check_host_expiration(fixture, 6, false, true);
    check_host_expiration(fixture, 7, false, false);
    check_host_expiration(fixture, 8, false, false);
    econtainer_runtime_t *runtime = open_host_calls(fixture);
    set_test_clock(20000U, 0, 0);
    host_event(runtime, 9, 4); /* four <100 ms calls, sum >100 ms, entry <1000 ms */
    assert(clock_reads * 20000U > 100000U && clock_reads * 20000U < 1000000U);
    set_test_clock(0, 0, 0);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    /* A tighter platform grant also applies, independently of signed 100 ms. */
    fixture->limits.max_host_call_timeout_ms = 20;
    runtime = open_host_calls(fixture);
    set_test_clock(20000U, 0, 0);
    int32_t value = 1234; uint8_t event = 1;
    assert(econtainer_product_on_event(runtime, &event, 1, &value) == ECONTAINER_RUNTIME_HOST_CALL_EXPIRED);
    assert(value == 1234);
    set_test_clock(0, 0, 0);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    fixture->limits.max_host_call_timeout_ms = 500;
    /* Clock failure/backward at the log import's completion cannot succeed. */
    for (unsigned backwards = 0; backwards < 2; ++backwards) {
        runtime = open_host_calls(fixture);
        set_test_clock(1000U, backwards ? 0 : 3, backwards ? 3 : 0);
        assert(econtainer_product_on_event(runtime, &event, 1, &value) == ECONTAINER_RUNTIME_ENGINE_FAILURE);
        set_test_clock(0, 0, 0);
        uint8_t log[16]; size_t size = 0;
        assert(econtainer_product_take_log(runtime, log, sizeof log, &size) == ECONTAINER_RUNTIME_NO_LOG);
        assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    }
    /* An existing WAMR bad-address trap wins over the newly elapsed host limit. */
    runtime = open_host_calls(fixture);
    event = 3;
    set_test_clock(150000U, 0, 0);
    assert(econtainer_product_on_event(runtime, &event, 1, &value) == ECONTAINER_RUNTIME_ENGINE_FAILURE);
    set_test_clock(0, 0, 0);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    /* Entry expiration and owner cancellation retain their original precedence. */
    fixture->limits.max_entry_duration_ms = 300;
    runtime = open_host_calls(fixture);
    event = 1;
    set_test_clock(150000U, 0, 0);
    assert(econtainer_product_on_event(runtime, &event, 1, &value) == ECONTAINER_RUNTIME_ENTRY_EXPIRED);
    set_test_clock(0, 0, 0);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    fixture->limits.max_entry_duration_ms = 1000;
    fixture->limits.cancel_requested = cancel_after_native;
    runtime = open_host_calls(fixture);
    set_test_clock(150000U, 0, 0);
    assert(econtainer_product_on_event(runtime, &event, 1, &value) == ECONTAINER_RUNTIME_ENTRY_CANCELLED);
    set_test_clock(0, 0, 0);
    /* Stop ignores the caller cancellation predicate, while retaining its budgets. */
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    fixture->limits.cancel_requested = NULL;
    /* Init and stop imports each receive a fresh host-call budget as well. */
    const econtainer_slot_selection_request_t selected = request_for(fixture, true);
    econtainer_slot_runtime_result_t opened = open_request(fixture, &selected, &runtime);
    assert(opened.slots == ECONTAINER_SLOTS_OK && opened.runtime == ECONTAINER_RUNTIME_OK);
    set_test_clock(70000U, 0, 0);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_HOST_CALL_EXPIRED);
    set_test_clock(0, 0, 0);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    runtime = open_host_calls(fixture);
    set_test_clock(70000U, 0, 0);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_HOST_CALL_EXPIRED);
    set_test_clock(0, 0, 0);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    /* New host begin clock failure/overflow uses the same per-entry cleanup.
     * Earlier successful log is retained, timers cannot be delivered, and a
     * closed/reopened instance has neither logs nor timers. A near-UINT64_MAX
     * POSIX fixture is a software boundary, not an ESP int64 clock scenario. */
    for (unsigned overflow = 0; overflow < 2; ++overflow) {
        fixture->limits.max_entry_duration_ms = overflow ? 1 : 1000;
        set_test_clock(1U, 0, 0);
        /* Preparation includes real signature/WAMR work while its platform
         * clock stays real. Keep this fixture's tiny logical entry deadline
         * safely ahead of that clock; it does not measure wall-clock latency. */
        clock_now_us += 1000000U;
        runtime = open_host_calls(fixture);
        host_event(runtime, 4, 0);
        host_event(runtime, 1, 0);
        set_test_clock(100U, overflow ? 0 : 2, 0);
        if (overflow) clock_now_us = UINT64_MAX - 1500U;
        event = 8; value = 1234;
        assert(econtainer_product_on_event(runtime, &event, 1, &value) == ECONTAINER_RUNTIME_ENGINE_FAILURE);
        assert(value == 1234);
        set_test_clock(0, 0, 0);
        uint8_t log[16]; size_t size = 0;
        assert(econtainer_product_take_log(runtime, log, sizeof log, &size) == ECONTAINER_RUNTIME_OK);
        assert(size == 3 && memcmp(log, "new", 3) == 0);
        assert(econtainer_product_take_log(runtime, log, sizeof log, &size) == ECONTAINER_RUNTIME_NO_LOG);
        uint64_t deadline = 0;
        assert(econtainer_product_next_timer_deadline(runtime, &deadline) == ECONTAINER_RUNTIME_INVALID_STATE);
        assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_INVALID_STATE);
        assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
        fixture->limits.max_entry_duration_ms = 1000;
        runtime = open_host_calls(fixture);
        assert(econtainer_product_next_timer_deadline(runtime, &deadline) == ECONTAINER_RUNTIME_NO_TIMER);
        assert(econtainer_product_take_log(runtime, log, sizeof log, &size) == ECONTAINER_RUNTIME_NO_LOG);
        assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
        assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    }
    fixture->limits.max_host_call_timeout_ms = 0;
    const econtainer_slot_selection_request_t request = request_for(fixture, true);
    const econtainer_slot_runtime_result_t rejected = open_request(fixture, &request, &runtime);
    assert(rejected.slots == ECONTAINER_SLOTS_OK && rejected.runtime == ECONTAINER_RUNTIME_INVALID_INPUT && runtime == NULL);
    fixture->limits.max_host_call_timeout_ms = 500;
    abandon(fixture);
    run_counter(fixture, false, 6);
    puts("host_call_timeout: real signed limits, four imports/all returns, independent calls, cleanup/reopen passed");
}
#endif

static void run_retained_reference_identity(const file_t *key, const file_t *current,
                                             const file_t *other, bool damaged)
{
    fixture_t *fixture = malloc(sizeof(*fixture));
    assert(fixture != NULL);
    initialize(fixture, key);
    fixture->store.present = false;
    econtainer_slot_binding_t bindings[2] = {{0}};
    for (unsigned index = 0; index < 2U; ++index) {
        const file_t *package = index == 0U ? current : other;
        assert(package->size <= SLOT_BYTES);
        uint8_t *bytes = fixture->store.flash + index * SLOT_BYTES;
        memcpy(bytes, package->bytes, package->size);
        if (damaged && index == 1U) bytes[512] ^= 1;
        bindings[index].present = true;
        bindings[index].package_present = true;
        bindings[index].slot = (uint8_t)index;
        bindings[index].package_size_bytes = (uint32_t)package->size;
        bindings[index].guest_abi_version = 2U;
        bindings[index].data_schema_version = 1U;
        memcpy(bindings[index].firmware_sha256,
               fixture->firmware.bootable_firmware_sha256[index], 32U);
        assert(SHA256(bytes, package->size, bindings[index].package_sha256));
    }
    assert(econtainer_slots_initialize(&fixture->io, &geometry, &fixture->firmware,
                                       bindings) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_load(&fixture->io, &geometry, &fixture->state) == ECONTAINER_SLOTS_OK);
    econtainer_slot_selection_request_t request = request_for(fixture, false);
    expect_rejection(fixture, &request, ECONTAINER_SLOTS_UNTRUSTED);
    assert(fixture->store.mapped == fixture->store.unmapped); /* No retained mapping/runtime. */
    econtainer_slot_firmware_set_t prepared = fixture->firmware;
    memset(prepared.bootable_firmware_sha256[1], 0x33, 32U);
    econtainer_slot_operation_t operation = {.kind = ECONTAINER_SLOT_PACKAGE_REUSE};
    operation.operation_id[0] = 1U;
    memset(operation.target_firmware_sha256, 0x33, 32U);
    operation.slot = bindings[0].slot;
    operation.package_size_bytes = bindings[0].package_size_bytes;
    operation.guest_abi_version = bindings[0].guest_abi_version;
    operation.data_schema_version = bindings[0].data_schema_version;
    memcpy(operation.package_sha256, bindings[0].package_sha256, 32U);
    assert(econtainer_slots_stage_firmware(&fixture->io, &geometry, fixture->state.sequence,
        &prepared, &operation, econtainer_package_slot_validate_binding,
        &fixture->validation, &fixture->state) == ECONTAINER_SLOTS_OK);
    fixture->firmware = prepared;
    request = request_for(fixture, false);
    econtainer_runtime_t *runtime = NULL;
    const econtainer_slot_runtime_result_t result = open_request(fixture, &request, &runtime);
    assert(result.slots == ECONTAINER_SLOTS_OK && result.runtime == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    assert(!fixture->store.locked && fixture->store.mapped == fixture->store.unmapped);
    assert(pthread_cond_destroy(&fixture->store.condition) == 0);
    assert(pthread_mutex_destroy(&fixture->store.gate) == 0);
    assert(pthread_mutex_destroy(&fixture->store.lock) == 0);
    free(fixture);
}

static bool run_required_runtime_grant(const file_t *key, const file_t *package)
{
    fixture_t *fixture = malloc(sizeof(*fixture));
    assert(fixture != NULL);
    initialize(fixture, key);
    prepare(fixture, package);
    begin_trial(fixture);
    bool passed = true;
    for (unsigned selection = 0; selection < 2; ++selection) {
        const econtainer_slot_selection_request_t request = request_for(fixture, selection == 0);
        fixture->limits.allowed_capabilities = 0;
        fixture->limits.max_log_bytes = 0;
        fixture->limits.max_timers = 0;
        econtainer_runtime_t *runtime = NULL;
        const unsigned before_map = fixture->store.mapped;
        const econtainer_slot_runtime_result_t rejected = open_request(fixture, &request, &runtime);
        const uint8_t empty_digest[32] = {0};
        const bool metadata_zero = rejected.event_queue_limit == 0U &&
            memcmp(rejected.package_sha256, empty_digest, sizeof empty_digest) == 0 &&
            rejected.product_id_offset_bytes == 0U && rejected.product_id_size_bytes == 0U &&
            rejected.product_version_offset_bytes == 0U && rejected.product_version_size_bytes == 0U &&
            rejected.guest_abi_version == 0U && rejected.data_schema_version == 0U;
        const bool released = !fixture->store.locked && fixture->store.mapping == NULL &&
            fixture->store.mapped == before_map + 1U &&
            fixture->store.mapped == fixture->store.unmapped;
        const bool denied = rejected.slots == ECONTAINER_SLOTS_OK &&
            rejected.runtime == ECONTAINER_RUNTIME_NOT_AUTHORIZED && runtime == NULL &&
            metadata_zero && released;
        printf("required_runtime_grant: selection=%s slots=%d runtime=%d out_null=%u metadata_zero=%u mapping_released=%u\n",
            selection == 0 ? "trial" : "confirmed", (int)rejected.slots,
            (int)rejected.runtime, (unsigned)(runtime == NULL),
            (unsigned)metadata_zero, (unsigned)released);
        if (!denied) {
            /* Red evidence exits normally after closing the unexpectedly live
             * instance; it must not leave a runtime claim or mapping behind. */
            if (runtime != NULL) {
                const econtainer_runtime_result_t initialized = econtainer_product_init(runtime);
                printf("required_runtime_grant: unexpected_init=%d close=%d\n", (int)initialized,
                    (int)econtainer_product_close(&runtime));
                assert(runtime == NULL);
            }
            passed = false;
            break;
        }
        fixture->limits.allowed_capabilities = ECONTAINER_CAP_ALL;
        fixture->limits.max_log_bytes = 16;
        fixture->limits.max_timers = 1;
        econtainer_slot_runtime_result_t accepted = open_request(fixture, &request, &runtime);
        assert(accepted.slots == ECONTAINER_SLOTS_OK && accepted.runtime == ECONTAINER_RUNTIME_OK && runtime != NULL);
        assert(accepted.event_queue_limit == 8U && accepted.product_id_size_bytes == 7U &&
               accepted.product_version_size_bytes == 19U && accepted.guest_abi_version == 2U &&
               accepted.data_schema_version == 1U);
        assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
        const uint8_t event[] = {1, 2, 3};
        int32_t guest_result = -1;
        assert(econtainer_product_on_event(runtime, event, sizeof event, &guest_result) == ECONTAINER_RUNTIME_OK && guest_result == 3);
        assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
        assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
        assert(!fixture->store.locked && fixture->store.mapping == NULL &&
               fixture->store.mapped == fixture->store.unmapped);
        if (selection == 0) confirm(fixture);
    }
    assert(pthread_cond_destroy(&fixture->store.condition) == 0);
    assert(pthread_mutex_destroy(&fixture->store.gate) == 0);
    assert(pthread_mutex_destroy(&fixture->store.lock) == 0);
    free(fixture);
    return passed;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    file_t key = read_file(argv[1], "public.der");
    file_t required_timer = read_file(argv[1], "required-timer.pkg");
    const bool required_grant_passed = run_required_runtime_grant(&key, &required_timer);
    free(required_timer.bytes);
    if (!required_grant_passed) {
        free(key.bytes);
        return 1;
    }
    const char *names[] = {"p0.pkg", "p1.pkg", "p2.pkg", "p3.pkg",
                           "host.pkg", "budget.pkg", "stack.pkg", "bad-loader.pkg",
                           "timer.pkg", "message-counter.pkg"};
    file_t packages[10];
    for (unsigned index = 0; index < 10; ++index) packages[index] = read_file(argv[1], names[index]);
    file_t identity = read_file(argv[1], "identity.pkg");
    assert(identity.size == packages[0].size &&
           memcmp(identity.bytes, packages[0].bytes, identity.size) != 0);
    run_retained_reference_identity(&key, &packages[0], &identity, false);
    run_retained_reference_identity(&key, &packages[0], &identity, true);
    free(identity.bytes);
    fixture_t *fixture = malloc(sizeof(*fixture));
    assert(fixture != NULL);
    initialize(fixture, &key);
    for (unsigned index = 0; index < 4; ++index) {
        char name[32];
        (void)snprintf(name, sizeof name, "p%u-version.txt", index);
        fixture->counter_versions[index] = read_file(argv[1], name);
        assert(SHA256(packages[index].bytes, packages[index].size,
                      fixture->counter_package_sha256[index]) != NULL);
    }
    econtainer_slot_selection_request_t request = request_for(fixture, false);
    expect_rejection(fixture, &request, ECONTAINER_SLOTS_EMPTY);
    for (unsigned index = 0; index < 4; ++index) {
        /* P0 remains bound to firmware A; firmware B progresses P1 -> P2 -> P3. */
        if (index == 1) memcpy(fixture->firmware.running_firmware_sha256,
                               fixture->firmware.bootable_firmware_sha256[1], 32);
        prepare(fixture, &packages[index]);
        request = request_for(fixture, true);
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_CONFLICT); /* PREPARED */
        begin_trial(fixture);
        request = request_for(fixture, true);
        econtainer_slot_selection_request_t wrong = request;
        ++wrong.expected_sequence;
        expect_rejection(fixture, &wrong, ECONTAINER_SLOTS_CONFLICT);
        wrong = request; ++wrong.boot_id[0];
        expect_rejection(fixture, &wrong, ECONTAINER_SLOTS_CONFLICT);
        wrong = request; ++wrong.operation_id[0];
        expect_rejection(fixture, &wrong, ECONTAINER_SLOTS_CONFLICT);
        wrong = request;
        memcpy(wrong.firmware_set.running_firmware_sha256,
               fixture->firmware.bootable_firmware_sha256[index == 0 ? 1 : 0], 32);
        expect_rejection(fixture, &wrong, ECONTAINER_SLOTS_CONFLICT);
        wrong = request; wrong.firmware_set.bootable_firmware_sha256[1][0] ^= 1;
        expect_rejection(fixture, &wrong, index == 0 ? ECONTAINER_SLOTS_CONFLICT : ECONTAINER_SLOTS_INVALID);
        fixture->store.map_fail = true;
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_IO_FAILED);
        fixture->store.map_fail = false;
        fixture->store.map_null = true;
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_IO_FAILED);
        fixture->store.map_null = false;
        fixture->store.unmap_fail = true;
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_IO_FAILED);
        fixture->store.unmap_fail = false;
        fixture->store.wrong_mapping = &packages[(index + 1) % 4];
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_UNTRUSTED);
        fixture->store.wrong_mapping = NULL;
        fixture->validation.expected_product_id = "other";
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_UNTRUSTED);
        fixture->validation.expected_product_id = "counter";
        fixture->validation.max_instruction_budget = 99999;
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_UNTRUSTED);
        fixture->validation.max_instruction_budget = 100000;
        fixture->validation.max_instruction_budget = UINT32_MAX;
        expect_rejection(fixture, &request, ECONTAINER_SLOTS_UNTRUSTED);
        fixture->validation.max_instruction_budget = 100000;
        fixture->limits.stack_size_bytes = 0;
        econtainer_runtime_t *invalid = NULL;
        const econtainer_slot_runtime_result_t invalid_result = open_request(fixture, &request, &invalid);
        assert(invalid_result.slots == ECONTAINER_SLOTS_OK &&
               invalid_result.runtime == ECONTAINER_RUNTIME_INVALID_INPUT && invalid == NULL);
        assert(fixture->store.mapped == fixture->store.unmapped && !fixture->store.locked);
        fixture->limits.stack_size_bytes = 16384;
        /* P1 -> P2 changes only the signed guest source, on firmware B in
         * the same boot. A version string change alone cannot prove this. */
        assert(index < 1 || memcmp(fixture->firmware.running_firmware_sha256,
            fixture->firmware.bootable_firmware_sha256[1], 32) == 0);
        run_counter(fixture, true, index < 2 ? 3 : 6);
        confirm(fixture);
        run_counter(fixture, false, index < 2 ? 3 : 6);
        assert(fixture->state.bindings[0].slot == 0);
        assert(memcmp(fixture->store.flash, packages[0].bytes, packages[0].size) == 0);
        if (index > 0) assert(fixture->state.bindings[1].slot == (index % 2 != 0 ? 1 : 2));
    }
    /* Confirmed startup recovers P3 even when a prepared candidate is invalid. */
    prepare(fixture, &packages[2]);
    const size_t candidate_offset = geometry.slots[fixture->state.operation.slot].offset_bytes - FLASH_BASE;
    fixture->store.flash[candidate_offset] ^= 1;
    run_counter(fixture, false, 6);
    fixture->store.flash[candidate_offset] ^= 1;
    /* But corruption/read failure in the other bootable firmware's reference blocks startup. */
    request = request_for(fixture, false);
    fixture->store.flash[0] ^= 1;
    expect_rejection(fixture, &request, ECONTAINER_SLOTS_UNTRUSTED);
    fixture->store.flash[0] ^= 1;
    fixture->store.read_fail = true;
    expect_rejection(fixture, &request, ECONTAINER_SLOTS_IO_FAILED);
    fixture->store.read_fail = false;
    begin_trial(fixture);
    fixture->store.race = true;
    const unsigned erases = fixture->store.erases, writes = fixture->store.writes;
    pthread_t writer;
    assert(pthread_create(&writer, NULL, competing_writer, fixture) == 0);
    run_counter(fixture, true, 6);
    assert(pthread_join(writer, NULL) == 0);
    fixture->store.race = false;
    assert(fixture->store.erases == erases && fixture->store.writes == writes);
    abandon(fixture);
    /* Global runtime BUSY must release only this attempt's mapping. */
    request = request_for(fixture, false);
    econtainer_runtime_t *runtime = NULL, *second = NULL;
    econtainer_slot_runtime_result_t result = open_request(fixture, &request, &runtime);
    assert(result.slots == ECONTAINER_SLOTS_OK && result.runtime == ECONTAINER_RUNTIME_OK);
    result = open_request(fixture, &request, &second);
    assert(result.slots == ECONTAINER_SLOTS_OK && result.runtime == ECONTAINER_RUNTIME_BUSY && second == NULL);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
    for (unsigned index = 4; index < 8; ++index) {
        prepare(fixture, &packages[index]);
        begin_trial(fixture);
        request = request_for(fixture, true);
        if (index == 4) {
            fixture->validation.wasm_authorization.allowed_capabilities = 0;
            expect_rejection(fixture, &request, ECONTAINER_SLOTS_UNTRUSTED);
            fixture->validation.wasm_authorization.allowed_capabilities = ECONTAINER_CAP_ALL;
            fixture->limits.allowed_capabilities = 0;
            result = open_request(fixture, &request, &runtime);
            assert(result.slots == ECONTAINER_SLOTS_OK && result.runtime == ECONTAINER_RUNTIME_NOT_AUTHORIZED);
            const uint8_t empty_digest[32] = {0};
            assert(result.event_queue_limit == 0U &&
                   memcmp(result.package_sha256, empty_digest, 32) == 0);
            assert(runtime == NULL);
            fixture->limits.allowed_capabilities = ECONTAINER_CAP_ALL;
        }
        result = open_request(fixture, &request, &runtime);
        assert(result.slots == ECONTAINER_SLOTS_OK);
        if (index == 7) {
            assert(result.runtime == ECONTAINER_RUNTIME_BAD_WASM && runtime == NULL);
        } else {
            assert(result.runtime == ECONTAINER_RUNTIME_OK && runtime != NULL);
            assert(fixture->store.mapping == NULL && !fixture->store.locked);
            const econtainer_runtime_result_t initialized = econtainer_product_init(runtime);
            if (index == 4) {
                assert(initialized == ECONTAINER_RUNTIME_OK);
                uint8_t log[16]; size_t size = 0;
                assert(econtainer_product_take_log(runtime, log, sizeof(log), &size) == ECONTAINER_RUNTIME_OK);
                assert(size == 4 && memcmp(log, "init", 4) == 0);
                const uint8_t event = 2; int32_t value = 0;
                assert(econtainer_product_on_event(runtime, &event, 1, &value) == ECONTAINER_RUNTIME_OK);
                assert(value == -2);
                assert(econtainer_product_take_log(runtime, log, sizeof(log), &size) == ECONTAINER_RUNTIME_OK);
                assert(size == 5 && memcmp(log, "first", 5) == 0);
                assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
            } else if (index == 5) {
                assert(initialized == ECONTAINER_RUNTIME_INSTRUCTION_LIMIT);
            } else {
                assert(initialized == ECONTAINER_RUNTIME_ENGINE_FAILURE);
            }
            assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK);
        }
        abandon(fixture);
        run_counter(fixture, false, 6);
    }
    /* Public owner-driven timer delivery uses the same signed slot path. */
    fixture->limits.max_timers = 2;
    prepare(fixture, &packages[8]);
    begin_trial(fixture);
    request = request_for(fixture, true);
    runtime = NULL;
    result = open_request(fixture, &request, &runtime);
    assert(result.slots == ECONTAINER_SLOTS_OK && result.runtime == ECONTAINER_RUNTIME_OK);
    assert(runtime != NULL && !fixture->store.locked && fixture->store.mapping == NULL);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    uint64_t deadline_ms = 0;
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) == ECONTAINER_RUNTIME_NO_TIMER);
    const uint8_t schedule = 'A';
    int32_t guest_result = -1;
    assert(econtainer_product_on_event(runtime, &schedule, 1, &guest_result) == ECONTAINER_RUNTIME_OK);
    assert(guest_result == 0);
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) == ECONTAINER_RUNTIME_OK);
    econtainer_timer_event_t timer_event = {0};
    struct timespec wait = {.tv_sec = 0, .tv_nsec = 40000000};
    assert(nanosleep(&wait, NULL) == 0);
    assert(econtainer_product_poll_timer(runtime, &timer_event, &guest_result) == ECONTAINER_RUNTIME_OK);
    assert(timer_event.handle != 0 && guest_result == 10);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_poll_timer(runtime, &timer_event, &guest_result) == ECONTAINER_RUNTIME_INVALID_STATE);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    abandon(fixture);
    run_message_counter(fixture, &packages[9]);
#ifdef ECONTAINER_TEST_HOST_CLOCK
    file_t host_calls = read_file(argv[1], "host-calls.pkg");
    run_host_call_deadlines(fixture, &host_calls);
    free(host_calls.bytes);
#endif
    assert(fixture->store.mapped == fixture->store.unmapped && !fixture->store.locked);
    assert(pthread_cond_destroy(&fixture->store.condition) == 0);
    assert(pthread_mutex_destroy(&fixture->store.gate) == 0);
    assert(pthread_mutex_destroy(&fixture->store.lock) == 0);
    printf("slot_runtime: P0-P3, exact identity/grants, transient mappings=%u, competing writers, real WAMR passed\n",
           fixture->store.mapped);
    for (unsigned index = 0; index < 4; ++index) free(fixture->counter_versions[index].bytes);
    free(fixture);
    for (unsigned index = 0; index < 10; ++index) free(packages[index].bytes);
    free(key.bytes);
    return 0;
}
