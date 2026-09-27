idf_component_register(SRCS "../../runtime-probe/main.c" INCLUDE_DIRS "."
    REQUIRES esp_container wasm-micro-runtime esp_timer heap)
if(NOT WAMR_BUILD_INSTRUCTION_METERING OR NOT WAMR_BUILD_CLASSIC_WALL_CLOCK_LIMIT)
    message(FATAL_ERROR "Runtime probe requires WAMR Classic instruction and deadline limits")
endif()
# WAMR keeps its compile definition private; this sample checks the same
# pre-project build setting in its own source profile assertion.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WASM_ENABLE_INSTRUCTION_METERING=1 WASM_ENABLE_CLASSIC_WALL_CLOCK_LIMIT=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
