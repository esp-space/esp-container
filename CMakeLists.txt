cmake_minimum_required(VERSION 3.16)

project(esp_container VERSION 0.1.0 LANGUAGES C)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(esp_container STATIC
    components/esp_container/src/wasm_scan.c
    components/esp_container/src/package_wasm.c
    components/esp_container/src/package_stream.c
    components/esp_container/src/package_slot.c
    components/esp_container/src/package_crypto.c
    components/esp_container/src/slots.c)
target_include_directories(esp_container PUBLIC components/esp_container/include)
target_include_directories(esp_container PRIVATE components/esp_container/src)
target_link_libraries(esp_container PUBLIC OpenSSL::Crypto)
target_compile_features(esp_container PUBLIC c_std_11)
target_compile_options(esp_container PRIVATE -Wall -Wextra -Werror -Wconversion)
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(package_stream_test tests/package_stream_test.c)
    target_link_libraries(package_stream_test PRIVATE esp_container)
    target_compile_options(package_stream_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME package_stream
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_stream_test.py"
                "$<TARGET_FILE:package_stream_test>")
    add_executable(package_wasm_test tests/package_wasm_test.c)
    target_link_libraries(package_wasm_test PRIVATE esp_container)
    target_compile_options(package_wasm_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME package_wasm
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_wasm_test.py"
                "$<TARGET_FILE:package_wasm_test>")
    add_executable(wasm_scan_test tests/wasm_scan_test.c)
    target_link_libraries(wasm_scan_test PRIVATE esp_container)
    target_compile_options(wasm_scan_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME wasm_scan COMMAND wasm_scan_test)
    add_executable(slots_test tests/slots_test.c)
    target_link_libraries(slots_test PRIVATE esp_container)
    target_compile_options(slots_test PRIVATE -Wall -Wextra -Werror -Wconversion -UNDEBUG)
    add_test(NAME slots COMMAND slots_test)
    add_executable(package_slot_test tests/package_slot_test.c)
    target_link_libraries(package_slot_test PRIVATE esp_container)
    target_compile_options(package_slot_test PRIVATE -Wall -Wextra -Werror -Wconversion -UNDEBUG)
    add_test(NAME package_slot
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_slot_test.py"
                "$<TARGET_FILE:package_slot_test>")
    add_executable(slots_idf_test
        components/esp_container/src/slots_idf.c tests/slots_idf_test.c)
    target_include_directories(slots_idf_test PRIVATE
        tests/fakes/slots_idf components/esp_container/include)
    target_link_libraries(slots_idf_test PRIVATE esp_container)
    target_compile_options(slots_idf_test PRIVATE -Wall -Wextra -Werror -Wconversion -UNDEBUG)
    add_test(NAME slots_idf COMMAND slots_idf_test)

    set(ESP_CONTAINER_WAMR_SOURCE "" CACHE PATH "Path to the WAMR source fetched by the locked C3 example")
    if(ESP_CONTAINER_WAMR_SOURCE)
        if(NOT EXISTS "${ESP_CONTAINER_WAMR_SOURCE}/core/iwasm/include/wasm_export.h")
            message(FATAL_ERROR "ESP_CONTAINER_WAMR_SOURCE must point to a WAMR source checkout")
        endif()
        set(WAMR_BUILD_INTERP 1)
        set(WAMR_BUILD_FAST_INTERP 0)
        set(WAMR_BUILD_AOT 0)
        set(WAMR_BUILD_JIT 0)
        set(WAMR_BUILD_FAST_JIT 0)
        set(WAMR_BUILD_LIBC_WASI 0)
        set(WAMR_BUILD_LIBC_BUILTIN 0)
        set(WAMR_BUILD_LIB_PTHREAD 0)
        set(WAMR_BUILD_MULTI_MODULE 0)
        set(WAMR_BUILD_SIMD 0)
        set(WAMR_BUILD_REF_TYPES 0)
        set(WAMR_BUILD_BULK_MEMORY 0)
        set(WAMR_BUILD_SHARED_MEMORY 0)
        set(WAMR_BUILD_SHRUNK_MEMORY 0)
        set(WAMR_BUILD_INSTRUCTION_METERING 1)
        set(WAMR_BUILD_CLASSIC_WALL_CLOCK_LIMIT 1)
        add_subdirectory("${ESP_CONTAINER_WAMR_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/wamr")
        target_sources(esp_container PRIVATE components/esp_container/src/runtime.c
            components/esp_container/src/slot_runtime.c components/esp_container/src/product.c)
        target_include_directories(esp_container PRIVATE components/esp_container/src)
        target_compile_definitions(esp_container PRIVATE WASM_ENABLE_CLASSIC_WALL_CLOCK_LIMIT=1)
        target_link_libraries(esp_container PRIVATE vmlib)
        add_executable(wamr_classic_test tests/wamr_classic_test.c)
        target_link_libraries(wamr_classic_test PRIVATE esp_container vmlib)
        target_compile_options(wamr_classic_test PRIVATE -Wall -Wextra -Werror)
        add_test(NAME wamr_classic COMMAND wamr_classic_test)
        set_tests_properties(wamr_classic PROPERTIES TIMEOUT 10)

        set(ESP_CONTAINER_WASI_SDK_ROOT "$ENV{WASI_SDK_ROOT}" CACHE PATH
            "Official wasi-sdk 33 root for real counter runtime tests")
        if(ESP_CONTAINER_WASI_SDK_ROOT)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            set(runtime_guests_dir "${CMAKE_CURRENT_BINARY_DIR}/runtime-guests")
            set(runtime_guests
                "${runtime_guests_dir}/counter.wasm"
                "${runtime_guests_dir}/event-read.wasm"
                "${runtime_guests_dir}/init-loop.wasm"
                "${runtime_guests_dir}/event-loop.wasm"
                "${runtime_guests_dir}/stop-loop.wasm"
                "${runtime_guests_dir}/stop-fail.wasm"
                "${runtime_guests_dir}/wrong-signature.wasm")
            list(APPEND runtime_guests "${runtime_guests_dir}/host-api.wasm")
            list(APPEND runtime_guests "${runtime_guests_dir}/timer.wasm")
            list(APPEND runtime_guests "${runtime_guests_dir}/deadline.wasm")
            list(APPEND runtime_guests "${runtime_guests_dir}/two-page.wasm")
            list(APPEND runtime_guests "${runtime_guests_dir}/memory.wasm")
            add_custom_command(OUTPUT ${runtime_guests}
                    "${runtime_guests_dir}/counter-v2.wasm"
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_runtime_guests.py"
                    --wasi-sdk "${ESP_CONTAINER_WASI_SDK_ROOT}"
                    --output-dir "${runtime_guests_dir}"
                DEPENDS tests/build_runtime_guests.py tests/runtime_guest.c tests/host_api_guest.c
                    tests/timer_guest.c tests/deadline_guest.c tests/memory_guest.c
                    tools/counter_guest.py examples/counter/counter.c
                    examples/counter-v2/counter.c
                    guest-sdk/include/econtainer_guest.h guest-sdk/src/econtainer_guest.c
                VERBATIM)
            add_custom_target(runtime_guest_fixtures DEPENDS ${runtime_guests}
                "${runtime_guests_dir}/counter-v2.wasm")
            add_executable(runtime_instance_test tests/runtime_instance_test.c)
            target_include_directories(runtime_instance_test PRIVATE components/esp_container/src)
            target_link_libraries(runtime_instance_test PRIVATE esp_container)
            target_compile_options(runtime_instance_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
            if(APPLE AND NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
                target_compile_definitions(runtime_instance_test PRIVATE
                    ECONTAINER_TEST_RESOURCE_STATS=1)
            endif()
            add_dependencies(runtime_instance_test runtime_guest_fixtures)
            add_test(NAME runtime_instance COMMAND runtime_instance_test ${runtime_guests})
            set_tests_properties(runtime_instance PROPERTIES TIMEOUT 15)
            find_package(Threads REQUIRED)
            add_executable(slot_runtime_test tests/slot_runtime_test.c)
            target_link_libraries(slot_runtime_test PRIVATE esp_container Threads::Threads)
            target_compile_options(slot_runtime_test PRIVATE -Wall -Wextra -Werror -Wconversion -UNDEBUG)
            add_dependencies(slot_runtime_test runtime_guest_fixtures)
            add_test(NAME slot_runtime
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/slot_runtime_test.py"
                    "$<TARGET_FILE:slot_runtime_test>" "${runtime_guests_dir}")
            set_tests_properties(slot_runtime PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()
