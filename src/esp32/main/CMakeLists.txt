file(GLOB U8G2_SRC ../../u8g2/csrc/*.c)

# ── Embedded bootloader header ────────────────────────────────────────────────
# Auto-generates src/esp32/bootloader_data.h from the bootloader project.bin
# by running generate_bootloader_header.py.  The file is (re)generated whenever
# the source bitstream or the generator script changes.
set(_BL_BIN       "${CMAKE_CURRENT_SOURCE_DIR}/../../../../papilio_tang_bootloader/fpga/impl/pnr/project.bin")
set(_BL_HEADER    "${CMAKE_CURRENT_SOURCE_DIR}/../bootloader_data.h")
set(_BL_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/../../../generate_bootloader_header.py")

# add_custom_command is not available in CMake script mode (used by ESP-IDF's
# dependency scanner).  Guard everything that requires a build context.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    if(EXISTS "${_BL_BIN}")
        add_custom_command(
            OUTPUT  "${_BL_HEADER}"
            COMMAND ${CMAKE_COMMAND} -E echo "Generating bootloader_data.h..."
            COMMAND python "${_BL_GENERATOR}" "${_BL_BIN}" "${_BL_HEADER}"
            DEPENDS "${_BL_BIN}" "${_BL_GENERATOR}"
            COMMENT "Compressing bootloader bitstream into C header"
            VERBATIM
        )
    elseif(NOT EXISTS "${_BL_HEADER}")
        message(FATAL_ERROR
            "bootloader_data.h not found and bootloader binary is missing.\n"
            "Either copy project.bin to ${_BL_BIN}\n"
            "or run: python ${_BL_GENERATOR} <path/to/project.bin> ${_BL_HEADER}")
    endif()
endif()
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(SRCS
	"../../main.c"
	"../../hid.c"
	"../../hidparser.c"
	"../../sysctrl.c"
	"../../sdc.c"
	"../../menu.c"
	"../../osd_u8g2.c"
	"../../inifile.c"
	"../../core.c"
	"../../core_atarist.c"
	"../../core_c64.c"
	"../../core_vic20.c"
	"../../core_amiga.c"
	"../../core_atari2600.c"
	"../../core_snes.c"
	"../../config.c"
	"../../xml.c"
	"../mcu_hw.c"
	"../wifi_log.c"
	"../ota_server.c"
	"../jtag_gowin.c"
	"../bt_hid.c"
	"../esp_hid_gap.c"
	${U8G2_SRC}	
	../../u8g2/sys/bitmap/common/u8x8_d_bitmap.c

	INCLUDE_DIRS
	"."
	".."
	"../../u8g2/csrc"
)

# Wrap _write_r at link time so __wrap__write_r intercepts both printf()
# (used by debugf) and ESP_LOGx output. Both ultimately call _write_r(r,fd,buf,n)
# to flush bytes to stdout — this is the single correct intercept point.
target_link_options(${COMPONENT_LIB} INTERFACE -Wl,--wrap=_write_r)

# Ensure the bootloader header is generated before ota_server.c is compiled.
if(NOT CMAKE_SCRIPT_MODE_FILE AND EXISTS "${_BL_BIN}")
    add_custom_target(__bootloader_data_h DEPENDS "${_BL_HEADER}")
    add_dependencies(${COMPONENT_LIB} __bootloader_data_h)
endif()

unset(_BL_BIN)
unset(_BL_HEADER)
unset(_BL_GENERATOR)
