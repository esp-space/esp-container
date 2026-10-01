#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif

#include "runtime_internal.h"

#include "esp_container.h"
#include "wasm_export.h"

#if WASM_ENABLE_FAST_INTERP != 0 || WASM_ENABLE_CUSTOM_NAME_SECTION != 0 || \
    WASM_ENABLE_LOAD_CUSTOM_SECTION != 0 || WASM_ENABLE_DEBUG_INTERP != 0 || \
    WASM_ENABLE_SHRUNK_MEMORY != 0
#error "Container requires Classic bytecode, standard pages and no retained custom sections"
#endif
#if WASM_ENABLE_CLASSIC_WALL_CLOCK_LIMIT == 0
#error "Container requires the Classic cooperative wall clock limit"
#endif

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifndef ESP_PLATFORM
#include <time.h>
#else
#include "esp_timer.h"
#endif

#define ECONTAINER_TIMER_CAPACITY 8U
#define ECONTAINER_TIMER_MAX_INTERVAL_MS 86400000U
#define ECONTAINER_ENTRY_EXPIRED_REASON "container entry expired"

typedef struct {
    uint64_t handle;
    uint64_t deadline_ms;
    uint32_t period_ms;
    bool active;
} econtainer_timer_t;

struct econtainer_runtime {
    econtainer_runtime_limits_t limits;
    econtainer_runtime_state_t state;
    uint8_t *code_copy;
    uint8_t *data_copy;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t environment;
    wasm_function_inst_t init_function;
    wasm_function_inst_t event_function;
    wasm_function_inst_t stop_function;
    uint32_t event_buffer_offset;
    uint8_t *event_buffer;
    NativeSymbol native_symbols[4];
    uint32_t native_count;
    uint8_t *pending_log;
    size_t pending_log_size;
    atomic_bool call_active;
    bool guest_active;
    bool entry_expired;
    bool entry_cancelled;
    uint64_t entry_deadline_us;
    bool stopping;
    bool natives_registered;
    bool wamr_initialized;
    econtainer_timer_t timers[ECONTAINER_TIMER_CAPACITY];
};

/* WAMR init/destroy owns process-global state. The caller serializes methods
 * on the accepted instance; the atomic claim also rejects a second opener. */
static atomic_bool runtime_claimed = false;
/* runtime_claimed and the single guest owner serialize every allocation. */
static uint64_t next_timer_handle = 1;

static bool limits_valid(const econtainer_runtime_limits_t *limits)
{
    return limits != NULL && limits->max_wasm_bytes > 0 &&
           limits->max_memory_pages == 1 && limits->stack_size_bytes > 0 &&
           limits->max_event_bytes > 0 &&
           limits->max_event_bytes <= ECONTAINER_EVENT_BUFFER_BYTES &&
           (limits->allowed_capabilities & (uint32_t)~ECONTAINER_CAP_ALL) == 0 &&
           (limits->allowed_capabilities & ECONTAINER_CAP_LOG
                ? limits->max_log_bytes > 0 && limits->max_log_bytes <= 256
                : limits->max_log_bytes == 0) &&
           (limits->allowed_capabilities & ECONTAINER_CAP_TIMER
                ? limits->max_timers > 0 &&
                  limits->max_timers <= ECONTAINER_TIMER_CAPACITY &&
                  limits->max_event_bytes >= 16
                : limits->max_timers == 0) &&
           limits->init_instruction_budget > 0 &&
           limits->event_instruction_budget > 0 &&
           limits->stop_instruction_budget > 0 &&
           limits->max_entry_duration_ms > 0;
}

static bool function_type_matches(wasm_func_type_t type, uint32_t param_count)
{
    if (type == NULL || wasm_func_type_get_param_count(type) != param_count ||
        wasm_func_type_get_result_count(type) != 1 ||
        wasm_func_type_get_result_valkind(type, 0) != WASM_I32) {
        return false;
    }
    for (uint32_t index = 0; index < param_count; ++index) {
        if (wasm_func_type_get_param_valkind(type, index) != WASM_I32) {
            return false;
        }
    }
    return true;
}

static bool module_abi_matches(wasm_module_t module)
{
    const int32_t count = wasm_runtime_get_export_count(module);
    if (count != 5) {
        return false;
    }
    bool init_found = false;
    bool event_found = false;
    bool stop_found = false;
    bool memory_found = false;
    bool buffer_found = false;
    for (int32_t index = 0; index < count; ++index) {
        wasm_export_t export_type;
        wasm_runtime_get_export_type(module, index, &export_type);
        if (export_type.name == NULL) {
            return false;
        }
        if (strcmp(export_type.name, "econtainer_init") == 0) {
            if (init_found || export_type.kind != WASM_IMPORT_EXPORT_KIND_FUNC ||
                !function_type_matches(export_type.u.func_type, 0)) {
                return false;
            }
            init_found = true;
        }
        else if (strcmp(export_type.name, "econtainer_on_event") == 0) {
            if (event_found || export_type.kind != WASM_IMPORT_EXPORT_KIND_FUNC ||
                !function_type_matches(export_type.u.func_type, 2)) {
                return false;
            }
            event_found = true;
        }
        else if (strcmp(export_type.name, "econtainer_stop") == 0) {
            if (stop_found || export_type.kind != WASM_IMPORT_EXPORT_KIND_FUNC ||
                !function_type_matches(export_type.u.func_type, 0)) {
                return false;
            }
            stop_found = true;
        }
        else if (strcmp(export_type.name, "memory") == 0) {
            if (memory_found || export_type.kind != WASM_IMPORT_EXPORT_KIND_MEMORY ||
                export_type.u.memory_type == NULL ||
                wasm_memory_type_get_shared(export_type.u.memory_type)) {
                return false;
            }
            memory_found = true;
        }
        else if (strcmp(export_type.name, "econtainer_event_buffer") == 0) {
            if (buffer_found || export_type.kind != WASM_IMPORT_EXPORT_KIND_GLOBAL ||
                export_type.u.global_type == NULL ||
                wasm_global_type_get_valkind(export_type.u.global_type) != WASM_I32 ||
                wasm_global_type_get_mutable(export_type.u.global_type)) {
                return false;
            }
            buffer_found = true;
        }
        else {
            return false;
        }
    }
    return init_found && event_found && stop_found && memory_found && buffer_found;
}

static bool bind_event_buffer(econtainer_runtime_t *runtime)
{
    wasm_memory_inst_t memory = wasm_runtime_get_default_memory(runtime->instance);
    wasm_global_inst_t buffer;
    if (memory == NULL || wasm_memory_get_cur_page_count(memory) != 1 ||
        wasm_memory_get_max_page_count(memory) != 1 ||
        wasm_memory_get_bytes_per_page(memory) != 65536 ||
        !wasm_runtime_get_export_global_inst(runtime->instance,
                                             "econtainer_event_buffer", &buffer) ||
        buffer.kind != WASM_I32 || buffer.is_mutable || buffer.global_data == NULL) {
        return false;
    }
    uint32_t offset = 0;
    memcpy(&offset, buffer.global_data, sizeof(offset));
    /* Subtraction keeps negative i32 bit patterns and integer wrap out. */
    if (offset == 0 || offset > 65536U - ECONTAINER_EVENT_BUFFER_BYTES ||
        !wasm_runtime_validate_app_addr(runtime->instance, offset,
                                        ECONTAINER_EVENT_BUFFER_BYTES)) {
        return false;
    }
    runtime->event_buffer = wasm_runtime_addr_app_to_native(runtime->instance, offset);
    runtime->event_buffer_offset = offset;
    return runtime->event_buffer != NULL;
}

static void release_runtime(econtainer_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    if (runtime->environment != NULL) {
        wasm_runtime_destroy_exec_env(runtime->environment);
    }
    if (runtime->instance != NULL) {
        wasm_runtime_deinstantiate(runtime->instance);
    }
    if (runtime->module != NULL) {
        wasm_runtime_unload(runtime->module);
    }
    if (runtime->natives_registered) {
        wasm_runtime_unregister_natives("econtainer", runtime->native_symbols);
    }
    free(runtime->pending_log);
    free(runtime->code_copy);
    free(runtime->data_copy);
    if (runtime->wamr_initialized) {
        wasm_runtime_destroy();
    }
    free(runtime);
    atomic_store(&runtime_claimed, false);
}

static bool claim_call(econtainer_runtime_t *runtime)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&runtime->call_active, &expected, true);
}

static bool monotonic_us(uint64_t *result);
static bool monotonic_ms(uint64_t *result);

static bool cancellation_requested(void *context)
{
    econtainer_runtime_t *runtime = context;
    if (!runtime->stopping && runtime->limits.cancel_requested != NULL &&
        runtime->limits.cancel_requested(runtime->limits.cancel_context)) {
        runtime->entry_cancelled = true;
        return true;
    }
    return false;
}

static econtainer_runtime_t *native_owner(wasm_exec_env_t environment)
{
    econtainer_runtime_t *runtime =
        wasm_runtime_get_function_attachment(environment);
    if (runtime == NULL || !runtime->guest_active ||
        runtime->environment != environment ||
        runtime->instance != wasm_runtime_get_module_inst(environment) ||
        !atomic_load(&runtime->call_active)) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                                   "Exception: invalid container host call owner");
        return NULL;
    }
    uint64_t now_us = 0;
    if (!monotonic_us(&now_us)) {
        wasm_runtime_set_exception(runtime->instance,
                                   "Exception: monotonic clock failed");
        return NULL;
    }
    if (now_us >= runtime->entry_deadline_us) {
        runtime->entry_expired = true;
        wasm_runtime_set_exception(runtime->instance,
                                   ECONTAINER_ENTRY_EXPIRED_REASON);
        return NULL;
    }
    if (cancellation_requested(runtime)) {
        wasm_runtime_set_exception(runtime->instance, "execution cancelled");
        return NULL;
    }
    return runtime;
}

static bool monotonic_us(uint64_t *result)
{
#ifdef ESP_PLATFORM
    const int64_t now_us = esp_timer_get_time();
    if (now_us < 0) return false;
    *result = (uint64_t)now_us;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return false;
    const uint64_t fractional_us = (uint64_t)now.tv_nsec / 1000U;
    if ((uint64_t)now.tv_sec > (UINT64_MAX - fractional_us) / 1000000U)
        return false;
    *result = (uint64_t)now.tv_sec * 1000000U + fractional_us;
#endif
    return true;
}

static bool monotonic_ms(uint64_t *result)
{
    uint64_t now_us = 0;
    if (!monotonic_us(&now_us)) return false;
    *result = now_us / 1000U;
    return true;
}

static uint64_t native_monotonic_ms(wasm_exec_env_t environment)
{
    econtainer_runtime_t *runtime = native_owner(environment);
    if (runtime == NULL ||
        (runtime->limits.allowed_capabilities & ECONTAINER_CAP_MONOTONIC_TIME) == 0) {
        if (runtime != NULL) {
            wasm_runtime_set_exception(runtime->instance,
                                       "Exception: container clock not authorized");
        }
        return 0;
    }
    uint64_t now_ms = 0;
    if (!monotonic_ms(&now_ms)) {
        wasm_runtime_set_exception(runtime->instance,
                                   "Exception: monotonic clock failed");
        return 0;
    }
    return now_ms;
}

static int32_t native_log(wasm_exec_env_t environment, uint32_t offset,
                          uint32_t size_bytes)
{
    econtainer_runtime_t *runtime = native_owner(environment);
    if (runtime == NULL ||
        (runtime->limits.allowed_capabilities & ECONTAINER_CAP_LOG) == 0) {
        if (runtime != NULL) {
            wasm_runtime_set_exception(runtime->instance,
                                       "Exception: container log not authorized");
        }
        return -1;
    }
    if (size_bytes == 0 || size_bytes > runtime->limits.max_log_bytes) {
        return -1;
    }
    if (runtime->pending_log_size != 0) {
        return -2;
    }
    if (!wasm_runtime_validate_app_addr(runtime->instance, offset, size_bytes)) {
        return -1;
    }
    const uint8_t *source = wasm_runtime_addr_app_to_native(runtime->instance, offset);
    if (source == NULL) {
        wasm_runtime_set_exception(runtime->instance,
                                   "Exception: invalid container log buffer");
        return -1;
    }
    memcpy(runtime->pending_log, source, size_bytes);
    runtime->pending_log_size = size_bytes;
    return 0;
}

static uint64_t native_timer_start(wasm_exec_env_t environment,
                                   uint32_t delay_ms, uint32_t period_ms)
{
    econtainer_runtime_t *runtime = native_owner(environment);
    if (runtime == NULL ||
        (runtime->limits.allowed_capabilities & ECONTAINER_CAP_TIMER) == 0) {
        if (runtime != NULL)
            wasm_runtime_set_exception(runtime->instance,
                                       "Exception: container timer not authorized");
        return 0;
    }
    if (runtime->stopping ||
        (runtime->state != ECONTAINER_RUNTIME_LOADED &&
         runtime->state != ECONTAINER_RUNTIME_RUNNING) ||
        delay_ms == 0 || delay_ms > ECONTAINER_TIMER_MAX_INTERVAL_MS ||
        period_ms > ECONTAINER_TIMER_MAX_INTERVAL_MS ||
        next_timer_handle == 0) return 0;
    uint32_t slot = runtime->limits.max_timers;
    for (uint32_t index = 0; index < runtime->limits.max_timers; ++index) {
        if (!runtime->timers[index].active) { slot = index; break; }
    }
    if (slot == runtime->limits.max_timers) return 0;
    uint64_t now_ms = 0;
    if (!monotonic_ms(&now_ms)) {
        wasm_runtime_set_exception(runtime->instance,
                                   "Exception: monotonic clock failed");
        return 0;
    }
    if (now_ms > UINT64_MAX - delay_ms) return 0;
    const uint64_t handle = econtainer_runtime_issue_timer_handle(&next_timer_handle);
    runtime->timers[slot] = (econtainer_timer_t){
        .handle = handle, .deadline_ms = now_ms + delay_ms,
        .period_ms = period_ms, .active = true,
    };
    return handle;
}

static bool read_section_size(const uint8_t *wasm, size_t wasm_size_bytes,
                              size_t *cursor, uint32_t *size_bytes)
{
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (*cursor >= wasm_size_bytes) return false;
        const uint8_t byte = wasm[(*cursor)++];
        if (shift == 28 && (byte & 0xf0U) != 0) return false;
        value |= (uint32_t)(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            *size_bytes = value;
            return true;
        }
    }
    return false;
}

/* The exact Classic loader copies names when given a section list. It keeps
 * function code and active data by pointer, so only those bodies need owned,
 * writable storage after this call. All other bodies are read during load.
 * The caller's original Wasm can be read-only and released after open. */
static econtainer_runtime_result_t prepare_sections(
    const uint8_t *wasm, size_t wasm_size_bytes, econtainer_runtime_t *runtime,
    wasm_section_t **out_sections)
{
    size_t section_count = 0;
    size_t cursor = 8;
    while (cursor < wasm_size_bytes) {
        ++cursor; /* section id, already checked by the module scanner */
        uint32_t body_size_bytes = 0;
        if (!read_section_size(wasm, wasm_size_bytes, &cursor, &body_size_bytes) ||
            body_size_bytes > wasm_size_bytes - cursor) {
            return ECONTAINER_RUNTIME_BAD_WASM;
        }
        cursor += body_size_bytes;
        ++section_count;
    }
    if (section_count == 0 || section_count > SIZE_MAX / sizeof(wasm_section_t)) {
        return ECONTAINER_RUNTIME_BAD_WASM;
    }
    wasm_section_t *sections = calloc(section_count, sizeof(*sections));
    if (sections == NULL) return ECONTAINER_RUNTIME_NO_MEMORY;
    cursor = 8;
    for (size_t index = 0; index < section_count; ++index) {
        const uint8_t section_id = wasm[cursor++];
        uint32_t body_size_bytes = 0;
        if (!read_section_size(wasm, wasm_size_bytes, &cursor, &body_size_bytes) ||
            body_size_bytes > wasm_size_bytes - cursor) {
            free(sections);
            return ECONTAINER_RUNTIME_BAD_WASM;
        }
        sections[index].section_type = section_id;
        sections[index].section_body = (uint8_t *)(wasm + cursor);
        sections[index].section_body_size = body_size_bytes;
        sections[index].next = index + 1 < section_count ? &sections[index + 1] : NULL;
        if ((section_id == 10 || section_id == 11) && body_size_bytes > 0) {
            uint8_t **owned = section_id == 10 ? &runtime->code_copy :
                                                &runtime->data_copy;
            if (*owned != NULL) {
                free(sections);
                return ECONTAINER_RUNTIME_BAD_WASM;
            }
            *owned = malloc(body_size_bytes);
            if (*owned == NULL) {
                free(sections);
                return ECONTAINER_RUNTIME_NO_MEMORY;
            }
            memcpy(*owned, wasm + cursor, body_size_bytes);
            sections[index].section_body = *owned;
        }
        cursor += body_size_bytes;
    }
    *out_sections = sections;
    return ECONTAINER_RUNTIME_OK;
}

static int32_t native_timer_cancel(wasm_exec_env_t environment, uint64_t handle)
{
    econtainer_runtime_t *runtime = native_owner(environment);
    if (runtime == NULL ||
        (runtime->limits.allowed_capabilities & ECONTAINER_CAP_TIMER) == 0) {
        if (runtime != NULL)
            wasm_runtime_set_exception(runtime->instance,
                                       "Exception: container timer not authorized");
        return -1;
    }
    if (runtime->stopping || handle == 0) return -1;
    for (uint32_t index = 0; index < runtime->limits.max_timers; ++index) {
        if (runtime->timers[index].active &&
            runtime->timers[index].handle == handle) {
            runtime->timers[index].active = false;
            return 0;
        }
    }
    return -1;
}

econtainer_runtime_result_t econtainer_runtime_open(const uint8_t *wasm,
                                                   size_t wasm_size_bytes,
                                                   const econtainer_runtime_limits_t *limits,
                                                   econtainer_runtime_t **out)
{
    if (out == NULL || *out != NULL || wasm == NULL ||
        wasm_size_bytes == 0 || wasm_size_bytes > UINT32_MAX ||
        !limits_valid(limits) || wasm_size_bytes > limits->max_wasm_bytes) {
        return ECONTAINER_RUNTIME_INVALID_INPUT;
    }
    uint32_t required_capabilities = 0;
    if (!econtainer_wasm_imported_capabilities(wasm, wasm_size_bytes,
                                               &required_capabilities)) {
        return ECONTAINER_RUNTIME_BAD_WASM;
    }
    if ((required_capabilities & ~limits->allowed_capabilities) != 0) {
        return ECONTAINER_RUNTIME_NOT_AUTHORIZED;
    }
    if (!econtainer_wasm_memory_within_limit(wasm, wasm_size_bytes,
                                             limits->max_memory_pages)) {
        return ECONTAINER_RUNTIME_BAD_ABI;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&runtime_claimed, &expected, true)) {
        return ECONTAINER_RUNTIME_BUSY;
    }
    econtainer_runtime_t *runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) {
        atomic_store(&runtime_claimed, false);
        return ECONTAINER_RUNTIME_NO_MEMORY;
    }
    runtime->limits = *limits;
    runtime->state = ECONTAINER_RUNTIME_LOADED;
    atomic_init(&runtime->call_active, false);
    if ((required_capabilities & ECONTAINER_CAP_LOG) != 0) {
        runtime->pending_log = malloc(limits->max_log_bytes);
        if (runtime->pending_log == NULL) {
            release_runtime(runtime);
            return ECONTAINER_RUNTIME_NO_MEMORY;
        }
    }
    wasm_section_t *sections = NULL;
    const econtainer_runtime_result_t section_result =
        prepare_sections(wasm, wasm_size_bytes, runtime, &sections);
    if (section_result != ECONTAINER_RUNTIME_OK) {
        release_runtime(runtime);
        return section_result;
    }
    if (!wasm_runtime_init()) {
        free(sections);
        release_runtime(runtime);
        return ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    runtime->wamr_initialized = true;
    if ((required_capabilities & ECONTAINER_CAP_MONOTONIC_TIME) != 0) {
        runtime->native_symbols[runtime->native_count++] = (NativeSymbol){
            "monotonic_ms", (void *)native_monotonic_ms, "()I", runtime
        };
    }
    if ((required_capabilities & ECONTAINER_CAP_LOG) != 0) {
        runtime->native_symbols[runtime->native_count++] = (NativeSymbol){
            "log", (void *)native_log, "(ii)i", runtime
        };
    }
    if ((required_capabilities & ECONTAINER_CAP_TIMER) != 0) {
        runtime->native_symbols[runtime->native_count++] = (NativeSymbol){
            "timer_start", (void *)native_timer_start, "(ii)I", runtime
        };
        runtime->native_symbols[runtime->native_count++] = (NativeSymbol){
            "timer_cancel", (void *)native_timer_cancel, "(I)i", runtime
        };
    }
    if (runtime->native_count != 0) {
        if (!wasm_runtime_register_natives("econtainer", runtime->native_symbols,
                                           runtime->native_count)) {
            free(sections);
            release_runtime(runtime);
            return ECONTAINER_RUNTIME_ENGINE_FAILURE;
        }
        runtime->natives_registered = true;
    }
    char error[128] = {0};
    runtime->module = wasm_runtime_load_from_sections(sections, false,
                                                      error, sizeof(error));
    free(sections);
    if (runtime->module == NULL) {
        release_runtime(runtime);
        return ECONTAINER_RUNTIME_BAD_WASM;
    }
    if (!module_abi_matches(runtime->module)) {
        release_runtime(runtime);
        return ECONTAINER_RUNTIME_BAD_ABI;
    }
    InstantiationArgs args = {
        .default_stack_size = limits->stack_size_bytes,
        .host_managed_heap_size = 0,
        .max_memory_pages = 0, /* raw Wasm memory limits were checked above */
    };
    runtime->instance = wasm_runtime_instantiate_ex(runtime->module, &args,
                                                    error, sizeof(error));
    if (runtime->instance == NULL) {
        release_runtime(runtime);
        return ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    if (!bind_event_buffer(runtime)) {
        release_runtime(runtime);
        return ECONTAINER_RUNTIME_BAD_ABI;
    }
    runtime->environment = wasm_runtime_create_exec_env(runtime->instance,
                                                         limits->stack_size_bytes);
    runtime->init_function = wasm_runtime_lookup_function(runtime->instance,
                                                           "econtainer_init");
    runtime->event_function = wasm_runtime_lookup_function(runtime->instance,
                                                            "econtainer_on_event");
    runtime->stop_function = wasm_runtime_lookup_function(runtime->instance,
                                                           "econtainer_stop");
    if (runtime->environment == NULL || runtime->init_function == NULL ||
        runtime->event_function == NULL || runtime->stop_function == NULL) {
        release_runtime(runtime);
        return ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    *out = runtime;
    return ECONTAINER_RUNTIME_OK;
}

static econtainer_runtime_result_t expire_entry(econtainer_runtime_t *runtime,
                                                size_t prior_log_size)
{
    runtime->state = ECONTAINER_RUNTIME_FAILED;
    runtime->pending_log_size = prior_log_size;
    for (uint32_t index = 0; index < runtime->limits.max_timers; ++index)
        runtime->timers[index].active = false;
    return ECONTAINER_RUNTIME_ENTRY_EXPIRED;
}

static econtainer_runtime_result_t cancel_entry(econtainer_runtime_t *runtime,
                                                size_t prior_log_size)
{
    runtime->entry_cancelled = true;
    runtime->state = ECONTAINER_RUNTIME_FAILED;
    runtime->pending_log_size = prior_log_size;
    for (uint32_t index = 0; index < runtime->limits.max_timers; ++index)
        runtime->timers[index].active = false;
    return ECONTAINER_RUNTIME_ENTRY_CANCELLED;
}

static econtainer_runtime_result_t invoke(econtainer_runtime_t *runtime,
                                          wasm_function_inst_t function,
                                          int32_t budget, uint32_t argc,
                                          uint32_t *arguments, int32_t *result)
{
    uint64_t began_us = 0;
    const uint64_t duration_us =
        (uint64_t)runtime->limits.max_entry_duration_ms * 1000U;
    if (!monotonic_us(&began_us) ||
        began_us > UINT64_MAX - duration_us) {
        runtime->state = ECONTAINER_RUNTIME_FAILED;
        return ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    const size_t prior_log_size = runtime->pending_log_size;
    runtime->entry_deadline_us = began_us + duration_us;
    runtime->entry_expired = false;
    runtime->entry_cancelled = false;
    wasm_runtime_clear_exception(runtime->instance);
    wasm_runtime_set_instruction_count_limit(runtime->environment, budget);
    wasm_runtime_set_classic_wall_clock_deadline_us(
        runtime->environment, runtime->entry_deadline_us);
    wasm_runtime_set_classic_cancel_callback(runtime->environment,
                                             cancellation_requested, runtime);
    runtime->guest_active = true;
    const bool call_ok = wasm_runtime_call_wasm(runtime->environment, function,
                                                argc, arguments);
    runtime->guest_active = false;
    wasm_runtime_set_classic_cancel_callback(runtime->environment, NULL, NULL);
    wasm_runtime_set_classic_wall_clock_deadline_us(runtime->environment, 0);
    if (runtime->entry_expired)
        return expire_entry(runtime, prior_log_size);
    const char *exception = wasm_runtime_get_exception(runtime->instance);
    if (runtime->entry_cancelled ||
        (exception != NULL &&
         strcmp(exception, "Exception: execution cancelled") == 0)) {
        return cancel_entry(runtime, prior_log_size);
    }
    if (!call_ok) {
        if (exception != NULL &&
            strcmp(exception, "Exception: wall clock deadline exceeded") == 0)
            return expire_entry(runtime, prior_log_size);
        runtime->state = ECONTAINER_RUNTIME_FAILED;
        return exception != NULL &&
                       strcmp(exception, "Exception: instruction limit exceeded") == 0
                   ? ECONTAINER_RUNTIME_INSTRUCTION_LIMIT
                   : ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    if (exception != NULL) {
        runtime->state = ECONTAINER_RUNTIME_FAILED;
        return ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    uint64_t ended_us = 0;
    if (!monotonic_us(&ended_us)) {
        runtime->state = ECONTAINER_RUNTIME_FAILED;
        return ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    if (ended_us >= runtime->entry_deadline_us)
        return expire_entry(runtime, prior_log_size);
    if (cancellation_requested(runtime))
        return cancel_entry(runtime, prior_log_size);
    *result = (int32_t)arguments[0];
    return ECONTAINER_RUNTIME_OK;
}

econtainer_runtime_result_t econtainer_runtime_init(econtainer_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ECONTAINER_RUNTIME_INVALID_STATE;
    }
    if (!claim_call(runtime)) {
        return ECONTAINER_RUNTIME_BUSY;
    }
    if (runtime->state != ECONTAINER_RUNTIME_LOADED) {
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_INVALID_STATE;
    }
    uint32_t arguments[2] = {0};
    int32_t result = -1;
    econtainer_runtime_result_t status = invoke(runtime, runtime->init_function,
                                                runtime->limits.init_instruction_budget,
                                                0, arguments, &result);
    if (status != ECONTAINER_RUNTIME_OK) {
        for (uint32_t index = 0; index < runtime->limits.max_timers; ++index)
            runtime->timers[index].active = false;
        atomic_store(&runtime->call_active, false);
        return status;
    }
    if (result != 0) {
        runtime->state = ECONTAINER_RUNTIME_FAILED;
        for (uint32_t index = 0; index < runtime->limits.max_timers; ++index)
            runtime->timers[index].active = false;
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_GUEST_FAILURE;
    }
    runtime->state = ECONTAINER_RUNTIME_RUNNING;
    atomic_store(&runtime->call_active, false);
    return ECONTAINER_RUNTIME_OK;
}

static econtainer_runtime_result_t call_event(econtainer_runtime_t *runtime,
                                               const uint8_t *event,
                                               size_t event_size_bytes,
                                               int32_t *guest_result)
{
    if (guest_result == NULL || event_size_bytes > runtime->limits.max_event_bytes ||
        (event_size_bytes > 0 && event == NULL)) {
        return ECONTAINER_RUNTIME_INVALID_INPUT;
    }
    uint32_t offset = 0;
    if (event_size_bytes > 0) {
        offset = runtime->event_buffer_offset;
        memcpy(runtime->event_buffer, event, event_size_bytes);
    }
    uint32_t arguments[2] = {(uint32_t)offset, (uint32_t)event_size_bytes};
    int32_t result = 0;
    const econtainer_runtime_result_t status = invoke(
        runtime, runtime->event_function, runtime->limits.event_instruction_budget,
        2, arguments, &result);
    if (offset != 0) {
        /* Serialized borrowing ends on every return, including traps. */
        memset(runtime->event_buffer, 0, event_size_bytes);
    }
    if (status == ECONTAINER_RUNTIME_OK) {
        *guest_result = result;
    }
    return status;
}

econtainer_runtime_result_t econtainer_runtime_on_event(econtainer_runtime_t *runtime,
                                                       const uint8_t *event,
                                                       size_t event_size_bytes,
                                                       int32_t *guest_result)
{
    if (runtime == NULL) return ECONTAINER_RUNTIME_INVALID_STATE;
    if (!claim_call(runtime)) return ECONTAINER_RUNTIME_BUSY;
    econtainer_runtime_result_t status = ECONTAINER_RUNTIME_INVALID_STATE;
    if (runtime->state == ECONTAINER_RUNTIME_RUNNING) {
        if (event != NULL && event_size_bytes == 16 &&
            event[0] == 'E' && event[1] == 'C' &&
            event[2] == 'T' && event[3] == 1) {
            status = ECONTAINER_RUNTIME_INVALID_INPUT;
        } else {
            status = call_event(runtime, event, event_size_bytes, guest_result);
        }
    }
    atomic_store(&runtime->call_active, false);
    return status;
}

econtainer_runtime_result_t econtainer_runtime_stop(econtainer_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ECONTAINER_RUNTIME_INVALID_STATE;
    }
    if (!claim_call(runtime)) {
        return ECONTAINER_RUNTIME_BUSY;
    }
    if (runtime->state == ECONTAINER_RUNTIME_STOPPED) {
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_OK;
    }
    if (runtime->state != ECONTAINER_RUNTIME_RUNNING &&
        !(runtime->state == ECONTAINER_RUNTIME_FAILED &&
          runtime->entry_cancelled)) {
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_INVALID_STATE;
    }
    /* Cancel before entering guest stop. A stop import cannot create a timer. */
    runtime->stopping = true;
    for (uint32_t index = 0; index < runtime->limits.max_timers; ++index)
        runtime->timers[index].active = false;
    uint32_t arguments[2] = {0};
    int32_t result = -1;
    econtainer_runtime_result_t status = invoke(runtime, runtime->stop_function,
                                                runtime->limits.stop_instruction_budget,
                                                0, arguments, &result);
    if (status != ECONTAINER_RUNTIME_OK) {
        atomic_store(&runtime->call_active, false);
        return status;
    }
    if (result != 0) {
        runtime->state = ECONTAINER_RUNTIME_FAILED;
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_GUEST_FAILURE;
    }
    runtime->state = ECONTAINER_RUNTIME_STOPPED;
    atomic_store(&runtime->call_active, false);
    return ECONTAINER_RUNTIME_OK;
}

econtainer_runtime_state_t econtainer_runtime_state(const econtainer_runtime_t *runtime)
{
    return runtime == NULL ? ECONTAINER_RUNTIME_FAILED : runtime->state;
}

econtainer_runtime_result_t econtainer_runtime_take_log(econtainer_runtime_t *runtime,
                                                       uint8_t *output,
                                                       size_t output_capacity,
                                                       size_t *log_size_bytes)
{
    if (runtime == NULL || log_size_bytes == NULL) {
        return ECONTAINER_RUNTIME_INVALID_INPUT;
    }
    if (!claim_call(runtime)) {
        return ECONTAINER_RUNTIME_BUSY;
    }
    econtainer_runtime_result_t status = ECONTAINER_RUNTIME_OK;
    if (runtime->pending_log_size == 0) {
        status = ECONTAINER_RUNTIME_NO_LOG;
    }
    else if (output == NULL || output_capacity < runtime->pending_log_size) {
        status = ECONTAINER_RUNTIME_INVALID_INPUT;
    }
    else {
        memcpy(output, runtime->pending_log, runtime->pending_log_size);
        *log_size_bytes = runtime->pending_log_size;
        runtime->pending_log_size = 0;
    }
    atomic_store(&runtime->call_active, false);
    return status;
}

econtainer_runtime_result_t econtainer_runtime_next_timer_deadline(
    econtainer_runtime_t *runtime, uint64_t *deadline_ms)
{
    if (runtime == NULL || deadline_ms == NULL) return ECONTAINER_RUNTIME_INVALID_INPUT;
    if (!claim_call(runtime)) return ECONTAINER_RUNTIME_BUSY;
    econtainer_runtime_result_t status = ECONTAINER_RUNTIME_NO_TIMER;
    if (runtime->state != ECONTAINER_RUNTIME_RUNNING) {
        status = ECONTAINER_RUNTIME_INVALID_STATE;
    } else {
        uint64_t earliest = UINT64_MAX;
        for (uint32_t index = 0; index < runtime->limits.max_timers; ++index) {
            const econtainer_timer_t *timer = &runtime->timers[index];
            if (timer->active && timer->deadline_ms < earliest)
                earliest = timer->deadline_ms;
        }
        if (earliest != UINT64_MAX) {
            *deadline_ms = earliest;
            status = ECONTAINER_RUNTIME_OK;
        }
    }
    atomic_store(&runtime->call_active, false);
    return status;
}

econtainer_runtime_result_t econtainer_runtime_poll_timer(econtainer_runtime_t *runtime,
                                                         econtainer_timer_event_t *event,
                                                         int32_t *guest_result)
{
    if (runtime == NULL || event == NULL || guest_result == NULL)
        return ECONTAINER_RUNTIME_INVALID_INPUT;
    if (!claim_call(runtime)) return ECONTAINER_RUNTIME_BUSY;
    if (runtime->state != ECONTAINER_RUNTIME_RUNNING) {
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_INVALID_STATE;
    }
    uint64_t now_ms = 0;
    if (!monotonic_ms(&now_ms)) {
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_ENGINE_FAILURE;
    }
    uint32_t selected = runtime->limits.max_timers;
    uint64_t earliest = UINT64_MAX;
    for (uint32_t index = 0; index < runtime->limits.max_timers; ++index) {
        const econtainer_timer_t *timer = &runtime->timers[index];
        if (timer->active && timer->deadline_ms <= now_ms &&
            timer->deadline_ms < earliest) {
            selected = index;
            earliest = timer->deadline_ms;
        }
    }
    if (selected == runtime->limits.max_timers) {
        atomic_store(&runtime->call_active, false);
        return ECONTAINER_RUNTIME_NO_TIMER;
    }
    econtainer_timer_t *timer = &runtime->timers[selected];
    const econtainer_timer_t previous = *timer;
    const uint64_t handle = timer->handle;
    uint32_t skipped = 0;
    if (timer->period_ms == 0) {
        timer->active = false;
    } else {
        const uint64_t periods_missed = (now_ms - timer->deadline_ms) / timer->period_ms;
        skipped = periods_missed > UINT32_MAX ? UINT32_MAX : (uint32_t)periods_missed;
        const uint32_t until_next = timer->period_ms -
                                    (uint32_t)((now_ms - timer->deadline_ms) % timer->period_ms);
        if (now_ms > UINT64_MAX - until_next) {
            timer->active = false;
        } else {
            timer->deadline_ms = now_ms + until_next;
        }
    }
    uint8_t bytes[16] = {'E', 'C', 'T', 1};
    for (uint32_t index = 0; index < 8; ++index)
        bytes[4 + index] = (uint8_t)(handle >> (index * 8));
    for (uint32_t index = 0; index < 4; ++index)
        bytes[12 + index] = (uint8_t)(skipped >> (index * 8));
    const econtainer_runtime_result_t status = call_event(runtime, bytes, sizeof(bytes),
                                                          guest_result);
    if (status == ECONTAINER_RUNTIME_NO_MEMORY) *timer = previous;
    if (status == ECONTAINER_RUNTIME_OK) {
        event->handle = handle;
        event->skipped_periods = skipped;
    }
    atomic_store(&runtime->call_active, false);
    return status;
}

econtainer_runtime_result_t econtainer_runtime_close(econtainer_runtime_t **runtime)
{
    if (runtime == NULL || *runtime == NULL) {
        return ECONTAINER_RUNTIME_OK;
    }
    if (!claim_call(*runtime)) {
        return ECONTAINER_RUNTIME_BUSY;
    }
    release_runtime(*runtime);
    *runtime = NULL;
    return ECONTAINER_RUNTIME_OK;
}
