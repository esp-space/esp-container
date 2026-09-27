cmake_minimum_required(VERSION 3.16)

# These settings must be visible before the upstream WAMR component is configured.
set(WAMR_BUILD_INSTRUCTION_METERING 1 CACHE BOOL "Bound guest instructions" FORCE)
set(WAMR_BUILD_CLASSIC_WALL_CLOCK_LIMIT 1 CACHE BOOL "Bound guest entry time" FORCE)
set(WAMR_BUILD_BULK_MEMORY 0 CACHE BOOL "Avoid implicit constructor invocation" FORCE)
set(WAMR_BUILD_SHARED_MEMORY 0 CACHE BOOL "Disallow guest shared memory" FORCE)
set(WAMR_BUILD_SHRUNK_MEMORY 0 CACHE BOOL "Preserve standard Wasm page sizes" FORCE)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/esp_container")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_container_runtime_probe)
