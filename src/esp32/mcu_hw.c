/*
  mcu_hw.c - MiSTeryNano FPGA companion hardware driver for esp32 s2/s3
*/

#include <stdio.h>
#include <inttypes.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <freertos/queue.h>

#include "../hidparser.h"
#include "../debug.h"
#include "../mcu_hw.h"
#include "../hid.h"
#include "../config.h"

#include "../sysctrl.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "wifi_log.h"
#include "bt_hid.h"

//#define USB_ERROR_CHECK(a)  ESP_ERROR_CHECK(a)
#define USB_ERROR_CHECK(a) (a)

/* ========================================================================= */
/* =========                          USB                        =========== */
/* ========================================================================= */

#include "usb/usb_host.h"
#include "usb/hid_host.h"

static struct {
  hid_host_device_handle_t handle;
  hid_state_t state;
  hid_report_t rep;
} hid_device[MAX_HID_DEVICES];

QueueHandle_t hid_host_event_queue;

typedef struct {
    hid_host_device_handle_t hid_device_handle;
    hid_host_driver_event_t event;
    void *arg;
} hid_host_event_queue_t;

void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event, void *arg) {
    uint8_t data[64];
    size_t data_length = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        USB_ERROR_CHECK( hid_host_device_get_raw_input_report_data(hid_device_handle,
					   data, sizeof(data), &data_length));

	for(int idx=0;idx<MAX_HID_DEVICES;idx++)
	  if(hid_device[idx].handle == hid_device_handle) {
	    hid_parse(&hid_device[idx].rep, &hid_device[idx].state, data, data_length);
	  }

        break;
	
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        usb_debugf("HID Device DISCONNECTED");
        USB_ERROR_CHECK( hid_host_device_close(hid_device_handle) );

	// find and remove entry
	for(int idx=0;idx<MAX_HID_DEVICES;idx++) {
	  if(hid_device[idx].handle == hid_device_handle) {
	    usb_debugf("releasing %d", idx);
	    hid_device[idx].handle = NULL;
	    if(hid_device[idx].rep.type == REPORT_TYPE_JOYSTICK)
	      hid_release_joystick(hid_device[idx].state.joystick.js_index);
	  }
	}
	break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        usb_debugf("HID Device: TRANSFER_ERROR");
        break;
    default:
        usb_debugf("HID Device: Unhandled event");
        break;
    }
}

void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                           const hid_host_driver_event_t event,
                           void *arg) {
    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        usb_debugf("HID Device: CONNECTED");

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL
        };

        USB_ERROR_CHECK( hid_host_device_open(hid_device_handle, &dev_config) );
	// the following fails on the Rii R8
	// USB_ERROR_CHECK( hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
	//	if (HID_PROTOCOL_KEYBOARD == dev_params.proto)
	//  USB_ERROR_CHECK( hid_class_request_set_idle(hid_device_handle, 0, 0));

	// request report descriptor
	size_t report_desc_len;
	uint8_t *report_desc = hid_host_get_report_descriptor(hid_device_handle, &report_desc_len);
	if(report_desc) {	
	  int idx;
	  for(idx=0;idx<MAX_HID_DEVICES && (hid_device[idx].handle != NULL);idx++);
	  if(idx != MAX_HID_DEVICES) {
	    usb_debugf("Using HID entry %d", idx);
	    
	    if(parse_report_descriptor(report_desc, report_desc_len, &hid_device[idx].rep, NULL)) {
	      usb_debugf("HID[%d] type=%d id_present=%d id=%d size=%d", idx,
		 hid_device[idx].rep.type,
		 hid_device[idx].rep.report_id_present,
		 hid_device[idx].rep.report_id,
		 hid_device[idx].rep.report_size);
	      hid_device[idx].handle = hid_device_handle;
	      if(hid_device[idx].rep.type == REPORT_TYPE_JOYSTICK)
		hid_device[idx].state.joystick.js_index = hid_allocate_joystick();
	      
	      esp_err_t start_err = hid_host_device_start(hid_device_handle);
	      usb_debugf("hid_host_device_start: %d", start_err);

	      // Ask keyboards to use boot protocol so they send standard 8-byte reports
	      // instead of NKRO reports with a report-ID prefix.  The request may return
	      // an error for non-boot-interface keyboards (e.g. Rii R8) – that is fine;
	      // USB_ERROR_CHECK is a no-op so execution continues either way.
	      if(hid_device[idx].rep.type == REPORT_TYPE_KEYBOARD) {
	        esp_err_t proto_err = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
	        usb_debugf("Set boot protocol: %d (ok if 0)", proto_err);
	      }

	      // DualShock 3 / Sixaxis requires a feature report to start streaming HID data.
	      // Without this the device connects but never sends INPUT reports.
	      // Only send this to joystick/gamepad devices — sending it to keyboards causes
	      // them to accept the feature report (ESP_OK) and potentially reset/misbehave.
	      if(hid_device[idx].rep.type == REPORT_TYPE_JOYSTICK) {
	        uint8_t ds3_activate[] = { 0x42, 0x0c, 0x00, 0x00 };
	        esp_err_t ds3_err = hid_class_request_set_report(hid_device_handle,
	                              HID_REPORT_TYPE_FEATURE, 0xf4,
	                              ds3_activate, sizeof(ds3_activate));
	        usb_debugf("DS3 activate: %d (ok if 0 or ESP_ERR_NOT_SUPPORTED)", ds3_err);
	      }
	    } else {
	      usb_debugf("ignoring device - starting anyway to keep driver state clean");
	      // Don't close an opened-but-never-started device: that leaves the HID
	      // host pipe in an incomplete state that can stall hub enumeration for
	      // subsequent devices.  Start it instead so the driver lifecycle is
	      // correct; data from this interface is discarded in the callback
	      // because the handle is never stored in hid_device[].
	      hid_host_device_start(hid_device_handle);
	    }
	  } else {
	    usb_debugf("Error, no more free HID entries - starting untracked");
	    hid_host_device_start(hid_device_handle);
	  }
	}
        break;
    default:
        break;
    }
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg) {
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    USB_ERROR_CHECK( usb_host_install(&host_config) );
    xTaskNotifyGive(arg);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            usb_debugf("USB Event flags: NO_CLIENTS");
        }
        // All devices were removed
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            usb_debugf("USB Event flags: ALL_FREE");
        }
    }
}

#if 0
/* ============================ XBOX ====================================== */

static usb_host_client_handle_t xbox_client_handle;

typedef enum { XBOX_UNKNOWN, XBOX360_WIRELESS, XBOX360_WIRED, XBOXONE, XBOXOG } xbox_type_t;

static bool xbox_host_device_init_attempt(uint8_t dev_addr) {
  usb_device_handle_t dev_hdl;

  if (usb_host_device_open(xbox_client_handle, dev_addr, &dev_hdl) == ESP_OK) {
    const usb_device_desc_t *device_desc = NULL;
    if(usb_host_get_device_descriptor(dev_hdl, &device_desc) == ESP_OK) {
      usb_debugf("check for xbox device [%04x:%04x]", device_desc->idVendor,  device_desc->idProduct);

      const usb_config_desc_t *config_desc = NULL;
      if (usb_host_get_active_config_descriptor(dev_hdl, &config_desc) == ESP_OK) {
	usb_debugf("Interfaces: %d", config_desc->bNumInterfaces);
	
	// walk over all interfaces
	for(int intf=0;intf<config_desc->bNumInterfaces;intf++) {
	  xbox_type_t type = XBOX_UNKNOWN;
	  usb_debugf("Intf #%d", intf);
	  
	  const usb_intf_desc_t *intf_desc = usb_parse_interface_descriptor(config_desc, intf, 0, NULL);
	  if(intf_desc && intf_desc->bNumEndpoints >= 2) {	  
	    if (intf_desc->bInterfaceSubClass == 0x5D &&        //Xbox360 wireless bInterfaceSubClass
		intf_desc->bInterfaceProtocol == 0x81) {        //Xbox360 wireless bInterfaceProtocol
	      usb_debugf("%d: XBOX360_WIRELESS", intf);
	      type = XBOX360_WIRELESS;
	    } else if (intf_desc->bInterfaceSubClass == 0x5D && //Xbox360 wired bInterfaceSubClass
		     intf_desc->bInterfaceProtocol == 0x01) {   //Xbox360 wired bInterfaceProtocol
	      usb_debugf("%d: XBOX360_WIRED", intf);
	      type = XBOX360_WIRED;
	    } else if (intf_desc->bInterfaceSubClass == 0x47 && //Xbone and SX bInterfaceSubClass
		     intf_desc->bInterfaceProtocol == 0xD0) {   //Xbone and SX bInterfaceProtocol
	      usb_debugf("%d: XBOXONE", intf);
	      type = XBOXONE;
	    } else if (intf_desc->bInterfaceClass == 0x58 &&    //XboxOG bInterfaceClass
		     intf_desc->bInterfaceSubClass == 0x42) {   //XboxOG bInterfaceSubClass
	      usb_debugf("%d: XBOXOG", intf);
	      type = XBOXOG;
	    }
	  }

	  // found xbox controller -> try to use it
	  if(type != XBOX_UNKNOWN) {
	    for(int ep = 0; ep < intf_desc->bNumEndpoints; ep++) {
	      int ep_offset = 0;
	      const usb_ep_desc_t *ep_desc =
		usb_parse_endpoint_descriptor_by_index(intf_desc, ep, config_desc->wTotalLength, &ep_offset);
	      usb_debugf("EP%d: len=%d type=%d attr=%d, maxpkt=%d, interval=%d",
			 ep, ep_desc->bLength, ep_desc->bDescriptorType, ep_desc->bEndpointAddress,
			 ep_desc->wMaxPacketSize, ep_desc->bInterval);
	      if(ep_desc && USB_EP_DESC_GET_EP_DIR(ep_desc)) usb_debugf("in ep");
	    }
	  }	  
	}
      }
    }
  }
    
  return false;
}

static esp_err_t xbox_host_device_disconnected(usb_device_handle_t dev_hdl) {
  // free everything else ...  
  usb_host_device_close(xbox_client_handle, dev_hdl);
  return ESP_OK;
}
    
static void xbox_client_event_cb(const usb_host_client_event_msg_t *event, void *arg) {
  usb_debugf("XBOX EVENT %d", event->event);
  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    xbox_host_device_init_attempt(event->new_dev.address);
  } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    xbox_host_device_disconnected(event->dev_gone.dev_hdl);
  }
}


 static void xbox_event_handler_task(void *arg) {
  while (1) {
    /* Here wee need a timeout 50 ms to handle end_client_event_handling flag
     * during situation when all devices were removed and it is time to remove
     * and destroy everything.
     */
    usb_host_client_handle_events(xbox_client_handle, portMAX_DELAY);
  }
}
#endif

/**
 * @brief HID Host main task
 *
 * Creates queue and get new event from the queue
 *
 * @param[in] pvParameters Not used
 */
void hid_host_task(void *pvParameters) {
  hid_host_event_queue_t evt_queue;
  // Create queue
  hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));
  
  // Wait queue
  while (1) {
    if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50))) {
      hid_host_device_event(evt_queue.hid_device_handle,
			    evt_queue.event,
			    evt_queue.arg);
    }
  }
}

void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                              const hid_host_driver_event_t event, void *arg) {
  const hid_host_event_queue_t evt_queue = {
    .hid_device_handle = hid_device_handle,
    .event = event,
    .arg = arg
  };
  xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

static void usb_init(void) {
    BaseType_t task_created;
 
    usb_debugf("Initializing");    
    debugf("USB D+/D- on GPIO20 and GPIO19");

  // mark all entries as unused
    for(int idx=0;idx<MAX_HID_DEVICES;idx++)
      hid_device[idx].handle = NULL;

/*
    * Create usb_lib_task to:
    * - initialize USB Host library
    * - Handle USB Host events while APP pin in in HIGH state
    */
    task_created = xTaskCreatePinnedToCore(usb_lib_task,
                                           "usb_events",
                                           6144,
                                           xTaskGetCurrentTaskHandle(),
                                           2, NULL, 0);
    assert(task_created == pdTRUE);

    // Wait for notification from usb_lib_task to proceed
    ulTaskNotifyTake(false, 1000);

    /*
    * HID host driver configuration
    * - create background task for handling low level event inside the HID driver
    * - provide the device callback to get new HID Device connection event
    */
    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 8192,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL
    };

    USB_ERROR_CHECK( hid_host_install(&hid_host_driver_config) );

   /*
    * Create HID Host task process for handle events
    * IMPORTANT: Task is necessary here while there is no possibility to interact
    * with USB device from the callback.
    */
    task_created = xTaskCreate(&hid_host_task, "hid_task", 8 * 1024, NULL, 2, NULL);
    assert(task_created == pdTRUE);

#if 0
    /* =========================== xbox ================================== */    
    usb_host_client_config_t xbox_client_config = {
      .is_synchronous = false,
      .async.client_event_callback = xbox_client_event_cb,
      .async.callback_arg = NULL,
      .max_num_event_msg = 10,
    };
    
    // register xbox driver
    ESP_ERROR_CHECK( usb_host_client_register(&xbox_client_config, &xbox_client_handle));
    xTaskCreatePinnedToCore(xbox_event_handler_task, "USB XBOX Host", 2048, NULL, 2, NULL, 0);
#endif
}

/* ========================================================================= */
/* ========                          SPI                            ======== */
/* ========================================================================= */

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"

#define SPI_HOST_ID SPI3_HOST
#define PIN_NUM_MISO 3       // Papilio Arcade
// #define PIN_NUM_MISO 13        // Dock
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10
#define PIN_NUM_IRQ  9
#define PIN_NUM_RECONFIG_N   13    // Papilio Arcade - To reconfigure the FPGA after a new bitstream has been written to the flash 
// #define PIN_NUM_RECONFIG_N   8    // Dock - To reconfigure the FPGA after a new bitstream has been written to the flash 
// #define PIN_NUM_IRQ  14
#define PIN_NUM_FLASH_CS   4   // Papilio Arcade - To select the flash chip
// #define PIN_NUM_FLASH_CS   7   // Dock - To select the flash chip

esp_flash_t* ext_flash;
static bool ext_flash_ready = false;

/* Config used when creating/recreating the flash device handle. */
static const esp_flash_spi_device_config_t flash_device_cfg = {
    .host_id   = SPI_HOST_ID,
    .cs_id     = 0,
    .cs_io_num = PIN_NUM_FLASH_CS,
    .io_mode   = SPI_FLASH_SLOWRD,  /* SLOWRD (0x03) avoids dummy-byte issues through FPGA bridge */
    .freq_mhz  = 20
};

extern TaskHandle_t com_task_handle;
static spi_device_handle_t spi;
static SemaphoreHandle_t sem;

static void irq_handler(void *) {
  // debugf("IRQ");
  
  // Disable interrupt. It will be re-enabled by the com task
  gpio_intr_disable(PIN_NUM_IRQ);

  if(com_task_handle) {    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR( com_task_handle, &xHigherPriorityTaskWoken );
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
  }
}
  
void mcu_hw_spi_init(void) {
  debugf("Initializing SPI");

  sem = xSemaphoreCreateMutex();

  debugf("  MISO = GPIO%d", PIN_NUM_MISO);
  debugf("  SCK  = GPIO%d", PIN_NUM_CLK);
  debugf("  MOSI = GPIO%d", PIN_NUM_MOSI);
  
  spi_bus_config_t buscfg = {
     .miso_io_num = PIN_NUM_MISO,
     .mosi_io_num = PIN_NUM_MOSI,
     .sclk_io_num = PIN_NUM_CLK,
     .quadwp_io_num = -1,
     .quadhd_io_num = -1,
     .intr_flags = ESP_INTR_FLAG_LOWMED,      // Needed to fix "No free interrupt inputs for USB interrupt (flags 0x802)" errors
     .max_transfer_sz = 32
  };
  
  spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);

  spi_device_interface_config_t devcfg = {
     .clock_speed_hz = 10 * 1000 * 1000,      // 10 MHz (reduced for SPI timing diagnostics)
     .mode = 1,                               // SPI mode 1
     .spics_io_num = -1,
     .command_bits = 0,                       // no command, address or dummy bits since we
     .address_bits = 0,                       // are tranferring single bytes
     .dummy_bits = 0,     
     .queue_size = 7,                         // We want to be able to queue 7 transactions at a time
  };
  
  spi_bus_add_device(SPI_HOST_ID, &devcfg, &spi);

  // Chip select is active-low, so we'll initialise it to a driven-high state
  debugf("  CSn  = GPIO%d", PIN_NUM_CS);
  gpio_set_direction(PIN_NUM_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_NUM_CS, 1);
 
  // The interruput input isn't strictly part of the SPi
  // The interrupt is active low
  debugf("  IRQn = GPIO%d", PIN_NUM_IRQ);
  gpio_set_pull_mode(PIN_NUM_IRQ, GPIO_PULLUP_ONLY);
  gpio_set_direction(PIN_NUM_IRQ, GPIO_MODE_INPUT);  
  gpio_install_isr_service(ESP_INTR_FLAG_LEVEL2);
  gpio_isr_handler_add(PIN_NUM_IRQ, irq_handler, NULL);
  gpio_set_intr_type(PIN_NUM_IRQ, GPIO_INTR_LOW_LEVEL);

  /* Register the external SPI flash device on the bus.
   * We do NOT call esp_flash_init() here — the FPGA SPI bridge is not yet
   * active at boot time (no valid bitstream in flash until after first OTA).
   * Actual probing is deferred to mcu_hw_reinit_flash(), called by the OTA
   * handler after the SRAM bootloader has brought the SPI bridge up. */
  ESP_ERROR_CHECK(spi_bus_add_flash_device(&ext_flash, &flash_device_cfg));
  debugf("External flash device registered (init deferred until FPGA SPI bridge ready)");

  sys_wait4fpga();
}

/* Release SPI3 bus so tangcore_task can reclaim it for direct SD card access.
 * Call only after com_task has entered its idle loop (active_interface == 2).
 * After this returns, spi_bus_initialize(SPI3_HOST, ...) may be called again. */
void mcu_hw_spi_deinit(void) {
  debugf("mcu_hw_spi_deinit: releasing SPI3 for tangcore SD passthrough");
  /* Remove external flash device first (it lives on the same bus) */
  if(ext_flash) {
    spi_bus_remove_flash_device(ext_flash);
    ext_flash = NULL;
    ext_flash_ready = false;
  }
  /* Remove the FPGA-bridge SPI device */
  spi_bus_remove_device(spi);
  /* Free the SPI bus */
  spi_bus_free(SPI_HOST_ID);
}

esp_err_t mcu_hw_reinit_flash(void) {
  /* Probe and initialise the external flash chip.  The FPGA SRAM bootloader
   * must be running before calling this so that its SPI bridge is active.
   * Each attempt fully removes and re-adds the flash device to get a clean
   * handle — field-clearing alone doesn't reset host-side SPI state. */
  const int MAX_RETRIES = 5;
  const int RETRY_DELAY_MS[] = { 500, 1000, 1500, 2000, 2000 };

  ext_flash_ready = false;

  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    /* Remove the existing device handle to discard all internal state. */
    spi_bus_remove_flash_device(ext_flash);
    ext_flash = NULL;

    /* Re-add to get a fresh handle. */
    esp_err_t add_err = spi_bus_add_flash_device(&ext_flash, &flash_device_cfg);
    if (add_err != ESP_OK) {
      debugf("Flash device re-add attempt %d/%d failed: %s",
             attempt + 1, MAX_RETRIES, esp_err_to_name(add_err));
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS[attempt]));
      continue;
    }

    esp_err_t err = esp_flash_init(ext_flash);
    if (err == ESP_OK) {
      uint32_t id = 0, flash_size = 0;
      esp_flash_read_id(ext_flash, &id);
      esp_flash_get_size(ext_flash, &flash_size);
      debugf("External flash ready: size=%" PRIu32 " KB, ID=0x%" PRIx32, flash_size / 1024, id);
      ext_flash_ready = true;
      return ESP_OK;
    }

    debugf("Flash init attempt %d/%d failed: %s — retrying in %d ms",
           attempt + 1, MAX_RETRIES, esp_err_to_name(err), RETRY_DELAY_MS[attempt]);
    vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS[attempt]));
  }

  /* Ensure we always have a valid (though uninitialised) handle for safety. */
  if (!ext_flash) spi_bus_add_flash_device(&ext_flash, &flash_device_cfg);
  debugf("External flash init failed after %d attempts", MAX_RETRIES);
  return ESP_ERR_FLASH_UNSUPPORTED_CHIP;
}

/* Ensure flash is initialized before use.  The FPGA SPI bridge must be active
 * (i.e. a valid bitstream is running) for this to succeed.  On normal boots the
 * FPGA loads from flash before the system reaches this point, so a single quick
 * attempt is sufficient.  The OTA path uses mcu_hw_reinit_flash() instead, which
 * has full retry logic for the blank-flash first-boot case. */
static esp_err_t ensure_flash_ready(void) {
  if (ext_flash_ready) return ESP_OK;

  /* Remove and re-add for a clean handle, then probe. */
  spi_bus_remove_flash_device(ext_flash);
  ext_flash = NULL;

  esp_err_t add_err = spi_bus_add_flash_device(&ext_flash, &flash_device_cfg);
  if (add_err != ESP_OK) {
    debugf("Flash lazy-init: device re-add failed: %s", esp_err_to_name(add_err));
    if (!ext_flash) spi_bus_add_flash_device(&ext_flash, &flash_device_cfg);
    return add_err;
  }

  esp_err_t err = esp_flash_init(ext_flash);
  if (err == ESP_OK) {
    uint32_t id = 0, flash_size = 0;
    esp_flash_read_id(ext_flash, &id);
    esp_flash_get_size(ext_flash, &flash_size);
    debugf("External flash lazy-init: size=%" PRIu32 " KB, ID=0x%" PRIx32, flash_size / 1024, id);
    ext_flash_ready = true;
  } else {
    debugf("Flash lazy-init failed: %s", esp_err_to_name(err));
  }
  return err;
}

void mcu_hw_erase_flash_region(uint32_t addr, uint32_t size) {
  if (ensure_flash_ready() != ESP_OK) return;
  esp_flash_erase_region(ext_flash, addr, size);
}

void mcu_hw_write_flash(uint32_t addr, uint8_t *data, uint32_t size) {
  if (ensure_flash_ready() != ESP_OK) return;
  esp_flash_write(ext_flash, data, addr, size);
}

void mcu_hw_read_flash(uint32_t addr, uint8_t *data, uint32_t size) {
  if (ensure_flash_ready() != ESP_OK) return;
  esp_flash_read(ext_flash, data, addr, size);
}

void mcu_hw_irq_ack(void) {
  // re-enable the interrupt since it was now serviced outside the irq handler
  gpio_intr_enable(PIN_NUM_IRQ);
}

/* ========================================================================= */
/* =========           UART1 — TangCore / BL616 protocol         ========== */
/* ========================================================================= */

#include "driver/uart.h"
#include "driver/gpio.h"

#define UART_TANGCORE    UART_NUM_1
#define PIN_NUM_UART_TX  43    // ESP32 TX → FPGA pin E14 (uart_rx)
#define PIN_NUM_UART_RX  44    // FPGA pin C9 (uart_tx) → ESP32 RX
#define UART_BAUD_RATE   2000000

void mcu_hw_uart_init(void) {
  const uart_config_t cfg = {
    .baud_rate  = UART_BAUD_RATE,
    .data_bits  = UART_DATA_8_BITS,
    .parity     = UART_PARITY_DISABLE,
    .stop_bits  = UART_STOP_BITS_1,
    .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_driver_install(UART_TANGCORE, 2048, 2048, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(UART_TANGCORE, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(UART_TANGCORE,
                               PIN_NUM_UART_TX, PIN_NUM_UART_RX,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  debugf("UART1 init: TX=GPIO%d RX=GPIO%d @ %d baud",
         PIN_NUM_UART_TX, PIN_NUM_UART_RX, UART_BAUD_RATE);
}

void mcu_hw_uart_tx_byte(uint8_t b) {
  uart_write_bytes(UART_TANGCORE, &b, 1);
}

void mcu_hw_uart_tx_buf(const uint8_t *buf, size_t len) {
  uart_write_bytes(UART_TANGCORE, buf, (int)len);
}

void mcu_hw_uart_tx_flush(void) {
  uart_wait_tx_done(UART_TANGCORE, pdMS_TO_TICKS(3000));
}

int mcu_hw_uart_rx_available(void) {
  size_t len = 0;
  uart_get_buffered_data_len(UART_TANGCORE, &len);
  return (int)len;
}

uint8_t mcu_hw_uart_rx_byte(void) {
  uint8_t b = 0;
  uart_read_bytes(UART_TANGCORE, &b, 1, portMAX_DELAY);
  return b;
}

void mcu_hw_spi_begin() {
  xSemaphoreTake(sem, 0xffffffffUL);      // wait forever
  gpio_set_level(PIN_NUM_CS, 0);  
}

void mcu_hw_spi_end() {
  gpio_set_level(PIN_NUM_CS, 1);
  xSemaphoreGive(sem);
}

void mcu_hw_spi_flash_begin() {
  xSemaphoreTake(sem, 0xffffffffUL);      // wait forever
}

void mcu_hw_spi_flash_end() {
  xSemaphoreGive(sem);
}

unsigned char mcu_hw_spi_tx_u08(unsigned char b) {
  unsigned char retval = 0;

  spi_transaction_t trans = {
    .cmd = 0,
    .addr = 0,
    .length = 8,
    .flags = SPI_TRANS_USE_TXDATA,
    .tx_data = { [0] = b },
    .rx_buffer = &retval
  };

  if(spi_device_polling_transmit(spi, &trans) != ESP_OK)
    debugf("SPI failed");
  
  // debugf("SPI(%d)", b);
  return retval;
}

void mcu_hw_fpga_reset(void) {
  debugf("FPGA RESET");
  gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_NUM_RECONFIG_N, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(PIN_NUM_RECONFIG_N, 1);
  gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_INPUT);
}

/* Brief pulse used to race JTAG into config mode before the FPGA's auto-load
 * from SPI flash completes.  No post-pulse delay — the caller is expected to
 * immediately drive JTAG.  ~5 ms low is enough for the GW2A config FSM to
 * abort an in-progress flash load and tri-state user I/O cells; after the
 * pin returns high, JTAG CONFIG_ENABLE will be latched before the next flash
 * auto-load attempt begins. */
void mcu_hw_fpga_reset_brief(void) {
  gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_NUM_RECONFIG_N, 0);
  esp_rom_delay_us(5000);
  gpio_set_level(PIN_NUM_RECONFIG_N, 1);
  gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_INPUT);
}

void mcu_hw_reset(void) {
  debugf("MCU RESET");
  esp_restart();
  for(;;);
}

void mcu_hw_init(void) {
  /* Capture reset reason early so we can diagnose unexpected reboots */
  esp_reset_reason_t reset_reason = esp_reset_reason();

  // gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_OUTPUT);
  // gpio_set_pull_mode(PIN_NUM_RECONFIG_N, GPIO_PULLUP_ONLY);
  // gpio_set_level(PIN_NUM_RECONFIG_N, 1);
  wifi_log_early_init();

  const char *rr_str = "?";
  switch (reset_reason) {
    case ESP_RST_POWERON:    rr_str = "POWERON"; break;
    case ESP_RST_EXT:        rr_str = "EXT"; break;
    case ESP_RST_SW:         rr_str = "SW (esp_restart)"; break;
    case ESP_RST_PANIC:      rr_str = "PANIC"; break;
    case ESP_RST_INT_WDT:    rr_str = "INT_WDT"; break;
    case ESP_RST_TASK_WDT:   rr_str = "TASK_WDT"; break;
    case ESP_RST_WDT:        rr_str = "WDT (other)"; break;
    case ESP_RST_DEEPSLEEP:  rr_str = "DEEPSLEEP"; break;
    case ESP_RST_BROWNOUT:   rr_str = "BROWNOUT"; break;
    case ESP_RST_SDIO:       rr_str = "SDIO"; break;
    case ESP_RST_USB:        rr_str = "USB"; break;
    case ESP_RST_JTAG:       rr_str = "JTAG"; break;
    default: break;
  }
  ESP_LOGW("BOOT", "reset_reason=%d (%s)", (int)reset_reason, rr_str);
  printf("\r\n[BOOT] reset_reason=%d (%s)\r\n", (int)reset_reason, rr_str);
  vTaskDelay(pdMS_TO_TICKS(5000));

  printf("\r\n\r\n" LOGO "           FPGA Companion for ESP32-S2/S3\r\n\r\n");

  debugf("  FPGA Reconfig  = GPIO%d", PIN_NUM_RECONFIG_N);

  wifi_log_init();

  mcu_hw_spi_init();
  mcu_hw_uart_init();
#if CONFIG_USB_HOST_ENABLE
  usb_init();
#else
  debugf("USB host disabled — Serial/JTAG active for debugging");
#endif

  bt_hid_init();
}

void mcu_hw_main_loop(void) {
  for(;;) vTaskDelay(pdMS_TO_TICKS(100));
}
