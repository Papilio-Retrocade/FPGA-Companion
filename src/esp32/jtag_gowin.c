/*
 * jtag_gowin.c - Minimal JTAG programmer for Gowin GW2A FPGAs
 *
 * Direct bitstream programming - no SVF conversion required.
 * Works with native .fs files from Gowin tools.
 */

#include "jtag_gowin.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jtag_gowin";

/* ========================================================================= */
/* JTAG State Machine                                                        */
/* ========================================================================= */

typedef enum {
    TEST_LOGIC_RESET = 0,
    RUN_TEST_IDLE,
    SELECT_DR_SCAN,
    CAPTURE_DR,
    SHIFT_DR,
    EXIT1_DR,
    PAUSE_DR,
    EXIT2_DR,
    UPDATE_DR,
    SELECT_IR_SCAN,
    CAPTURE_IR,
    SHIFT_IR,
    EXIT1_IR,
    PAUSE_IR,
    EXIT2_IR,
    UPDATE_IR
} jtag_state_t;

/* Global state */
static jtag_pins_t g_pins;
static jtag_state_t g_state = TEST_LOGIC_RESET;

/* ========================================================================= */
/* Low-Level Bit-Bang Functions                                              */
/* ========================================================================= */

/* Delay for TCK timing - adjust based on your needs */
#define TCK_DELAY_US 1

static inline void tck_pulse(void)
{
    gpio_set_level(g_pins.tck, 0);
    esp_rom_delay_us(TCK_DELAY_US);
    gpio_set_level(g_pins.tck, 1);
    esp_rom_delay_us(TCK_DELAY_US);
}

static inline void set_tms(uint8_t val)
{
    gpio_set_level(g_pins.tms, val ? 1 : 0);
}

static inline void set_tdi(uint8_t val)
{
    gpio_set_level(g_pins.tdi, val ? 1 : 0);
}

static inline uint8_t get_tdo(void)
{
    return gpio_get_level(g_pins.tdo) ? 1 : 0;
}

/**
 * Clock one bit through JTAG TAP
 * 
 * @param tms  TMS value (state control)
 * @param tdi  TDI value (data in)
 * @return TDO value (data out)
 */
static uint8_t jtag_clock_bit(uint8_t tms, uint8_t tdi)
{
    set_tms(tms);
    set_tdi(tdi);
    
    uint8_t tdo = get_tdo();  // Sample TDO on falling edge
    tck_pulse();              // Clock the bit
    
    return tdo;
}

/* ========================================================================= */
/* JTAG State Machine Navigation                                             */
/* ========================================================================= */

/**
 * Navigate to Test-Logic-Reset state
 */
static void jtag_goto_reset(void)
{
    /* 5+ TMS=1 cycles always reach reset */
    for (int i = 0; i < 5; i++) {
        jtag_clock_bit(1, 0);
    }
    g_state = TEST_LOGIC_RESET;
}

/**
 * Navigate to Run-Test/Idle state
 */
static void jtag_goto_idle(void)
{
    jtag_goto_reset();
    jtag_clock_bit(0, 0);  // Reset → Idle
    g_state = RUN_TEST_IDLE;
}

/**
 * Stay in Run-Test/Idle for N cycles
 */
static void jtag_run_idle(uint32_t cycles)
{
    for (uint32_t i = 0; i < cycles; i++) {
        jtag_clock_bit(0, 0);
    }
}

/* ========================================================================= */
/* JTAG IR/DR Scan Functions                                                 */
/* ========================================================================= */

/**
 * Shift instruction into IR (Instruction Register)
 * 
 * Gowin FPGAs use 8-bit IR.
 */
static void jtag_ir_scan(uint8_t instruction)
{
    /* Navigate: Idle → Select-DR → Select-IR → Capture-IR → Shift-IR */
    jtag_clock_bit(1, 0);  // Idle → Select-DR
    jtag_clock_bit(1, 0);  // Select-DR → Select-IR
    jtag_clock_bit(0, 0);  // Select-IR → Capture-IR
    jtag_clock_bit(0, 0);  // Capture-IR → Shift-IR
    
    /* Shift 8 bits, LSB first */
    for (int i = 0; i < 7; i++) {
        jtag_clock_bit(0, (instruction >> i) & 1);
    }
    
    /* Last bit transitions to Exit1-IR (TMS=1) */
    jtag_clock_bit(1, (instruction >> 7) & 1);
    
    /* Exit1-IR → Update-IR → Idle */
    jtag_clock_bit(1, 0);  // Exit1 → Update
    jtag_clock_bit(0, 0);  // Update → Idle
    
    g_state = RUN_TEST_IDLE;
}

/**
 * Shift data into DR (Data Register)
 * 
 * Used for both IDCODE reads and bitstream programming.
 */
static void jtag_dr_scan(const uint8_t *data_in, uint8_t *data_out, size_t bits)
{
    /* Navigate: Idle → Select-DR → Capture-DR → Shift-DR */
    jtag_clock_bit(1, 0);  // Idle → Select-DR
    jtag_clock_bit(0, 0);  // Select-DR → Capture-DR
    jtag_clock_bit(0, 0);  // Capture-DR → Shift-DR
    
    /* Shift data, LSB first */
    for (size_t i = 0; i < bits; i++) {
        uint8_t tdi = 0;
        if (data_in) {
            tdi = (data_in[i / 8] >> (i % 8)) & 1;
        }
        
        uint8_t tms = (i == bits - 1) ? 1 : 0;  // Exit on last bit
        uint8_t tdo = jtag_clock_bit(tms, tdi);
        
        if (data_out) {
            data_out[i / 8] |= (tdo << (i % 8));
        }
    }
    
    /* Exit1-DR → Update-DR → Idle */
    jtag_clock_bit(1, 0);  // Exit1 → Update
    jtag_clock_bit(0, 0);  // Update → Idle
    
    g_state = RUN_TEST_IDLE;
}

/* ========================================================================= */
/* Gowin-Specific JTAG Instructions                                          */
/* ========================================================================= */

#define GOWIN_IR_IDCODE    0x09  /* Read device ID */
#define GOWIN_IR_JPROGRAM  0x3C  /* Erase SRAM configuration */
#define GOWIN_IR_CFG_IN    0x39  /* Shift in configuration data */
#define GOWIN_IR_JSTART    0x3D  /* Start FPGA */
#define GOWIN_IR_BYPASS    0xFF  /* Bypass */

/* ========================================================================= */
/* Public API Implementation                                                 */
/* ========================================================================= */

esp_err_t jtag_gowin_init(const jtag_pins_t *pins)
{
    /* Use provided pins or defaults */
    if (pins) {
        g_pins = *pins;
    } else {
        jtag_pins_t defaults = JTAG_DEFAULT_PINS;
        g_pins = defaults;
    }
    
    ESP_LOGI(TAG, "Initializing JTAG: TDI=%d TDO=%d TCK=%d TMS=%d",
             g_pins.tdi, g_pins.tdo, g_pins.tck, g_pins.tms);
    
    /* Configure GPIO pins */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << g_pins.tdi) | (1ULL << g_pins.tck) | (1ULL << g_pins.tms),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    io_conf.pin_bit_mask = (1ULL << g_pins.tdo);
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);
    
    /* Initialize to known state */
    gpio_set_level(g_pins.tck, 0);
    gpio_set_level(g_pins.tms, 1);
    gpio_set_level(g_pins.tdi, 0);
    
    /* Reset TAP state machine */
    jtag_goto_reset();
    jtag_goto_idle();
    
    ESP_LOGI(TAG, "JTAG initialized");
    return ESP_OK;
}

esp_err_t jtag_gowin_read_idcode(uint32_t *idcode)
{
    if (!idcode) return ESP_ERR_INVALID_ARG;
    
    /* Reset and load IDCODE instruction */
    jtag_goto_idle();
    jtag_ir_scan(GOWIN_IR_IDCODE);
    
    /* Read 32-bit IDCODE from DR */
    uint32_t id = 0;
    jtag_dr_scan(NULL, (uint8_t*)&id, 32);
    
    *idcode = id;
    
    ESP_LOGI(TAG, "IDCODE: 0x%08lX (%s)", id, jtag_gowin_device_name(id));
    return ESP_OK;
}

esp_err_t jtag_gowin_program_sram(const uint8_t *bitstream, size_t length)
{
    if (!bitstream || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Programming SRAM: %zu bytes", length);
    int64_t start_time = esp_timer_get_time();
    
    /* 1. Reset TAP */
    jtag_goto_idle();
    
    /* 2. Verify device */
    uint32_t idcode;
    jtag_gowin_read_idcode(&idcode);
    if ((idcode & 0x0FFFFFFF) != (GOWIN_IDCODE_GW2A_18 & 0x0FFFFFFF)) {
        ESP_LOGW(TAG, "Unexpected IDCODE 0x%08lX (expected 0x%08lX)",
                 idcode, GOWIN_IDCODE_GW2A_18);
        /* Continue anyway - might be different variant */
    }
    
    /* 3. Load JPROGRAM instruction - clears SRAM configuration */
    ESP_LOGI(TAG, "Erasing SRAM configuration");
    jtag_ir_scan(GOWIN_IR_JPROGRAM);
    jtag_run_idle(10000);  // Wait for erase (10ms)
    
    /* 4. Load CFG_IN instruction - prepare to receive bitstream */
    ESP_LOGI(TAG, "Loading CFG_IN instruction");
    jtag_ir_scan(GOWIN_IR_CFG_IN);
    
    /* 5. Shift bitstream into DR */
    ESP_LOGI(TAG, "Shifting bitstream (%zu bytes)...", length);
    size_t bits = length * 8;
    
    /* Shift in chunks for progress reporting */
    size_t chunk_size = 8192;  // 64 Kbits
    for (size_t byte_offset = 0; byte_offset < length; byte_offset += chunk_size) {
        size_t chunk_bytes = (byte_offset + chunk_size > length) ? 
                             (length - byte_offset) : chunk_size;
        
        jtag_dr_scan(&bitstream[byte_offset], NULL, chunk_bytes * 8);
        
        if ((byte_offset % (64 * 1024)) == 0 && byte_offset > 0) {
            ESP_LOGI(TAG, "Progress: %zu / %zu bytes", byte_offset, length);
        }
    }
    
    /* 6. Load JSTART instruction - start FPGA */
    ESP_LOGI(TAG, "Starting FPGA");
    jtag_ir_scan(GOWIN_IR_JSTART);
    jtag_run_idle(100000);  // Wait for startup (100ms)
    
    /* 7. Return to idle */
    jtag_goto_idle();
    
    int64_t elapsed_us = esp_timer_get_time() - start_time;
    ESP_LOGI(TAG, "Programming complete in %.2f seconds", elapsed_us / 1000000.0);
    
    return ESP_OK;
}

esp_err_t jtag_gowin_verify(void)
{
    uint32_t idcode;
    esp_err_t ret = jtag_gowin_read_idcode(&idcode);
    
    if (ret == ESP_OK && idcode != 0 && idcode != 0xFFFFFFFF) {
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Verification failed - IDCODE invalid: 0x%08lX", idcode);
    return ESP_FAIL;
}

const char* jtag_gowin_device_name(uint32_t idcode)
{
    switch (idcode & 0x0FFFFFFF) {  /* Mask version bits */
        case (GOWIN_IDCODE_GW2A_18 & 0x0FFFFFFF):
            return "GW2A-18";
        case (GOWIN_IDCODE_GW2AR_18 & 0x0FFFFFFF):
            return "GW2AR-18";
        case (GOWIN_IDCODE_GW1N_1 & 0x0FFFFFFF):
            return "GW1N-1";
        case (GOWIN_IDCODE_GW1NR_9 & 0x0FFFFFFF):
            return "GW1NR-9";
        default:
            return "Unknown";
    }
}
