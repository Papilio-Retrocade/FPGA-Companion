/*
  mcu_hw.c - MiSTeryNano FPGA companion hardware driver for esp32 s2/s3
*/

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
#include "esp_log.h"


//#define USB_ERROR_CHECK(a)  ESP_ERROR_CHECK(a)
#define USB_ERROR_CHECK(a) (a)

/* ========================================================================= */
/* =========                          USB                        =========== */
/* ========================================================================= */
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
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
    uint8_t data[16];
    size_t data_length = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        USB_ERROR_CHECK( hid_host_device_get_raw_input_report_data(hid_device_handle,
					   data, sizeof(data), &data_length));

	// find matching hid report
	for(int idx=0;idx<MAX_HID_DEVICES;idx++)
	  if(hid_device[idx].handle == hid_device_handle)     
	    hid_parse(&hid_device[idx].rep, &hid_device[idx].state, data, data_length);
	  
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
	      hid_device[idx].handle = hid_device_handle;
	      if(hid_device[idx].rep.type == REPORT_TYPE_JOYSTICK)
		hid_device[idx].state.joystick.js_index = hid_allocate_joystick();
	      
	      USB_ERROR_CHECK( hid_host_device_start(hid_device_handle) );
	    } else
	      usb_debugf("ignoring device");
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
                                           4096,
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
        .stack_size = 4096,
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
    task_created = xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);
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

// End of check for CONFIG_IDF_TARGET_ESP32S3
#endif
/* ========================================================================= */
/* ========                          SPI                            ======== */
/* ========================================================================= */

#include "driver/spi_master.h"
#include "driver/gpio.h"

#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10
#define PIN_NUM_IRQ  9
// #define PIN_NUM_IRQ  14

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
     .max_transfer_sz = 32
  };
  
  spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

  spi_device_interface_config_t devcfg = {
     .clock_speed_hz = 20 * 1000 * 1000,      // 20 MHz
     .mode = 1,                               // SPI mode 1
     .spics_io_num = -1,
     .command_bits = 0,                       // no command, address or dummy bits since we
     .address_bits = 0,                       // are tranferring single bytes
     .dummy_bits = 0,     
     .queue_size = 7,                         // We want to be able to queue 7 transactions at a time
  };
  
  spi_bus_add_device(SPI2_HOST, &devcfg, &spi);

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
}

void mcu_hw_irq_ack(void) {
  // re-enable the interrupt since it was now serviced outside the irq handler
  gpio_intr_enable(PIN_NUM_IRQ);
}

void mcu_hw_spi_begin() {
  xSemaphoreTake(sem, 0xffffffffUL);      // wait forever
  gpio_set_level(PIN_NUM_CS, 0);  
}

void mcu_hw_spi_end() {
  gpio_set_level(PIN_NUM_CS, 1);
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

/* ========================================================================= */
/* =========                          BLUETOOTH                  =========== */
/* ========================================================================= */
static const char *TAG = "ESP_BT";

#include "nvs_flash.h"
#include "esp_bt.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#else
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#endif

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#define ESP_BD_ADDR_STR         "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(addr)   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]
#else
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#endif

#include "esp_hidh.h"
#include "esp_hid_gap.h"

static struct {
  int handle;
  hid_state_t state;
  hid_report_t rep;
} bt_hid_device[MAX_HID_DEVICES];

// typedef struct {
//   uint8_t bSize: 2;
//   uint8_t bType: 2;
//   uint8_t bTag: 4;
// } __attribute__((packed)) item_t;



void bt_hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            ESP_LOGI(TAG, ESP_BD_ADDR_STR " OPEN: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->open.dev));
            esp_hidh_dev_dump(param->open.dev, stdout);
            const esp_hid_device_config_t *config = esp_hidh_dev_config_get(param->open.dev);
            esp_hid_raw_report_map_t *report_map = config->report_maps;
            parse_report_descriptor(report_map->data, report_map->len, &bt_hid_device[0].rep, NULL);
        } else {
            ESP_LOGE(TAG, " OPEN failed!");
        }
        break;
    }
    case ESP_HIDH_BATTERY_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " BATTERY: %d%%", ESP_BD_ADDR_HEX(bda), param->battery.level);
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->input.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " INPUT: %8s, MAP: %2u, ID: %3u, Len: %d, Byte: 0x%08X, Data:", ESP_BD_ADDR_HEX(bda), esp_hid_usage_str(param->input.usage), param->input.map_index, param->input.report_id, param->input.length, param->input.data[2]);
        ESP_LOG_BUFFER_HEX(TAG, param->input.data, param->input.length);

        // item_t all = ((item_t*)rep)->bTag;
        // item_t testing = (item_t*)param->input.usage->bType;
        // uint8_t size = ((item_t*)rep)->bSize;

        // typedef struct {
        //   uint8_t bSize: 2;
        //   uint8_t bType: 2;
        //   uint8_t bTag: 4;
        // } __attribute__((packed)) item_t;

        // item_t testing;
        // testing.bType = (item_t*)param->input.usage;

        // struct test_t testing = { 0, param->input.usage, 0 };
        // struct {
        //   uint8_t bSize: 2;
        //   uint8_t bType: 2;
        //   uint8_t bTag: 4;
        // } __attribute__((packed)) testing = {0,0,0};
        // testing.bType = param->input.usage;


        // uint8_t temp[sizeof(param->input.length + 1)];
        // temp[0] = *(uint8_t*)&testing;
        // for (int i=0; i< param->input.length + 1; i++)
        //   temp[i+1] = param->input.data[i];
        hid_parse(&bt_hid_device[0].rep, &bt_hid_device[0].state, param->input.data, param->input.length);

        // if(parse_report_descriptor(report_desc, report_desc_len, &hid_device[idx].rep, NULL)) {
          // hid_device[idx].handle = hid_device_handle;
          // if(hid_device[idx].rep.type == REPORT_TYPE_JOYSTICK)
        // hid_device[idx].state.joystick.js_index = hid_allocate_joystick();

        // for(int idx=0;idx<MAX_HID_DEVICES;idx++)
        //   if(hid_device[idx].handle == hid_device_handle)     
        //     hid_parse(&hid_device[idx].rep, &hid_device[idx].state, data, data_length);

        // mcu_hw_spi_begin();
        // mcu_hw_spi_tx_u08(SPI_TARGET_HID);
        // mcu_hw_spi_tx_u08(SPI_HID_KEYBOARD);
        // mcu_hw_spi_tx_u08(core_map_key(param->input.data[2]));
        // mcu_hw_spi_end();
        break;
    }
    case ESP_HIDH_FEATURE_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->feature.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " FEATURE: %8s, MAP: %2u, ID: %3u, Len: %d", ESP_BD_ADDR_HEX(bda),
                 esp_hid_usage_str(param->feature.usage), param->feature.map_index, param->feature.report_id,
                 param->feature.length);
        ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " CLOSE: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->close.dev));
        break;
    }
    default:
        ESP_LOGI(TAG, "EVENT: %d", event);
        break;
    }
}

#define SCAN_DURATION_SECONDS 5

void bt_hid_task(void *pvParameters)
{
    size_t results_len = 0;
    esp_hid_scan_result_t *results = NULL;
    ESP_LOGI(TAG, "SCAN...");
    //start scan for HID devices
    esp_hid_scan(SCAN_DURATION_SECONDS, &results_len, &results);
    ESP_LOGI(TAG, "SCAN: %u results", results_len);
    if (results_len) {
        esp_hid_scan_result_t *r = results;
        esp_hid_scan_result_t *cr = NULL;
        while (r) {
            printf("  %s: " ESP_BD_ADDR_STR ", ", (r->transport == ESP_HID_TRANSPORT_BLE) ? "BLE" : "BT ", ESP_BD_ADDR_HEX(r->bda));
            printf("RSSI: %d, ", r->rssi);
            printf("USAGE: %s, ", esp_hid_usage_str(r->usage));
#if CONFIG_BT_BLE_ENABLED
            if (r->transport == ESP_HID_TRANSPORT_BLE) {
                cr = r;
                printf("APPEARANCE: 0x%04x, ", r->ble.appearance);
                printf("ADDR_TYPE: '%s', ", ble_addr_type_str(r->ble.addr_type));
            }
#endif /* CONFIG_BT_BLE_ENABLED */
#if CONFIG_BT_NIMBLE_ENABLED
            if (r->transport == ESP_HID_TRANSPORT_BLE) {
                cr = r;
                printf("APPEARANCE: 0x%04x, ", r->ble.appearance);
                printf("ADDR_TYPE: '%d', ", r->ble.addr_type);
            }
#endif /* CONFIG_BT_BLE_ENABLED */
#if CONFIG_BT_HID_HOST_ENABLED
            if (r->transport == ESP_HID_TRANSPORT_BT) {
                cr = r;
                printf("COD: %s[", esp_hid_cod_major_str(r->bt.cod.major));
                esp_hid_cod_minor_print(r->bt.cod.minor, stdout);
                printf("] srv 0x%03x, ", r->bt.cod.service);
                print_uuid(&r->bt.uuid);
                printf(", ");
            }
#endif /* CONFIG_BT_HID_HOST_ENABLED */
            printf("NAME: %s ", r->name ? r->name : "");
            printf("\n");
            r = r->next;
        }
        if (cr) {
            //open the last result
            esp_hidh_dev_open(cr->bda, cr->transport, cr->ble.addr_type);
        }
        //free the results
        esp_hid_scan_results_free(results);
    }
    vTaskDelete(NULL);
}

#if CONFIG_BT_NIMBLE_ENABLED
void ble_hid_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}
void ble_store_config_init(void);
#endif

void mcu_hw_bt_init(void)
{

    // mark all entries as unused
    for(int idx=0;idx<MAX_HID_DEVICES;idx++)
      bt_hid_device[idx].handle = -1;

    esp_err_t ret;
#if HID_HOST_MODE == HIDH_IDLE_MODE
    ESP_LOGE(TAG, "Please turn on BT HID host or BLE!");
    return;
#endif
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
    ESP_LOGI(TAG, "setting hid gap, mode:%d", HID_HOST_MODE);
    ESP_ERROR_CHECK( esp_hid_gap_init(HID_HOST_MODE) );
#if CONFIG_BT_BLE_ENABLED
    ESP_ERROR_CHECK( esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler) );
#endif /* CONFIG_BT_BLE_ENABLED */
    esp_hidh_config_t config = {
        .callback = bt_hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK( esp_hidh_init(&config) );

#if CONFIG_BT_NIMBLE_ENABLED
    /* XXX Need to have template for store */
    ble_store_config_init();

    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
	/* Starting nimble task after gatts is initialized*/
    ret = esp_nimble_enable(ble_hid_host_task);
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_enable failed: %d", ret);
    }
#endif
    xTaskCreate(&bt_hid_task, "bt_hid_task", 15 * 1024, NULL, 2, NULL);
}

void mcu_hw_reset(void) {
  debugf("RESET");
  esp_restart();
  for(;;);
}

void mcu_hw_init(void) {
  printf("\r\n\r\n" LOGO "           FPGA Companion for ESP32-S2/S3\r\n\r\n");

  mcu_hw_spi_init();
  #if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  usb_init();
  #endif
  mcu_hw_bt_init();
}

void mcu_hw_main_loop(void) {
  for(;;) vTaskDelay(pdMS_TO_TICKS(100));
}