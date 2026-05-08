# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Patch ffconf.h settings that are not exposed through Kconfig.
# This replaces the manual edit step previously documented in README.md.
set(_ffconf "$ENV{IDF_PATH}/components/fatfs/src/ffconf.h")
if(EXISTS "${_ffconf}")
    string(ASCII 9 _TAB)
    file(READ "${_ffconf}" _content)
    set(_orig "${_content}")
    # IDF 6.0+: FF_USE_STRFUNC is now Kconfig-controlled via CONFIG_FATFS_USE_STRFUNC_*
    # in a #if/#elif/#else block. Use CONFIG_FATFS_USE_STRFUNC_WITHOUT_CRLF_CONV=y in
    # sdkconfig.defaults instead of patching the header directly.
    string(REGEX REPLACE "#define FF_FS_EXFAT[ \t]+0"    "#define FF_FS_EXFAT${_TAB}${_TAB}1" _content "${_content}")
    # IDF 6.0+ ffconf.h still references these Kconfig symbols bare (no #ifdef guard).
    # When disabled (default n), CONFIG_FATFS_USE_LABEL / CONFIG_FATFS_USE_DYN_BUFFERS
    # are not emitted in sdkconfig.h, causing an undeclared identifier compile error.
    # Replace with a literal 0 to keep the feature disabled without the missing symbol.
    string(REGEX REPLACE "#define FF_USE_LABEL[ \t]+CONFIG_FATFS_USE_LABEL" "#define FF_USE_LABEL${_TAB}${_TAB}0" _content "${_content}")
    string(REGEX REPLACE "#define FF_USE_DYN_BUFFER[ \t]+CONFIG_FATFS_USE_DYN_BUFFERS" "#define FF_USE_DYN_BUFFER${_TAB}0" _content "${_content}")
    if(NOT _content STREQUAL _orig)
        file(WRITE "${_ffconf}" "${_content}")
        message(STATUS "Patched ${_ffconf}: FF_FS_EXFAT=1")
    endif()
    unset(_ffconf)
    unset(_TAB)
    unset(_content)
    unset(_orig)
endif()

# Load local credentials (gitignored) on top of the shared defaults.
# sdkconfig.defaults.local overrides sdkconfig.defaults for WIFI_LOG_SSID/PASSWORD.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

set(PROJECT fpga_companion)
project(${PROJECT})

# target_link_options(${PROJECT} PRIVATE --print-memory-usage)
