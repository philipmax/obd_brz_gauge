# ESP-IDF 5.5.3 has FatFs exFAT support in the source but hard-codes
# FF_FS_EXFAT=0 without a Kconfig option. Build a patched copy locally so the
# installed SDK remains untouched and FAT32 behavior stays unchanged.
idf_component_get_property(_obd_fatfs_lib fatfs COMPONENT_LIB)
idf_component_get_property(_obd_fatfs_dir fatfs COMPONENT_DIR)
idf_build_get_property(_obd_python PYTHON)
set(_obd_fatfs_src "${_obd_fatfs_dir}/src")
set(_obd_fatfs_patch_dir "${CMAKE_BINARY_DIR}/idf_compat/fatfs")
set(_obd_fatfs_patcher "${CMAKE_CURRENT_LIST_DIR}/../tools/idf_compat/patch_fatfs_exfat.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_obd_fatfs_src}/ff.c" "${_obd_fatfs_src}/ff.h"
    "${_obd_fatfs_src}/ffunicode.c" "${_obd_fatfs_src}/ffconf.h"
    "${_obd_fatfs_patcher}")
execute_process(COMMAND "${_obd_python}" "${_obd_fatfs_patcher}"
    "${_obd_fatfs_src}" "${_obd_fatfs_patch_dir}"
    RESULT_VARIABLE _obd_fatfs_result OUTPUT_VARIABLE _obd_fatfs_output
    ERROR_VARIABLE _obd_fatfs_error OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _obd_fatfs_result EQUAL 0)
    message(FATAL_ERROR "${_obd_fatfs_output}\n${_obd_fatfs_error}")
endif()
get_target_property(_obd_fatfs_sources ${_obd_fatfs_lib} SOURCES)
foreach(_obd_fatfs_name IN ITEMS ff.c ffunicode.c)
    set(_obd_fatfs_original "${_obd_fatfs_src}/${_obd_fatfs_name}")
    list(REMOVE_ITEM _obd_fatfs_sources "${_obd_fatfs_original}")
    list(APPEND _obd_fatfs_sources "${_obd_fatfs_patch_dir}/${_obd_fatfs_name}")
endforeach()
set_property(TARGET ${_obd_fatfs_lib} PROPERTY SOURCES "${_obd_fatfs_sources}")
target_include_directories(${_obd_fatfs_lib} BEFORE PUBLIC "${_obd_fatfs_patch_dir}")
message(STATUS "${_obd_fatfs_output}")
