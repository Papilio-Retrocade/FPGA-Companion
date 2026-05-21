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
#include "soc/gpio_reg.h"
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

/* Precomputed bitmasks for direct register access — set once in jtag_gowin_init().
 * REG_WRITE to GPIO_OUT_W1TS/W1TC is ~3x faster than gpio_set_level() because it
 * bypasses the function call chain and GPIO matrix lookup. */
static uint32_t g_tck_mask;
static uint32_t g_tms_mask;
static uint32_t g_tdi_mask;
static uint32_t g_tdo_mask;

/* ========================================================================= */
/* Low-Level Bit-Bang Functions                                              */
/* ========================================================================= */

/* TCK delay per half-cycle.
 * 0 = max GPIO speed (~3MHz effective on ESP32-S3 with rom delay overhead).
 * GW2A-18 JTAG is rated to 25MHz so 0 is fine for signal integrity.
 * Keep at 0; the bottleneck is now network recv, not JTAG speed. */
#define TCK_DELAY_US 0

/**
 * Clock one bit through JTAG TAP using direct register access.
 *
 * REG_WRITE to GPIO_OUT_W1TS/W1TC is ~3x faster than gpio_set_level() for
 * the high-throughput streaming path.  State-machine transitions (IR scan,
 * state navigation) also use this path; they are few hundred bits total so
 * speed is irrelevant there, but correctness is paramount.
 *
 * Timing note: the ESP32-S3 GPIO matrix adds ~2 APB cycles (~25 ns) of output
 * latency between a peripheral write and the pin changing.  REG_WRITE itself
 * takes ~1 APB cycle (~12.5 ns) from the CPU's view.  If we issue W1TS(TDI)
 * then immediately W1TS(TCK), both signals enter the same pipeline and TDI
 * settles at the pad only ~8-12 ns before TCK rises — barely above the 3 ns
 * spec minimum and unreliable with trace capacitance.  The 8-NOP fence below
 * adds ~33 ns of CPU time, bringing TDI setup to a comfortable ~41 ns.
 *
 * Standard JTAG timing:
 * - TDI/TMS sampled by target on rising TCK edge
 * - TDO updated by target on falling TCK edge; sampled after falling edge
 */
static inline __attribute__((always_inline)) uint8_t jtag_clock_bit(uint8_t tms, uint8_t tdi)
{
    /* Build set/clear masks for TMS and TDI simultaneously */
    uint32_t set = (tms ? g_tms_mask : 0u) | (tdi ? g_tdi_mask : 0u);
    uint32_t clr = (tms ? 0u : g_tms_mask) | (tdi ? 0u : g_tdi_mask) | g_tck_mask;

    REG_WRITE(GPIO_OUT_W1TC_REG, clr);          /* clear TCK + any 0-value signals */
    if (set) REG_WRITE(GPIO_OUT_W1TS_REG, set); /* set any 1-value signals (skip if 0 — W1TS(0) is a peripheral no-op
                                                  * that provides no settling time against the GPIO output pipeline) */
    /* 8 NOPs @ 240 MHz ≈ 33 ns — TDI/TMS setup fence before TCK rises */
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop");
    REG_WRITE(GPIO_OUT_W1TS_REG, g_tck_mask);   /* TCK rising edge */
    REG_WRITE(GPIO_OUT_W1TC_REG, g_tck_mask);   /* TCK falling edge */

    return (REG_READ(GPIO_IN_REG) & g_tdo_mask) ? 1u : 0u;
}

/* ========================================================================= */
/* JTAG State Machine Navigation                                             */
/* ========================================================================= */

/**
 * Navigate to Test-Logic-Reset state
 */
static void jtag_goto_reset(void)
{
    /* 10+ TMS=1 cycles to ensure TAP fully resets */
    for (int i = 0; i < 10; i++) {
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
        
        /* Yield to IDLE task every 4096 bits so it can feed the Task WDT timer */
        if ((i & 0xFFF) == 0xFFF) {
            taskYIELD();
        }
    }
    
    /* Exit1-DR → Update-DR → Idle */
    jtag_clock_bit(1, 0);  // Exit1 → Update
    jtag_clock_bit(0, 0);  // Update → Idle
    
    g_state = RUN_TEST_IDLE;
}

/* ========================================================================= */
/* Gowin JTAG Instruction Codes (from openFPGALoader / Gowin TN653)         */
/* ========================================================================= */

/* These are Gowin's actual IR codes — NOT Xilinx (JPROGRAM/CFG_IN/JSTART do NOT apply) */
#define GOWIN_NOOP           0x02  /* No operation */
#define GOWIN_ERASE_SRAM     0x05  /* Erase SRAM configuration */
#define GOWIN_XFER_DONE      0x09  /* Transfer done */
#define GOWIN_READ_IDCODE    0x11  /* Read device IDCODE */
#define GOWIN_INIT_ADDR      0x12  /* Initialize address pointer */
#define GOWIN_CONFIG_ENABLE  0x15  /* Enable configuration mode */
#define GOWIN_XFER_WRITE     0x17  /* Transfer write — IR to load before bitstream DR scan */
#define GOWIN_CONFIG_DISABLE 0x3A  /* Disable configuration mode */
#define GOWIN_RELOAD         0x3C  /* Reload / restart FPGA from loaded config */
#define GOWIN_STATUS_REG     0x41  /* Read status register */

/* Status register bit fields */
#define STATUS_CRC_ERROR        (1 << 0)
#define STATUS_BAD_COMMAND      (1 << 1)
#define STATUS_MEMORY_ERASE     (1 << 5)   /* Goes high when SRAM erase completes */
#define STATUS_SYSTEM_EDIT_MODE (1 << 7)   /* Set while in config enable mode */
#define STATUS_DONE_FINAL       (1 << 13)  /* FPGA configured and running */

/* ========================================================================= */
/* Gowin Command Helpers                                                      */
/* ========================================================================= */

/**
 * Send Gowin command = IR scan + 6 RTI clock pulses.
 * openFPGALoader's send_command() does exactly this.
 */
static void gowin_send_command(uint8_t cmd)
{
    jtag_ir_scan(cmd);
    jtag_run_idle(6);
}

/**
 * Read a 32-bit Gowin register (IDCODE, STATUS, USERCODE, etc.)
 * Load cmd into IR, then shift 32 bits out of DR (TDI=all-ones per spec).
 */
static uint32_t gowin_read_reg32(uint8_t cmd)
{
    gowin_send_command(cmd);

    /* Navigate: RTI → Select-DR → Capture-DR.
     * Per JTAG spec the shift register is loaded in Capture-DR; the first bit
     * shifts out (TDO) on the Capture-DR→Shift-DR transition, which is also
     * the first clock of the loop (i=0).  No extra explicit clock needed. */
    jtag_clock_bit(1, 0);  /* RTI → Select-DR */
    jtag_clock_bit(0, 0);  /* Select-DR → Capture-DR */

    uint32_t reg = 0;
    for (int i = 0; i < 32; i++) {
        /* First iteration: Capture-DR → Shift-DR, TDO = bit 0 */
        uint8_t tdo = jtag_clock_bit((i == 31) ? 1 : 0, 1);  /* TDI=1, exit on last */
        if (tdo) reg |= (1UL << i);
    }

    /* Exit1-DR → Update-DR → RTI */
    jtag_clock_bit(1, 0);
    jtag_clock_bit(0, 0);

    return reg;
}

/**
 * Poll status register until (status & mask) == value.
 * Returns true if condition met within timeout_ms milliseconds.
 */
static bool gowin_poll_flag(uint32_t mask, uint32_t value, int timeout_ms)
{
    /* Use wall-clock timeout rather than iteration count.
     * Keep TCK running with jtag_run_idle between reads — TN653 requires
     * clock to be active during erase; stopping TCK stalls the operation. */
    int64_t start_us = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    uint32_t last_status = 0;

    while ((esp_timer_get_time() - start_us) < timeout_us) {
        last_status = gowin_read_reg32(GOWIN_STATUS_REG);
        if ((last_status & mask) == value) return true;
        jtag_run_idle(50);  /* Keep TCK clocking — do NOT stop between reads */
    }
    ESP_LOGE(TAG, "poll_flag timeout: mask=0x%08lX value=0x%08lX last_status=0x%08lX",
             mask, value, last_status);
    return false;
}

/**
 * Erase SRAM configuration (TN653 p.9-10).
 * Sequence: CONFIG_ENABLE → ERASE_SRAM → NOOP → [poll MEMORY_ERASE] → XFER_DONE → NOOP → CONFIG_DISABLE → NOOP
 */
static esp_err_t gowin_erase_sram(void)
{
    ESP_LOGI(TAG, "Erasing SRAM configuration");

    /* TN653 SRAM erase sequence (p.9-10):
     * CONFIG_ENABLE → ERASE_SRAM → NOOP → [poll MEMORY_ERASE] → XFER_DONE → NOOP → CONFIG_DISABLE → NOOP
     *
     * Do NOT poll STATUS_SYSTEM_EDIT_MODE after CONFIG_ENABLE: loading STATUS_REGISTER into IR
     * while the FPGA is processing CONFIG_ENABLE cancels config mode entry on GW2A.
     * Extra RTI clocks after CONFIG_ENABLE are sufficient. */
    gowin_send_command(GOWIN_CONFIG_ENABLE);
    jtag_run_idle(50);  /* Extra RTI clocks — let config enable take effect */

    gowin_send_command(GOWIN_ERASE_SRAM);
    gowin_send_command(GOWIN_NOOP);

    /* TN653: keep TCK running for >=4ms while SRAM erase completes.
     * 4000 RTI clocks × 2µs/cycle = 8ms at 333kHz — well above minimum. */
    jtag_run_idle(4000);

    /* Poll STATUS_MEMORY_ERASE (bit 5) high — erase done.
     * Gowin TN653: erase takes ~4ms, we allow 5000ms to be safe. */
    if (!gowin_poll_flag(STATUS_MEMORY_ERASE, STATUS_MEMORY_ERASE, 5000)) {
        ESP_LOGE(TAG, "SRAM erase timed out");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "SRAM erase complete");

    gowin_send_command(GOWIN_XFER_DONE);
    gowin_send_command(GOWIN_NOOP);
    gowin_send_command(GOWIN_CONFIG_DISABLE);
    gowin_send_command(GOWIN_NOOP);

    return ESP_OK;
}

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

    /* Precompute bitmasks for direct GPIO register access in jtag_clock_bit() */
    g_tck_mask = 1u << (uint32_t)g_pins.tck;
    g_tms_mask = 1u << (uint32_t)g_pins.tms;
    g_tdi_mask = 1u << (uint32_t)g_pins.tdi;
    g_tdo_mask = 1u << (uint32_t)g_pins.tdo;
    
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
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // Enable pull-up for TDO
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

/* Extract JEDEC manufacturer ID from JTAG IDCODE (bits 11:1) */
static uint16_t jtag_idcode_manufacturer(uint32_t idcode)
{
    return (idcode >> 1) & 0x7FF;
}

#define GOWIN_JEDEC_MFR 0x40D  /* Gowin 11-bit JTAG manufacturer field (JEP106 bank 9, ID 0x0D) */

esp_err_t jtag_gowin_read_idcode(uint32_t *idcode)
{
    if (!idcode) return ESP_ERR_INVALID_ARG;
    
    /* Reset TAP - IDCODE is auto-loaded into DR after reset per JTAG spec */
    jtag_goto_reset();
    
    /* Go to idle, then run 6 RTI clocks (required by some Gowin devices) */
    jtag_goto_idle();
    for (int i = 0; i < 6; i++) {
        jtag_clock_bit(0, 0);  /* Stay in Run-Test/Idle */
    }
    
    /* Navigate to Shift-DR to read IDCODE */
    jtag_clock_bit(1, 0);  /* Idle → Select-DR */
    jtag_clock_bit(0, 0);  /* Select-DR → Capture-DR */
    
    /* Read all 32 bits, LSB first */
    uint32_t id = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t tdo = jtag_clock_bit((i == 31) ? 1 : 0, 0);
        if (tdo) id |= (1UL << i);
    }
    
    /* Exit1-DR → Update-DR → Idle */
    jtag_clock_bit(1, 0);
    jtag_clock_bit(0, 0);
    
    *idcode = id;
    
    ESP_LOGI(TAG, "IDCODE: 0x%08lX (%s)", id, jtag_gowin_device_name(id));
    
    /* Validate by checking manufacturer field (bits 11:1) */
    if (jtag_idcode_manufacturer(id) != GOWIN_JEDEC_MFR) {
        ESP_LOGW(TAG, "Unexpected manufacturer in IDCODE (bits 11:1 = 0x%03X, expected 0x%03X)",
                 jtag_idcode_manufacturer(id), GOWIN_JEDEC_MFR);
    }
    
    return ESP_OK;
}

esp_err_t jtag_gowin_program_sram(const uint8_t *bitstream, size_t length)
{
    if (!bitstream || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Delegate to streaming API */
    uint32_t idcode;
    esp_err_t err = jtag_gowin_program_sram_begin(&idcode);
    if (err != ESP_OK) return err;
    err = jtag_gowin_program_sram_write(bitstream, length, true);  /* network stream: yield for TCP */
    if (err != ESP_OK) return err;
    return jtag_gowin_program_sram_end();
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

/* ========================================================================= */
/* Streaming API Implementation                                              */
/* ========================================================================= */

/* True while TAP is sitting in Shift-DR during streaming */
static bool g_streaming_dr = false;

/**
 * Begin SRAM programming (streaming mode).
 *
 * Correct sequence per openFPGALoader / Gowin TN653:
 *   1. Reset TAP, read/validate IDCODE
 *   2. eraseSRAM(): CONFIG_ENABLE → ERASE_SRAM → NOOP → [poll] → XFER_DONE → NOOP → CONFIG_DISABLE → NOOP
 *   3. CONFIG_ENABLE → INIT_ADDR → XFER_WRITE
 *   4. Enter Shift-DR and HOLD — all subsequent write() calls stay in Shift-DR
 */
esp_err_t jtag_gowin_program_sram_begin(uint32_t *idcode_out)
{
    ESP_LOGI(TAG, "Starting SRAM programming (streaming mode)");
    g_streaming_dr = false;

    /* 1. Reset TAP and verify device */
    jtag_goto_idle();

    uint32_t idcode;
    jtag_gowin_read_idcode(&idcode);

    if (jtag_idcode_manufacturer(idcode) != GOWIN_JEDEC_MFR) {
        ESP_LOGE(TAG, "Not a Gowin device! IDCODE=0x%08lX (manufacturer=0x%03X)",
                 idcode, jtag_idcode_manufacturer(idcode));
        return ESP_ERR_NOT_FOUND;
    }

    if (idcode_out) *idcode_out = idcode;

    /* 2. Erase SRAM with correct Gowin sequence */
    esp_err_t err = gowin_erase_sram();
    if (err != ESP_OK) return err;

    /* 3. Load bitstream: CONFIG_ENABLE → [extra RTI] → INIT_ADDR → XFER_WRITE
     *    openFPGALoader writeSRAM() sends these three back-to-back without status polling.
     *    Extra RTI clocks replace SYSTEM_EDIT_MODE polling (see erase comment above). */
    gowin_send_command(GOWIN_CONFIG_ENABLE);
    jtag_run_idle(50);
    gowin_send_command(GOWIN_INIT_ADDR);
    gowin_send_command(GOWIN_XFER_WRITE);  /* Gowin "transfer write" IR — NOT CFG_IN */

    /* 4. Enter Shift-DR and hold for entire bitstream.
     *    write() clocks all bits with TMS=0.
     *    end() exits and finalises. */
    jtag_clock_bit(1, 0);  /* RTI → Select-DR */
    jtag_clock_bit(0, 0);  /* Select-DR → Capture-DR */
    /* First write() clock will be Capture-DR→Shift-DR per JTAG spec;
     * bit 7 of the first byte is clocked out during that transition. */
    g_streaming_dr = true;

    ESP_LOGI(TAG, "In Shift-DR, ready to receive bitstream");
    return ESP_OK;
}

esp_err_t jtag_gowin_program_sram_write(const uint8_t *data, size_t length, bool yield_for_tcp)
{
    if (!data || length == 0) return ESP_ERR_INVALID_ARG;
    if (!g_streaming_dr) {
        ESP_LOGE(TAG, "write() called but not in Shift-DR state");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure TMS=0 and TCK=0 before streaming.  TMS stays low the entire
     * transfer; we never touch it inside the bit loop, saving two register
     * writes per bit compared to the previous clr_base approach. */
    REG_WRITE(GPIO_OUT_W1TC_REG, g_tck_mask | g_tms_mask);

    for (size_t b = 0; b < length; b++) {
        uint8_t byte = data[b];
        for (int bit = 7; bit >= 0; bit--) {
            /* Set TDI while TCK is low, then fence with 8 NOPs (~33 ns at
             * 240 MHz) to ensure the pin has settled through the GPIO matrix
             * output pipeline before TCK rises.  The old "W1TC confirm 0"
             * trick was a peripheral no-op that provided no real settling time
             * because W1TC(0) / W1TS(0) writes are optimised away by the
             * peripheral and do not stall the output pipeline.  NOPs run on
             * the CPU itself and guarantee ordered time between TDI change
             * and TCK rise at ~41 ns — well above the 3 ns GW2A spec. */
            if ((byte >> bit) & 1u) {
                REG_WRITE(GPIO_OUT_W1TS_REG, g_tdi_mask);  /* TDI=1 */
            } else {
                REG_WRITE(GPIO_OUT_W1TC_REG, g_tdi_mask);  /* TDI=0 */
            }
            __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop");
            REG_WRITE(GPIO_OUT_W1TS_REG, g_tck_mask);      /* TCK rising edge */
            REG_WRITE(GPIO_OUT_W1TC_REG, g_tck_mask);      /* TCK falling edge */
        }

        /* Yield periodically so other tasks can run.
         * yield_for_tcp=true: vTaskDelay(1) forces a proper context switch so
         *   LwIP (lower priority) can ACK TCP segments and keep curl sending.
         * yield_for_tcp=false: taskYIELD() is enough when data is local (no TCP). */
        if ((b & 0x1FFF) == 0x1FFF) {
            if (yield_for_tcp) vTaskDelay(1);
            else taskYIELD();
        }
    }

    return ESP_OK;
}

/**
 * End SRAM programming.
 *
 * Correct sequence per Gowin TN653 / openFPGALoader writeSRAM():
 *   Exit Shift-DR → XFER_DONE → NOOP → CONFIG_DISABLE → NOOP → check STATUS_DONE_FINAL
 *
 * XFER_DONE is mandatory — it signals to the FPGA that the bitstream transfer is
 * complete and triggers the internal configuration commit.  Without it the FPGA
 * stays in the post-erase state and DONE_FINAL is never set.
 */
esp_err_t jtag_gowin_program_sram_end(void)
{
    if (g_streaming_dr) {
        /* Exit Shift-DR → Exit1-DR → Update-DR → RTI */
        jtag_clock_bit(1, 0);
        jtag_clock_bit(1, 0);
        jtag_clock_bit(0, 0);
        g_streaming_dr = false;
    }

    /* The Gowin .fs/.bin bitstream embeds its own end-of-configuration command
     * in its trailer. We just need to keep TCK running so the FPGA can process
     * those internal commands and assert DONE_FINAL. */
    jtag_run_idle(125);

    /* Poll STATUS for DONE_FINAL with a 1 s timeout. Keep TCK clocking between
     * reads so the FPGA continues processing. */
    uint32_t status = 0;
    int64_t deadline_us = esp_timer_get_time() + 1000000;
    bool done = false;
    while (esp_timer_get_time() < deadline_us) {
        status = gowin_read_reg32(GOWIN_STATUS_REG);
        if (status & STATUS_DONE_FINAL) { done = true; break; }
        if (status & STATUS_CRC_ERROR) break;  /* fatal — stop early */
        jtag_run_idle(125);
    }

    /* Exit configuration mode and return TAP to reset */
    gowin_send_command(GOWIN_CONFIG_DISABLE);
    gowin_send_command(GOWIN_NOOP);
    jtag_run_idle(125);
    jtag_goto_reset();

    if (done) {
        ESP_LOGI(TAG, "SRAM programming complete — FPGA running (STATUS=0x%08lX)", status);
    } else if (status & STATUS_CRC_ERROR) {
        ESP_LOGE(TAG, "SRAM programming failed — CRC error (STATUS=0x%08lX)", status);
        return ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "SRAM programming done but DONE_FINAL not set (STATUS=0x%08lX) — check bitstream", status);
    }

    return ESP_OK;
}

