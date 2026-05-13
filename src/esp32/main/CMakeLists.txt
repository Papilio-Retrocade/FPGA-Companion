file(GLOB U8G2_SRC ../../u8g2/csrc/*.c)

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
	"../../config.c"
	"../../xml.c"
	"../mcu_hw.c"
	"../wifi_log.c"
	"../ota_server.c"
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
