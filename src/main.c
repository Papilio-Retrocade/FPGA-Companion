/*
  main.c - MiSTeryNano FPGA Companion Pi Pico variant

*/

#include "../mcu_hw.h"

#ifdef ESP_PLATFORM
#include "wifi_log.h"
#endif

#include "../config.h"
#include "../sysctrl.h"
#include "../sdc.h"
#include "../osd.h"
#include "../menu.h"
#include "../core.h"
#include "../inifile.h"
#include "../debug.h"
#include "../xml.h"
#include "../tangcore.h"

/*-----------------------------------------------------------*/
/*---            main FPGA communication task            ----*/
/*-----------------------------------------------------------*/

TaskHandle_t com_task_handle;

static void com_task(__attribute__((unused)) void *p ) {
  debugf("Starting main communication task");
  
  /* Wait for interface arbitration: give tangcore_task up to 3s to claim UART */
  {
    int wait = 300;
    while(active_interface == 0 && wait-- > 0)
      vTaskDelay(pdMS_TO_TICKS(10));
  }

  if(active_interface == 2) {
    debugf("com_task: UART interface claimed by tangcore_task, idling");
    for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  /* Claim SPI interface */
  if(active_interface == 0) active_interface = 1;

  // startup FPGA, this will also put the core into reset
  if(sys_wait4fpga()) {
    // FPGA is ready and can be talked to

    // initialitze SD card
    debugf("SDC init..."); vTaskDelay(pdMS_TO_TICKS(100));
    sdc_init();
    debugf("SDC init done");
    
    // try to load a config .xml from sd card. If the core has identified itself,
    // then e.g. atarist.xml will be read. otherwise config.xml
    FIL fil;
    debugf("Opening config: %s", sys_get_config_name()); vTaskDelay(pdMS_TO_TICKS(100));
    if(f_open(&fil, sys_get_config_name(), FA_OPEN_EXISTING | FA_READ) == FR_OK) {
      config_init();

      UINT br; char c;
      debugf("XML config open, parsing..."); vTaskDelay(pdMS_TO_TICKS(100));

      // read byte by byte. Slow but that doesn't hurt ...
      FRESULT r = f_read(&fil, &c, 1, &br);
      while(r == FR_OK && br) {
	xml_parse(c);      
	r = f_read(&fil, &c, 1, &br);
      }    
      f_close(&fil);

      debugf("XML parsed, config %s", cfg ? "valid" : "null");
      vTaskDelay(pdMS_TO_TICKS(100));
      config_dump();
    } else {
      debugf("No XML config found (%s)", sys_get_config_name());
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    // process any pending interrupt. Filter out irq 1 which is the
    // FPGA cold boot event which we ignore since we just booted outselves
    debugf("Handling IRQs..."); vTaskDelay(pdMS_TO_TICKS(100));
    sys_handle_interrupts(sys_irq_ctrl(0xff) & 0xfe);
    
    // by default, DB9 interrupts are disabled. Reading
    // the DB9 state enables them. This is what hid_handle_event
    // does.
    debugf("HID init..."); vTaskDelay(pdMS_TO_TICKS(100));
    hid_handle_event();
    debugf("HID init done");

    if(!cfg) {
      // finally release FPGA from reset
      debugf("Releasing FPGA reset (no cfg)..."); vTaskDelay(pdMS_TO_TICKS(100));
      sys_set_val('R', 0);
    }
      
    // initialize on-screen-display and menu system
    debugf("OSD init..."); vTaskDelay(pdMS_TO_TICKS(100));
    osd_init();
    debugf("OSD init done");
    debugf("Menu init..."); vTaskDelay(pdMS_TO_TICKS(100));
    menu_init();
    debugf("Menu init done");

    // open disk images, either defaults set in sdc_init or
    // user configure ones from the ini file
    debugf("Mounting defaults..."); vTaskDelay(pdMS_TO_TICKS(100));
    sdc_mount_defaults();
    debugf("Defaults mounted");

    // cold reset so the C64 boots with default images (especially CRT) already loaded.
    // Without this, the CPU starts before the CRT ROM is in SDRAM and ignores it.
    debugf("Final reset cycle..."); vTaskDelay(pdMS_TO_TICKS(100));
    sys_set_val('R', 3);
    sys_set_val('R', 0);
  }

  debugf("Entering main loop");

#ifdef ESP_PLATFORM
  wifi_log_main_loop_reached();
#endif
  
  for(;;) {
    mcu_hw_irq_ack();  // re-enable interrupt
    
    ulTaskNotifyTake( pdTRUE, portMAX_DELAY);    
    sys_handle_interrupts(sys_irq_ctrl(0xff));      
  }
}

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  mcu_hw_init();
  
  // run tangcore UART task (probes FPGA for BL616 protocol; claims active_interface=2 if found)
  xTaskCreate( tangcore_task, "tangcore", 8192, NULL, CONFIG_MAX_PRIORITY-2, NULL );

  // run FPGA com thread (SPI iosys_retrocade; idles if tangcore_task claims UART)
  xTaskCreate( com_task, "FPGA Com", 8192, NULL, CONFIG_MAX_PRIORITY-1, &com_task_handle );

  mcu_hw_main_loop();

#ifndef ESP_PLATFORM
  return 0;
#endif
}
/*-----------------------------------------------------------*/

