cmake_minimum_required(VERSION 3.16)

# Re-configure automatically when git HEAD changes (new commit or branch switch)
if(EXISTS ${CMAKE_SOURCE_DIR}/.git/HEAD)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/.git/HEAD)
endif()

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
project(LianMing-PSU-Controller)
add_compile_definitions(FIRMWARE_VERSION="${GIT_FW_VERSION}")
