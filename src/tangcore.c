/* tangcore.c — TangCore BL616-compatible UART protocol for ESP32 FPGA-Companion
 *
 * Implements the tangcore 0.9+ framed UART protocol as used by nestang and snestang
 * when built with MCU_BL616 / iosys_bl616.
 *
 * Frame: 0xAA len_hi len_lo cmd [payload]
 *   len = total bytes in payload including cmd byte
 *
 * This file is compiled only for ESP32 (ESP_PLATFORM guard in CMakeLists).
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ff.h>

#ifdef ESP_PLATFORM
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"
#include "bt_hid.h"  /* for bt_hid_set_scan_paused() — gate BLE during ROM load */
#endif

#include "mcu_hw.h"
#include "tangcore.h"
#include "debug.h"

/* ---- shared interface flag ---- */
volatile int active_interface = 0;  /* 0=undecided, 1=SPI, 2=UART */

/* ---- joystick state (updated by tangcore_set_joy) ---- */
static volatile uint16_t tc_joy[2] = {0, 0};

void tangcore_set_joy(int player, uint16_t buttons) {
    if(player >= 0 && player < 2)
        tc_joy[player] = buttons;
}

/* ---- Protocol helpers ---- */

/* Send a framed command: 0xAA len_hi len_lo cmd [payload]
 * len = 1 (cmd) + payload_len
 */
static void tc_send(uint8_t cmd, const uint8_t *payload, uint16_t payload_len) {
    uint16_t frame_len = (uint16_t)(1 + payload_len);
    uint8_t hdr[4];
    hdr[0] = TANGCORE_SYNC_BYTE;
    hdr[1] = (uint8_t)(frame_len >> 8);
    hdr[2] = (uint8_t)(frame_len & 0xff);
    hdr[3] = cmd;
    mcu_hw_uart_tx_buf(hdr, 4);
    if(payload && payload_len)
        mcu_hw_uart_tx_buf(payload, payload_len);
}

/* Drain any stale bytes from the RX buffer */
static void tc_flush_rx(void) {
    while(mcu_hw_uart_rx_available())
        mcu_hw_uart_rx_byte();
}

/* Read exactly `n` bytes into buf.  Returns false on timeout.
 * Timeout is expressed in 1ms poll increments up to max_waits polls.
 */
static bool tc_read_bytes(uint8_t *buf, int n, int max_waits_ms) {
    int got = 0;
    int waited = 0;
    while(got < n) {
        if(mcu_hw_uart_rx_available()) {
            buf[got++] = mcu_hw_uart_rx_byte();
            waited = 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
            if(++waited > max_waits_ms) return false;
        }
    }
    return true;
}

/* Send query core ID (cmd 0x01) and wait up to 200ms for response 0x01 + core_id.
 * Returns core_id (1=NES, 2=SNES) or 0 on failure.
 */
static uint8_t tc_query_core_id(void) {
    tc_flush_rx();
    tc_send(TANGCORE_CMD_QUERY_CORE, NULL, 0);

    /* Response frame: 0xAA len_hi len_lo 0x01 core_id */
    uint8_t buf[5];
    if(!tc_read_bytes(buf, 5, 200)) return 0;
    if(buf[0] != TANGCORE_SYNC_BYTE) return 0;
    /* buf[1..2] = len, buf[3] = 0x01, buf[4] = core_id */
    if(buf[3] != TANGCORE_RESP_CORE_ID) return 0;
    return buf[4];
}

/* OSD control */
static void tc_osd_enable(bool on) {
    uint8_t b = on ? 1 : 0;
    tc_send(TANGCORE_CMD_OSD_ENABLE, &b, 1);
}

static void tc_move_cursor(uint8_t x, uint8_t y) {
    uint8_t p[2] = {x, y};
    tc_send(TANGCORE_CMD_MOVE_CURSOR, p, 2);
}

static void tc_write_string(const char *s) {
    tc_send(TANGCORE_CMD_WRITE_STRING, (const uint8_t *)s, (uint16_t)strlen(s));
}

static void tc_set_loading(uint8_t state) {
    tc_send(TANGCORE_CMD_SET_LOADING, &state, 1);
}

/* Forward current joystick state to FPGA via cmd 0x09.
 * Format: hid1_hi hid1_lo hid2_hi hid2_lo (MSB first per iosys_bl616.v)
 */
static void tc_send_joystick(void) {
    uint16_t j1 = tc_joy[0];
    uint16_t j2 = tc_joy[1];
    uint8_t p[4] = {
        (uint8_t)(j1 >> 8), (uint8_t)(j1 & 0xff),
        (uint8_t)(j2 >> 8), (uint8_t)(j2 & 0xff)
    };
    tc_send(TANGCORE_CMD_JOYSTICK, p, 4);
}

/* ---- Simple OSD file browser ---- */

/* Display a full-width row (30 chars) at row y, padded/truncated */
static void tc_osd_row(uint8_t y, const char *text) {
    char buf[31];
    int len = (int)strlen(text);
    if(len > 30) len = 30;
    memcpy(buf, text, len);
    memset(buf + len, ' ', 30 - len);
    buf[30] = '\0';
    tc_move_cursor(0, y);
    tc_write_string(buf);
}

#define MAX_DIR_ENTRIES   32
#define MAX_NAME_LEN      28

static char dir_entries[MAX_DIR_ENTRIES][MAX_NAME_LEN + 1];
static int  dir_count = 0;

/* Populate dir_entries[] with ROM files on the SD card.
 * Supported extensions: .nes  .sfc  .smc  .zip (NES/SNES)
 * Returns number of entries found.
 */
/* FatFS drive number for the tangcore direct SD mount (drive 1 avoids
 * conflict with sdc.c which uses drive 0 for the FPGA SPI bridge path). */
#define TC_SD_DRIVE  1
#define TC_SD_PATH   "1:/"

#ifdef ESP_PLATFORM
static FATFS s_tc_fs;

/* Initialize SPI3 for direct SD card access after mcu_hw_spi_deinit().
 * Uses SDSPI host + sdmmc card init + FatFS diskio registration.
 * No VFS (esp_vfs_fat) needed. */
static void tangcore_sd_init(void) {
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = 11,
        .miso_io_num = 3,
        .sclk_io_num = 12,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if(ret != ESP_OK) {
        debugf("tangcore_sd: SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = sdspi_host_init();
    if(ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        debugf("tangcore_sd: SDSPI host init failed: %s", esp_err_to_name(ret));
        return;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = 10;
    slot_cfg.host_id   = SPI3_HOST;

    sdspi_dev_handle_t sdspi_handle;
    ret = sdspi_host_init_device(&slot_cfg, &sdspi_handle);
    if(ret != ESP_OK) {
        debugf("tangcore_sd: SDSPI device init failed: %s", esp_err_to_name(ret));
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = sdspi_handle;
    host.max_freq_khz = 10000;  /* 10 MHz -- conservative for FPGA pass-through */

    sdmmc_card_t *card = malloc(sizeof(sdmmc_card_t));
    if(!card) { debugf("tangcore_sd: OOM"); return; }

    ret = sdmmc_card_init(&host, card);
    if(ret != ESP_OK) {
        debugf("tangcore_sd: card init failed: %s", esp_err_to_name(ret));
        free(card);
        return;
    }
    sdmmc_card_print_info(stdout, card);

    ff_diskio_register_sdmmc(TC_SD_DRIVE, card);

    FRESULT rc = f_mount(&s_tc_fs, TC_SD_PATH, 1);
    if(rc != FR_OK) {
        debugf("tangcore_sd: f_mount failed: %d", (int)rc);
    } else {
        debugf("tangcore_sd: SD mounted on drive %s", TC_SD_PATH);
    }
}
#endif /* ESP_PLATFORM */

static int tc_scan_dir(void) {
    dir_count = 0;
    FF_DIR dp;
    FILINFO fno;
    if(f_opendir(&dp, TC_SD_PATH) != FR_OK) {
        debugf("tc_scan_dir: f_opendir failed");
        return 0;
    }

    while(dir_count < MAX_DIR_ENTRIES) {
        if(f_readdir(&dp, &fno) != FR_OK || fno.fname[0] == '\0') break;
        if(fno.fattrib & AM_DIR) continue;  /* skip subdirs for now */

        /* Check extension */
        const char *ext = strrchr(fno.fname, '.');
        if(!ext) continue;
        if(strcasecmp(ext, ".nes") != 0 &&
           strcasecmp(ext, ".sfc") != 0 &&
           strcasecmp(ext, ".smc") != 0 &&
           strcasecmp(ext, ".zip") != 0) continue;

        strncpy(dir_entries[dir_count], fno.fname, MAX_NAME_LEN);
        dir_entries[dir_count][MAX_NAME_LEN] = '\0';
        dir_count++;
    }
    f_closedir(&dp);
    return dir_count;
}

/* Render the file list OSD starting at list_top, highlighting cursor row */
static void tc_osd_render_list(int cursor, int list_top, int visible_rows) {
    tc_osd_row(0, "   Select ROM to load   ");
    tc_osd_row(1, "------------------------");
    for(int i = 0; i < visible_rows; i++) {
        int idx = list_top + i;
        char line[32];
        if(idx < dir_count) {
            snprintf(line, sizeof(line), "%c%-28s",
                     (idx == cursor) ? '>' : ' ', dir_entries[idx]);
        } else {
            line[0] = '\0';
        }
        tc_osd_row((uint8_t)(2 + i), line);
    }
    /* Status row */
    char status[32];
    snprintf(status, sizeof(status), " %d/%d files  ", cursor + 1, dir_count);
    tc_osd_row((uint8_t)(2 + visible_rows), status);
}

/* Load the selected ROM file via UART.
 * Returns true on success.
 */
static bool tc_load_rom_file(const char *name) {
    char path[144];
    snprintf(path, sizeof(path), "1:/%s", name);

    FIL f;
    if(f_open(&f, path, FA_READ) != FR_OK) {
        debugf("tangcore: cannot open %s", path);
        return false;
    }

    FSIZE_t total = f_size(&f);
    debugf("tangcore: loading %s (%lu bytes total)", name, (unsigned long)total);

#ifdef ESP_PLATFORM
    /* Pause BLE HID scanning for the duration of the ROM transfer.
     * The 5-second blocking scan in bt_hid_scan_task() can starve the UART
     * TX path long enough that the FPGA loader times out and the load is
     * aborted (symptom: screen stays purple after picking a ROM). */
    bt_hid_set_scan_paused(true);
    debugf("tangcore: BLE scan paused for ROM load");
#endif

    tc_set_loading(1);
    tc_osd_row(9, "  Loading...           ");

    /* Max FPGA frame size is 2047 bytes (len_reg[15:8] must be < 8).
     * frame_len = 1 (cmd) + payload, so max payload = 2046.
     * Use 1024 to stay well within limits. */
    uint8_t chunk[1024];
    UINT br;
    FRESULT rc;
    uint32_t bytes_sent = 0;
    int chunk_num = 0;
    do {
        rc = f_read(&f, chunk, sizeof(chunk), &br);
        if(rc == FR_OK && br > 0) {
            tc_send(TANGCORE_CMD_LOAD_ROM, chunk, (uint16_t)br);
            bytes_sent += br;
            chunk_num++;
            debugf("tangcore: chunk %d: %u bytes (total so far: %lu)", chunk_num, (unsigned)br, (unsigned long)bytes_sent);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    } while(rc == FR_OK && br > 0);

    f_close(&f);

    debugf("tangcore: %d chunks, %lu bytes sent — waiting for UART TX to drain", chunk_num, (unsigned long)bytes_sent);

    /* Wait for all queued UART bytes to be physically transmitted before
     * asserting loading=0.  Without this the FPGA might receive loading=0
     * before the last ROM data chunk has left the TX FIFO. */
    mcu_hw_uart_tx_flush();

    debugf("tangcore: sending set_loading(0)");
    tc_set_loading(0);
    /* Small delay to give the FPGA time to process loading=0 before the OSD
     * disable command arrives. */
    vTaskDelay(pdMS_TO_TICKS(10));

#ifdef ESP_PLATFORM
    /* Re-enable BLE HID scanning now that the load is complete. */
    bt_hid_set_scan_paused(false);
    debugf("tangcore: BLE scan resumed");
#endif

    debugf("tangcore: load %s (rc=%d)", (rc == FR_OK) ? "complete" : "error", (int)rc);
    return (rc == FR_OK);
}

/* Drain unsolicited FPGA→ESP32 joypad frames (0x03 every 20ms).
 * Called during OSD to prevent RX buffer overflow. */
static void tc_drain_fpga_events(void) {
    while(mcu_hw_uart_rx_available() >= 5) {
        uint8_t b = mcu_hw_uart_rx_byte();
        if(b != TANGCORE_SYNC_BYTE) continue;
        /* read rest of frame header */
        uint8_t hdr[3];
        if(!tc_read_bytes(hdr, 3, 5)) break;
        uint16_t len = ((uint16_t)hdr[0] << 8) | hdr[1];
        uint8_t resp_cmd = hdr[2];
        /* drain payload bytes */
        if(len > 1) {
            uint16_t payload_len = len - 1;
            for(uint16_t i = 0; i < payload_len; i++) {
                if(!mcu_hw_uart_rx_available()) break;
                mcu_hw_uart_rx_byte();
            }
        }
        (void)resp_cmd;
    }
}

/* ---- FreeRTOS task ---- */

void tangcore_task(void *p) {
    (void)p;
    debugf("tangcore_task: starting probe");

    /* Wait a moment for FPGA to boot before probing */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Probe: try up to 10 times, 200ms apart */
    uint8_t core_id = 0;
    for(int attempt = 0; attempt < 10 && active_interface == 0; attempt++) {
        core_id = tc_query_core_id();
        if(core_id == TANGCORE_CORE_NES || core_id == TANGCORE_CORE_SNES) {
            active_interface = 2;
            debugf("tangcore_task: found core_id=%d (%s) — UART interface active",
                   core_id, (core_id == TANGCORE_CORE_NES) ? "NES" : "SNES");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if(active_interface != 2) {
        debugf("tangcore_task: no UART core found, idling");
        for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* ---- UART core is live ---- */

    /* Init SD card: free the FPGA SPI bridge, reinit SPI3 for direct SD access */
#ifdef ESP_PLATFORM
    mcu_hw_spi_deinit();
    tangcore_sd_init();
#endif

    /* Show OSD file browser */
    tc_osd_enable(true);
    tc_scan_dir();

    if(dir_count == 0) {
        tc_osd_row(0, "  No ROMs found on SD   ");
        tc_osd_row(1, "  Place .nes/.sfc files ");
        tc_osd_row(2, "  in the SD card root.  ");
        /* Idle — at least joystick forwarding still works */
        for(;;) {
            tc_send_joystick();
            tc_drain_fpga_events();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* File browser loop */
    const int VISIBLE = 8;
    int cursor = 0;
    int list_top = 0;
    bool rom_loaded = false;

    tc_osd_render_list(cursor, list_top, VISIBLE);

    TickType_t last_joy_send = xTaskGetTickCount();
    uint16_t last_joy0 = 0xFFFFu;  /* force initial send */

    for(;;) {
        /* Forward joystick every 20ms */
        if((xTaskGetTickCount() - last_joy_send) >= pdMS_TO_TICKS(20)) {
            tc_send_joystick();
            last_joy_send = xTaskGetTickCount();
        }

        tc_drain_fpga_events();

        /* Navigation — use current joystick state directly */
        uint16_t joy = tc_joy[0];
        if(joy == last_joy0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint16_t newly_pressed = joy & ~last_joy0;
        last_joy0 = joy;

        bool redraw = false;

        if(newly_pressed & TANGCORE_BTN_UP) {
            if(cursor > 0) {
                cursor--;
                if(cursor < list_top) list_top--;
                redraw = true;
            }
        }
        if(newly_pressed & TANGCORE_BTN_DOWN) {
            if(cursor < dir_count - 1) {
                cursor++;
                if(cursor >= list_top + VISIBLE) list_top++;
                redraw = true;
            }
        }
        if(newly_pressed & (TANGCORE_BTN_A | TANGCORE_BTN_B)) {
            /* Load selected ROM */
            if(!rom_loaded || (newly_pressed & TANGCORE_BTN_A)) {
                tc_osd_render_list(cursor, list_top, VISIBLE);
                if(tc_load_rom_file(dir_entries[cursor])) {
                    rom_loaded = true;
                    tc_osd_enable(false);
                    debugf("tangcore_task: ROM loaded, entering run loop");
                    goto run_loop;
                } else {
                    tc_osd_row(9, "  Load failed!         ");
                }
            }
        }
        if(newly_pressed & TANGCORE_BTN_START) {
            /* START alone: hide OSD (if ROM already loaded) */
            if(rom_loaded) {
                tc_osd_enable(false);
                goto run_loop;
            }
        }

        if(redraw) tc_osd_render_list(cursor, list_top, VISIBLE);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

run_loop:
    /* Run loop: forward joystick + handle OSD toggle */
    debugf("tangcore_task: entering run loop");
    last_joy0 = 0xFFFFu;
    for(;;) {
        tc_send_joystick();
        tc_drain_fpga_events();

        uint16_t joy = tc_joy[0];
        uint16_t newly_pressed = joy & ~last_joy0;
        last_joy0 = joy;

        /* SELECT + START = show OSD */
        if((newly_pressed & TANGCORE_BTN_SELECT) && (joy & TANGCORE_BTN_START)) {
            tc_osd_enable(true);
            tc_scan_dir();
            cursor = 0; list_top = 0;
            tc_osd_render_list(cursor, list_top, VISIBLE);
            /* Re-enter browser */
            last_joy0 = 0xFFFFu;
            goto browser_loop;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
        continue;

browser_loop:
        for(;;) {
            tc_send_joystick();
            tc_drain_fpga_events();

            joy = tc_joy[0];
            newly_pressed = joy & ~last_joy0;
            last_joy0 = joy;
            bool redraw = false;

            if(newly_pressed & TANGCORE_BTN_UP) {
                if(cursor > 0) { cursor--; if(cursor < list_top) list_top--; redraw = true; }
            }
            if(newly_pressed & TANGCORE_BTN_DOWN) {
                if(cursor < dir_count - 1) { cursor++; if(cursor >= list_top + VISIBLE) list_top++; redraw = true; }
            }
            if(newly_pressed & (TANGCORE_BTN_A | TANGCORE_BTN_B)) {
                if(tc_load_rom_file(dir_entries[cursor])) {
                    tc_osd_enable(false);
                    last_joy0 = 0xFFFFu;
                    goto run_loop_restart;
                } else {
                    tc_osd_row(9, "  Load failed!         ");
                }
            }
            if(newly_pressed & TANGCORE_BTN_START) {
                tc_osd_enable(false);
                last_joy0 = 0xFFFFu;
                goto run_loop_restart;
            }
            if(redraw) tc_osd_render_list(cursor, list_top, VISIBLE);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

run_loop_restart:
        /* Continue outer run_loop */
        ;
    }
}
