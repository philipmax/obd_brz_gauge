# Host tests for the vehicle profile core (main/vehicle/), no ESP-IDF build.
#
#   cmake -S test/host/vehicle -B build-host/vehicle && cmake --build build-host/vehicle
#   ctest --test-dir build-host/vehicle --output-on-failure
#
# cJSON is taken from the ESP-IDF tree when available (set IDF_PATH or -DCJSON_DIR=...),
# otherwise fetched automatically via FetchContent (requires network on first configure).
cmake_minimum_required(VERSION 3.16)
project(vehicle_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(VEHICLE_SRC_DIR "${REPO_ROOT}/main/vehicle")

# ---- cJSON: prefer IDF tree, fall back to FetchContent ----
if(DEFINED ENV{IDF_PATH})
    set(_idf_default "$ENV{IDF_PATH}")
else()
    set(_idf_default "$ENV{HOME}/.espressif/v5.5.3/esp-idf")
endif()
set(CJSON_DIR "${_idf_default}/components/json/cJSON" CACHE PATH "Directory with cJSON.c/cJSON.h")

if(EXISTS "${CJSON_DIR}/cJSON.c")
    add_library(veh_cjson STATIC "${CJSON_DIR}/cJSON.c")
    target_include_directories(veh_cjson PUBLIC "${CJSON_DIR}")
    target_compile_options(veh_cjson PRIVATE -w)
else()
    message(STATUS "cJSON not found at ${CJSON_DIR}; fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(cjson_fc
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18
        GIT_SHALLOW    TRUE)
    set(ENABLE_CJSON_TEST  OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson_fc)
    # cJSON's CMakeLists exports target 'cjson'; wrap it to match the local name.
    add_library(veh_cjson INTERFACE)
    target_link_libraries(veh_cjson INTERFACE cjson)
endif()

# ---- Optional sanitisers ----
option(VH_SANITIZE "Build with -fsanitize=address,undefined when supported" ON)
if(VH_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" VH_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(VH_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

set(VH_WARN -Wall -Wextra -Werror -Wshadow)

# ---- Vehicle core library (pure C99, no ESP-IDF) ----
add_library(vehicle_core STATIC
    "${VEHICLE_SRC_DIR}/vp_common.c"
    "${VEHICLE_SRC_DIR}/vp_compile.c"
    "${VEHICLE_SRC_DIR}/vp_rpn.c"
    "${VEHICLE_SRC_DIR}/vp_rpn_json.c"
    "${VEHICLE_SRC_DIR}/vp_validate.c")
target_include_directories(vehicle_core PUBLIC "${VEHICLE_SRC_DIR}")
target_compile_options(vehicle_core PRIVATE ${VH_WARN} -Wconversion -Wno-sign-conversion)
target_link_libraries(vehicle_core PUBLIC veh_cjson)
find_library(MATH_LIB m)
if(MATH_LIB)
    target_link_libraries(vehicle_core PUBLIC ${MATH_LIB})
endif()

# ---- Test support library (vt_support.c, shared by all tests) ----
add_library(vt_support STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vt_support.c")
target_include_directories(vt_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(vt_support PRIVATE ${VH_WARN})
target_link_libraries(vt_support PUBLIC vehicle_core veh_cjson)

# ---- Test fixtures path (resolved at configure time) ----
set(VH_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(VH_SIGNALS_JSON "${REPO_ROOT}/contracts/signals.json" CACHE FILEPATH
    "signals.json registry for vehicle tests (contracts/signals.json)")

# ---- Test executables ----
foreach(t test_rpn test_validate test_compile)
    add_executable(${t} "${CMAKE_CURRENT_SOURCE_DIR}/${t}.c")
    target_compile_options(${t} PRIVATE ${VH_WARN})
    target_compile_definitions(${t} PRIVATE
        "VT_FIXTURE_DIR=\"${VH_FIXTURE_DIR}\"")
    target_link_libraries(${t} PRIVATE vt_support)
endforeach()

# validate_catalog: validates fixture JSON files against the schema
add_executable(validate_catalog "${CMAKE_CURRENT_SOURCE_DIR}/validate_catalog.c")
target_compile_options(validate_catalog PRIVATE ${VH_WARN})
target_link_libraries(validate_catalog PRIVATE vt_support)

# vp_blobgen: standalone tool — built but not registered as a CTest
add_executable(vp_blobgen "${CMAKE_CURRENT_SOURCE_DIR}/vp_blobgen.c")
target_compile_options(vp_blobgen PRIVATE ${VH_WARN})
target_link_libraries(vp_blobgen PRIVATE vehicle_core veh_cjson)

# ---- CTest registrations ----
enable_testing()

add_test(NAME vehicle_rpn COMMAND test_rpn)

# Pass the signals registry when it exists; the test falls back to the built-in
# registry when the --signals flag is absent, so both local and CI paths work.
if(EXISTS "${VH_SIGNALS_JSON}")
    add_test(NAME vehicle_validate
             COMMAND test_validate --signals "${VH_SIGNALS_JSON}")
    add_test(NAME vehicle_compile
             COMMAND test_compile  --signals "${VH_SIGNALS_JSON}")
    add_test(NAME vehicle_catalog
             COMMAND validate_catalog --signals "${VH_SIGNALS_JSON}"
                     "${VH_FIXTURE_DIR}")
else()
    add_test(NAME vehicle_validate COMMAND test_validate)
    add_test(NAME vehicle_compile  COMMAND test_compile)
    add_test(NAME vehicle_catalog
             COMMAND validate_catalog "${VH_FIXTURE_DIR}")
endif()
