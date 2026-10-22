# ESP-IDF 5.5.3 can lose a pending acquire at SPI ISR exit (cross-core LCD
# streaming). Backport the upstream fix into the build tree, without changing
# the installed SDK. Exact context checks stop configuration on an unknown SDK.
idf_component_get_property(_obd_hw_lib esp_hw_support COMPONENT_LIB)
idf_component_get_property(_obd_hw_dir esp_hw_support COMPONENT_DIR)
idf_build_get_property(_obd_python PYTHON)
set(_obd_lock_original "${_obd_hw_dir}/spi_bus_lock.c")
set(_obd_lock_patched "${CMAKE_BINARY_DIR}/idf_compat/spi_bus_lock.c")
set(_obd_lock_patcher "${CMAKE_CURRENT_LIST_DIR}/../tools/idf_compat/patch_spi_bus_lock.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_obd_lock_original}" "${_obd_lock_patcher}"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/idf_compat/spi_bus_lock.patch")
execute_process(COMMAND "${_obd_python}" "${_obd_lock_patcher}"
    "${_obd_lock_original}" "${_obd_lock_patched}"
    RESULT_VARIABLE _obd_lock_result OUTPUT_VARIABLE _obd_lock_output
    ERROR_VARIABLE _obd_lock_error OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _obd_lock_result EQUAL 0)
    message(FATAL_ERROR "${_obd_lock_output}\n${_obd_lock_error}")
endif()
get_target_property(_obd_hw_sources ${_obd_hw_lib} SOURCES)
set(_obd_lock_found FALSE)
foreach(_obd_src IN LISTS _obd_hw_sources)
    if(_obd_src STREQUAL "${_obd_lock_original}")
        set(_obd_lock_found TRUE)
    endif()
endforeach()
if(NOT _obd_lock_found)
    message(FATAL_ERROR "Cannot locate esp_hw_support spi_bus_lock.c in the SDK target")
endif()
list(REMOVE_ITEM _obd_hw_sources "${_obd_lock_original}")
list(APPEND _obd_hw_sources "${_obd_lock_patched}")
set_property(TARGET ${_obd_hw_lib} PROPERTY SOURCES "${_obd_hw_sources}")
message(STATUS "${_obd_lock_output}")
