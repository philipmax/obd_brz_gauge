cmake_minimum_required(VERSION 3.16)
project(spi_lock_regression C)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SPI_LOCK_SOURCE "" CACHE FILEPATH "ESP-IDF spi_bus_lock.c (optional offline input)")
if(NOT SPI_LOCK_SOURCE)
    if(DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/components/esp_hw_support/spi_bus_lock.c")
        set(SPI_LOCK_SOURCE "$ENV{IDF_PATH}/components/esp_hw_support/spi_bus_lock.c")
    else()
        # Immutable reference for the negative control; never fetch a moving branch.
        set(SPI_LOCK_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/spi_bus_lock.v5.5.3.c")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_hw_support/spi_bus_lock.c"
            "${SPI_LOCK_SOURCE}"
            EXPECTED_HASH SHA256=e40cccc457a4fa4f2bb170c58031699beccbd384238459533e2cdf44bab5159b
            TLS_VERIFY ON)
    endif()
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare.py"
    "${SPI_LOCK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot generate SPI lock regression inputs")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SPI_LOCK_SOURCE}" prepare.py
    "${REPO_ROOT}/tools/idf_compat/patch_spi_bus_lock.py"
    "${REPO_ROOT}/tools/idf_compat/spi_bus_lock.patch")
foreach(variant baseline patched)
    add_executable(test_${variant} test_handoff.c)
    target_include_directories(test_${variant} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(test_${variant} PRIVATE LOCK_FUNCTIONS="${variant}.h")
    if(variant STREQUAL "baseline")
        target_compile_definitions(test_${variant} PRIVATE BASELINE=1)
    endif()
    target_compile_options(test_${variant} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME spi_lock_${variant} COMMAND test_${variant})
endforeach()
add_test(NAME spi_lock_patch_tool COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/test_patch.py" "${SPI_LOCK_SOURCE}")
