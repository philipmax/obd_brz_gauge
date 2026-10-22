# Host tests for the ESP-NOW wire codec (main/bsp_obd_dsp/espnow_v6.c), no ESP-IDF build.
#
#   cmake -S test/host/espnow -B build-host/espnow && cmake --build build-host/espnow
#   ctest --test-dir build-host/espnow --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(espnow_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# ---- Optional sanitisers ----
option(EH_SANITIZE "Build with -fsanitize=address,undefined when supported" ON)
if(EH_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" EH_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(EH_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

set(EH_WARN -Wall -Wextra -Werror -Wshadow)

# ---- Codec under test (pure C) + the generated static signal registry ----
add_library(espnow_codec STATIC
    "${REPO_ROOT}/main/bsp_obd_dsp/espnow_v6.c"
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
target_include_directories(espnow_codec PUBLIC
    "${REPO_ROOT}/main/bsp_obd_dsp"
    "${REPO_ROOT}/main/app_obd_dsp")
target_compile_options(espnow_codec PRIVATE ${EH_WARN} -Wconversion -Wno-sign-conversion)

add_executable(test_espnow_proto "${CMAKE_CURRENT_SOURCE_DIR}/test_espnow_proto.c")
target_compile_options(test_espnow_proto PRIVATE ${EH_WARN})
target_link_libraries(test_espnow_proto PRIVATE espnow_codec)

enable_testing()
add_test(NAME espnow_proto COMMAND test_espnow_proto)

# RaceChrono shares this wireless test job; its scheduling core is pure C.
add_executable(test_racechrono_filter
    "${CMAKE_CURRENT_SOURCE_DIR}/test_racechrono_filter.c"
    "${REPO_ROOT}/main/bsp_obd_dsp/racechrono_filter.c")
target_include_directories(test_racechrono_filter PRIVATE "${REPO_ROOT}/main/bsp_obd_dsp")
target_compile_options(test_racechrono_filter PRIVATE ${EH_WARN} -Wconversion -Wno-sign-conversion)
add_test(NAME racechrono_filter COMMAND test_racechrono_filter)

# Compile the real worker/GATT wrapper and BLE lifecycle against deterministic
# host primitives. No radio or ESP32 scheduler timing is simulated.
add_executable(test_racechrono_stream
    "${CMAKE_CURRENT_SOURCE_DIR}/test_racechrono_stream.c"
    "${REPO_ROOT}/main/bsp_obd_dsp/racechrono_ble_diy.c"
    "${REPO_ROOT}/main/bsp_obd_dsp/racechrono_filter.c"
    "${REPO_ROOT}/main/bsp_obd_dsp/ble_hub.c")
target_include_directories(test_racechrono_stream PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${REPO_ROOT}/main")
target_compile_options(test_racechrono_stream PRIVATE ${EH_WARN})
add_test(NAME racechrono_stream COMMAND test_racechrono_stream)
set_tests_properties(racechrono_stream PROPERTIES TIMEOUT 30)

# Real master worker + control callback + OTA stop, virtual time (no RF/RTOS).
add_executable(test_espnow_link
    "${CMAKE_CURRENT_SOURCE_DIR}/test_espnow_link.c"
    "${REPO_ROOT}/main/bsp_obd_dsp/espnow_link.c")
target_include_directories(test_espnow_link PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/link_stubs"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${REPO_ROOT}/main")
target_compile_options(test_espnow_link PRIVATE ${EH_WARN})
target_link_libraries(test_espnow_link PRIVATE espnow_codec)
add_test(NAME espnow_link COMMAND test_espnow_link)
set_tests_properties(espnow_link PROPERTIES TIMEOUT 30)

# Unique append and the real private collector on controlled signal-store inputs.
add_executable(test_snapshot_append test_snapshot_append.c snapshot_reference.c)
target_compile_options(test_snapshot_append PRIVATE ${EH_WARN})
target_link_libraries(test_snapshot_append PRIVATE espnow_codec)
add_test(NAME snapshot_append COMMAND test_snapshot_append)

add_library(collect_probe STATIC collect_probe.c)
target_include_directories(collect_probe PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/link_stubs" "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${REPO_ROOT}/main")
target_compile_options(collect_probe PRIVATE ${EH_WARN} -ffunction-sections -fdata-sections)
if(APPLE)
    target_link_options(collect_probe INTERFACE -Wl,-dead_strip)
else()
    target_link_options(collect_probe INTERFACE -Wl,--gc-sections)
endif()
target_link_libraries(collect_probe PUBLIC espnow_codec)
add_executable(test_master_collect test_master_collect.c)
target_compile_options(test_master_collect PRIVATE ${EH_WARN})
target_link_libraries(test_master_collect PRIVATE collect_probe)
add_test(NAME master_collect COMMAND test_master_collect)

option(EH_COLLECT_BENCHMARK "Build snapshot insertion and master collector benchmark" OFF)
if(EH_COLLECT_BENCHMARK)
    add_executable(bench_master_collect bench_collect.c snapshot_reference.c)
    target_compile_options(bench_master_collect PRIVATE ${EH_WARN})
    target_link_libraries(bench_master_collect PRIVATE collect_probe)
endif()

# Legacy encoder byte equivalence against a frozen pre-optimization reference.
add_executable(test_v5_encode test_v5_encode.c v5_encode_reference.c)
target_compile_options(test_v5_encode PRIVATE ${EH_WARN})
target_link_libraries(test_v5_encode PRIVATE espnow_codec)
add_test(NAME v5_encode COMMAND test_v5_encode)
option(EH_V5_BENCHMARK "Build v5 encoder and actual collector timing" OFF)
if(EH_V5_BENCHMARK)
    add_executable(bench_v5_encode bench_v5_encode.c v5_encode_reference.c)
    target_compile_options(bench_v5_encode PRIVATE ${EH_WARN})
    target_link_libraries(bench_v5_encode PRIVATE collect_probe)
endif()

# Test the CRC wrapper using the SDK's official Linux compatibility backend.
# Ordinary portable tests continue to work without an SDK installation.
if(DEFINED ENV{IDF_PATH})
    set(_eh_idf_default "$ENV{IDF_PATH}")
else()
    set(_eh_idf_default "$ENV{HOME}/.espressif/v5.5.3/esp-idf")
endif()
set(EH_ESP_ROM_DIR "${_eh_idf_default}/components/esp_rom" CACHE PATH
    "ESP-IDF esp_rom directory for optional CRC compatibility tests")
option(EH_ROM_CRC "Test ESP_PLATFORM CRC with the official SDK Linux backend" ON)
option(EH_CRC_BENCHMARK "Build CRC and full-codec timing against SDK Linux backend" OFF)

add_executable(test_espnow_crc test_crc.c crc_reference.c codec_portable.c)
target_compile_options(test_espnow_crc PRIVATE ${EH_WARN})
target_link_libraries(test_espnow_crc PRIVATE espnow_codec)
add_test(NAME espnow_crc_portable COMMAND test_espnow_crc)

if(EH_ROM_CRC AND EXISTS "${EH_ESP_ROM_DIR}/linux/esp_rom_crc.c" AND
   EXISTS "${EH_ESP_ROM_DIR}/include/esp_rom_crc.h")
    add_library(espnow_rom_compat STATIC "${EH_ESP_ROM_DIR}/linux/esp_rom_crc.c")
    target_include_directories(espnow_rom_compat PUBLIC "${EH_ESP_ROM_DIR}/include")
    add_library(espnow_rom_codec STATIC
        "${REPO_ROOT}/main/bsp_obd_dsp/espnow_v6.c"
        "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
    target_include_directories(espnow_rom_codec PUBLIC
        "${REPO_ROOT}/main/bsp_obd_dsp" "${REPO_ROOT}/main/app_obd_dsp")
    target_compile_definitions(espnow_rom_codec PRIVATE ESP_PLATFORM)
    target_compile_options(espnow_rom_codec PRIVATE ${EH_WARN} -Wconversion -Wno-sign-conversion)
    target_link_libraries(espnow_rom_codec PUBLIC espnow_rom_compat)

    add_executable(test_espnow_proto_rom test_espnow_proto.c)
    target_compile_options(test_espnow_proto_rom PRIVATE ${EH_WARN})
    target_link_libraries(test_espnow_proto_rom PRIVATE espnow_rom_codec)
    add_test(NAME espnow_proto_rom COMMAND test_espnow_proto_rom)

    add_executable(test_espnow_crc_rom test_crc.c crc_reference.c codec_portable.c)
    target_compile_definitions(test_espnow_crc_rom PRIVATE EH_HAVE_ROM)
    target_compile_options(test_espnow_crc_rom PRIVATE ${EH_WARN})
    target_link_libraries(test_espnow_crc_rom PRIVATE espnow_rom_codec)
    add_test(NAME espnow_crc_rom COMMAND test_espnow_crc_rom)

    if(EH_CRC_BENCHMARK)
        add_executable(bench_espnow_crc bench_crc.c codec_portable.c)
        target_compile_options(bench_espnow_crc PRIVATE ${EH_WARN})
        target_link_libraries(bench_espnow_crc PRIVATE espnow_rom_codec)
    endif()
elseif(EH_CRC_BENCHMARK)
    message(FATAL_ERROR "EH_CRC_BENCHMARK requires EH_ROM_CRC and an ESP-IDF esp_rom tree")
endif()
