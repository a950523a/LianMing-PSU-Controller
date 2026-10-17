cmake_minimum_required(VERSION 3.16)

# Re-configure automatically when git HEAD moves. .git/HEAD alone only changes on a branch
# switch ("ref: refs/heads/main" stays the same across commits), so the version stayed at
# whatever the first configure saw — "unknown" on the desktop (2026-10-14). .git/logs/HEAD is
# appended on every commit, checkout and reset; .git/index catches the -dirty flag.
foreach(f HEAD logs/HEAD index)
    if(EXISTS ${CMAKE_SOURCE_DIR}/.git/${f})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/.git/${f})
    endif()
endforeach()

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_FW_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_FW_VERSION)
    set(GIT_FW_VERSION "unknown")
endif()
message(STATUS "Firmware version: ${GIT_FW_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Must be a build property set before project(): add_compile_definitions() after project() only
# reached the top-level dummy target, so every component (web_ctrl, serial_cmd) fell back to
# config_common.h's "unknown" (2026-10-18).
idf_build_set_property(COMPILE_DEFINITIONS "FIRMWARE_VERSION=\"${GIT_FW_VERSION}\"" APPEND)
set(PROJECT_VER "${GIT_FW_VERSION}")
project(LianMing-PSU-Controller)
