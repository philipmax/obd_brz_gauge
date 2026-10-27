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
# signal_registry.c (generated, const data) backs vp_validate_default_cfg();
# the firmware include root is main/, hence the second include directory.
add_library(vehicle_core STATIC
    "${VEHICLE_SRC_DIR}/vp_common.c"
    "${VEHICLE_SRC_DIR}/vp_compile.c"
    "${VEHICLE_SRC_DIR}/vp_defaults.c"
    "${VEHICLE_SRC_DIR}/vp_builtin_generic.c"
    "${VEHICLE_SRC_DIR}/vp_rpn.c"
    "${VEHICLE_SRC_DIR}/vp_rpn_json.c"
    "${VEHICLE_SRC_DIR}/vp_validate.c"
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
target_include_directories(vehicle_core PUBLIC "${VEHICLE_SRC_DIR}" "${REPO_ROOT}/main")
target_compile_options(vehicle_core PRIVATE ${VH_WARN} -Wconversion -Wno-sign-conversion)
target_link_libraries(vehicle_core PUBLIC veh_cjson)
find_library(MATH_LIB m)
if(MATH_LIB)
    target_link_libraries(vehicle_core PUBLIC ${MATH_LIB})
endif()

# ---- Generated built-in profile (main/vehicle/vp_builtin_generic.c) ----
add_library(vehicle_builtin INTERFACE)
target_link_libraries(vehicle_builtin INTERFACE vehicle_core)

# ---- Test support library (vt_support.c, shared by all tests) ----
add_library(vt_support STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vt_support.c")
target_include_directories(vt_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(vt_support PRIVATE ${VH_WARN})
target_link_libraries(vt_support PUBLIC vehicle_core veh_cjson)

set(VH_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(VH_CATALOG_DIR "${REPO_ROOT}/vehicles/catalog")
set(VH_IMPORT_DIR "${REPO_ROOT}/vehicles/import/OBD/IMPORT")

# ---- Test executables ----
foreach(t test_rpn test_validate test_compile test_defaults test_golden)
    add_executable(${t} "${CMAKE_CURRENT_SOURCE_DIR}/${t}.c")
    target_compile_options(${t} PRIVATE ${VH_WARN})
    target_compile_definitions(${t} PRIVATE
        "VT_FIXTURE_DIR=\"${VH_FIXTURE_DIR}\""
        "VT_CATALOG_DIR=\"${VH_CATALOG_DIR}\"")
    target_link_libraries(${t} PRIVATE vt_support)
endforeach()
target_compile_definitions(test_compile PRIVATE VT_HAVE_BUILTIN)
target_link_libraries(test_compile PRIVATE vehicle_builtin)

# validate_catalog: validator + compiler + import limits over profile files
add_executable(validate_catalog "${CMAKE_CURRENT_SOURCE_DIR}/validate_catalog.c")
target_compile_options(validate_catalog PRIVATE ${VH_WARN})
target_link_libraries(validate_catalog PRIVATE vt_support)

# vp_blobgen: compiles one profile to a blob (tools/vehicles/gen_builtin.py)
add_executable(vp_blobgen "${CMAKE_CURRENT_SOURCE_DIR}/vp_blobgen.c")
target_compile_options(vp_blobgen PRIVATE ${VH_WARN})
target_link_libraries(vp_blobgen PRIVATE vt_support)

# ---- CTest registrations ----
enable_testing()

add_test(NAME vehicle_rpn      COMMAND test_rpn)
add_test(NAME vehicle_validate COMMAND test_validate)
add_test(NAME vehicle_defaults COMMAND test_defaults)
add_test(NAME vehicle_examples COMMAND validate_catalog --strict "${REPO_ROOT}/vehicles/examples")
add_test(NAME vehicle_compile  COMMAND test_compile "${VH_CATALOG_DIR}/obd2.generic.vehicle.json")
add_test(NAME vehicle_fixtures COMMAND validate_catalog --strict
         "${VH_FIXTURE_DIR}/good_full.json" "${VH_FIXTURE_DIR}/good_min.json")
# every catalog profile: valid with no warnings, compiles, fits JSON/NVS limits
add_test(NAME vehicle_catalog  COMMAND validate_catalog --strict "${VH_CATALOG_DIR}")
add_test(NAME vehicle_import_pack COMMAND validate_catalog --strict "${VH_IMPORT_DIR}")
# compiled init/request sets match the legacy firmware (fixtures/golden/)
add_test(NAME vehicle_golden   COMMAND test_golden "${VH_CATALOG_DIR}" "${VH_FIXTURE_DIR}/golden")

# tool checks (skipped without Python 3): catalog expr matches rpn; the
# generated built-in profile is current
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    file(GLOB VH_CATALOG_FILES "${VH_CATALOG_DIR}/*.vehicle.json")
    file(GLOB VH_IMPORT_FILES "${VH_IMPORT_DIR}/*.vehicle.json")
    file(GLOB VH_EXAMPLE_FILES "${REPO_ROOT}/vehicles/examples/*.vehicle.json")
    add_test(NAME vehicle_import_fresh COMMAND Python3::Interpreter
             "${REPO_ROOT}/tools/vehicles/gen_import_pack.py" --check)
    add_test(NAME vehicle_boost_profiles COMMAND Python3::Interpreter
             "${REPO_ROOT}/tools/vehicles/test_boost_profiles.py")
    add_test(NAME vehicle_expr COMMAND Python3::Interpreter
             "${REPO_ROOT}/tools/vehicles/expr2rpn.py" --check ${VH_CATALOG_FILES} ${VH_IMPORT_FILES} ${VH_EXAMPLE_FILES})
    add_test(NAME vehicle_builtin_fresh COMMAND Python3::Interpreter
             "${REPO_ROOT}/tools/vehicles/gen_builtin.py" --check --blobgen $<TARGET_FILE:vp_blobgen>)
endif()

# Explicit opt-in timing target: separate release tree with VH_SANITIZE=OFF.
option(VH_BENCHMARK "Build raw-field extraction microbenchmark" OFF)
if(VH_BENCHMARK)
    add_executable(bench_raw "${CMAKE_CURRENT_SOURCE_DIR}/bench_raw.c")
    target_compile_options(bench_raw PRIVATE ${VH_WARN})
    target_link_libraries(bench_raw PRIVATE vehicle_core)
endif()

# Private conversion tests use the actual RPN source; overrides are experiments
# only. Empty in ordinary CI. No probe or baseline is linked into firmware.
set(VH_INTEGER_SOURCE "" CACHE FILEPATH "Temporary RPN source for private conversion tests")
add_library(rpn_integer_probe STATIC rpn_integer_probe.c)
target_link_libraries(rpn_integer_probe PUBLIC vehicle_core)
target_include_directories(rpn_integer_probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(rpn_integer_probe PRIVATE ${VH_WARN})
if(VH_INTEGER_SOURCE)
    target_compile_definitions(rpn_integer_probe PRIVATE "RI_SOURCE=\"${VH_INTEGER_SOURCE}\"")
endif()
add_executable(test_rpn_integer test_rpn_integer.c)
target_link_libraries(test_rpn_integer PRIVATE rpn_integer_probe)
target_compile_options(test_rpn_integer PRIVATE ${VH_WARN})
add_test(NAME vehicle_rpn_integer COMMAND test_rpn_integer)

option(VH_INTEGER_BENCHMARK "Build paired RPN integer benchmark" OFF)
set(VH_INTEGER_BASELINE_SOURCE "" CACHE FILEPATH "Whole old RPN source for the optional benchmark")
if(VH_INTEGER_BENCHMARK)
    if(NOT EXISTS "${VH_INTEGER_BASELINE_SOURCE}")
        message(FATAL_ERROR "VH_INTEGER_BENCHMARK requires VH_INTEGER_BASELINE_SOURCE")
    endif()
    add_library(rpn_integer_baseline STATIC rpn_integer_probe.c)
    target_link_libraries(rpn_integer_baseline PUBLIC vehicle_core)
    target_compile_options(rpn_integer_baseline PRIVATE ${VH_WARN})
    target_compile_definitions(rpn_integer_baseline PRIVATE
        "RI_SOURCE=\"${VH_INTEGER_BASELINE_SOURCE}\""
        ri_op_arity=ri_baseline_op_arity ri_op_name=ri_baseline_op_name
        ri_err_str=ri_baseline_err_str ri_verify=ri_baseline_verify
        ri_extract_raw=ri_baseline_extract_raw ri_eval=ri_baseline_eval
        ri_to_u32=ri_baseline_to_u32 ri_to_shift=ri_baseline_to_shift)
    target_compile_definitions(test_rpn_integer PRIVATE RI_HAVE_BASELINE)
    target_link_libraries(test_rpn_integer PRIVATE rpn_integer_baseline)
    add_executable(bench_rpn_integer bench_rpn_integer.c)
    target_compile_options(bench_rpn_integer PRIVATE ${VH_WARN})
    target_link_libraries(bench_rpn_integer PRIVATE rpn_integer_probe rpn_integer_baseline)
endif()

# Raw-field conversion tests reuse the existing actual-source evaluator probe.
add_executable(test_rpn_raw_float test_rpn_raw_float.c)
target_compile_options(test_rpn_raw_float PRIVATE ${VH_WARN})
target_link_libraries(test_rpn_raw_float PRIVATE rpn_integer_probe)
add_test(NAME vehicle_rpn_raw_float COMMAND test_rpn_raw_float)
if(VH_INTEGER_BENCHMARK)
    target_compile_definitions(test_rpn_raw_float PRIVATE RI_HAVE_BASELINE)
    target_link_libraries(test_rpn_raw_float PRIVATE rpn_integer_baseline)
    add_executable(bench_rpn_raw_float bench_rpn_integer.c)
    target_compile_definitions(bench_rpn_raw_float PRIVATE RI_RAW_FLOAT_BENCHMARK)
    target_compile_options(bench_rpn_raw_float PRIVATE ${VH_WARN})
    target_link_libraries(bench_rpn_raw_float PRIVATE rpn_integer_probe rpn_integer_baseline)
endif()
