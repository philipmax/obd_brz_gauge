# Host tests for the theme rules core (main/theme_engine/rules), no ESP-IDF build.
#
#   cmake -S test/host/rules -B build-host/rules && cmake --build build-host/rules
#   ctest --test-dir build-host/rules --output-on-failure
#
# cJSON is compiled from the ESP-IDF tree (not vendored): set IDF_PATH or
# -DCJSON_DIR=... if it is not at ~/.espressif/v5.5.3/esp-idf.
cmake_minimum_required(VERSION 3.16)
project(theme_rules_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(RULES_SRC_DIR "${REPO_ROOT}/main/theme_engine/rules")

if(DEFINED ENV{IDF_PATH})
    set(_idf_default "$ENV{IDF_PATH}")
else()
    set(_idf_default "$ENV{HOME}/.espressif/v5.5.3/esp-idf")
endif()
set(CJSON_DIR "${_idf_default}/components/json/cJSON" CACHE PATH "Directory with cJSON.c/cJSON.h")
if(EXISTS "${CJSON_DIR}/cJSON.c")
    set(_TR_CJSON_LOCAL ON)
else()
    message(STATUS "cJSON not found at ${CJSON_DIR}; fetching via FetchContent")
    set(_TR_CJSON_LOCAL OFF)
    include(FetchContent)
    FetchContent_Declare(cjson_fc
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18
        GIT_SHALLOW    TRUE)
    set(ENABLE_CJSON_TEST  OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson_fc)
endif()

# Golden vectors: primary copy lives in contracts/ (always present in the repo).
# Pass a nonexistent second path so the binary's fallback logic still compiles cleanly.
set(RULE_VECTORS "${REPO_ROOT}/contracts/fixtures/rules/vectors.json" CACHE FILEPATH "Golden rule vectors")
set(RULE_VECTORS_FALLBACK
    "${REPO_ROOT}/contracts/fixtures/rules/vectors_extra.json"
    CACHE FILEPATH "Fallback golden rule vectors (optional, may not exist)")

set(LABEL_VECTORS "${REPO_ROOT}/contracts/fixtures/format/label_format.json" CACHE FILEPATH "Golden label format vectors")

option(TR_SANITIZE "Build with -fsanitize=address,undefined when supported" ON)
if(TR_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" TR_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(TR_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Third-party: no -Werror.
if(_TR_CJSON_LOCAL)
    add_library(tr_cjson STATIC "${CJSON_DIR}/cJSON.c")
    target_include_directories(tr_cjson PUBLIC "${CJSON_DIR}")
    target_compile_options(tr_cjson PRIVATE -w)
else()
    # FetchContent brought in the cjson target; wrap it under the local name.
    add_library(tr_cjson INTERFACE)
    target_link_libraries(tr_cjson INTERFACE cjson)
endif()

set(TR_WARN -Wall -Wextra -Werror)

add_library(theme_rules STATIC
    "${RULES_SRC_DIR}/tr_parse.c"
    "${RULES_SRC_DIR}/tr_eval.c"
    "${RULES_SRC_DIR}/tr_format.c")
target_include_directories(theme_rules PUBLIC "${RULES_SRC_DIR}")
# JS never fuses multiply-add; keep doubles bit-exact with the editor.
target_compile_options(theme_rules PRIVATE ${TR_WARN} -Wshadow -Wconversion -Wno-sign-conversion -ffp-contract=off)
target_link_libraries(theme_rules PUBLIC tr_cjson)
find_library(MATH_LIB m)
if(MATH_LIB)
    target_link_libraries(theme_rules PUBLIC ${MATH_LIB})
endif()

foreach(t test_rules_vectors test_label_format test_rules_unit test_rule_cache)
    add_executable(${t} ${t}.c test_util.c)
    target_compile_options(${t} PRIVATE ${TR_WARN})
    target_link_libraries(${t} PRIVATE theme_rules)
endforeach()

enable_testing()
add_test(NAME rules_golden_vectors
         COMMAND test_rules_vectors --expect-groups 38 --expect-steps 79
                 "${RULE_VECTORS}" "${RULE_VECTORS_FALLBACK}")
add_test(NAME rules_extra_vectors
         COMMAND test_rules_vectors "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/extra_vectors.json")
add_test(NAME label_format_golden
         COMMAND test_label_format --expect-cases 24 "${LABEL_VECTORS}")
add_test(NAME label_format_extra
         COMMAND test_label_format "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/label_vectors.json")
add_test(NAME rules_unit COMMAND test_rules_unit)

add_test(NAME rules_output_cache COMMAND test_rule_cache)

# Timing is explicitly opt-in; never use host wall time as a CTest assertion.
option(TR_BENCHMARK "Build rule output-cache microbenchmark" OFF)
if(TR_BENCHMARK)
    add_executable(bench_rule_cache bench_rule_cache.c)
    target_compile_options(bench_rule_cache PRIVATE ${TR_WARN})
    target_link_libraries(bench_rule_cache PRIVATE theme_rules)
endif()
