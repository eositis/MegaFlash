#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "pico/multicore.h"
#include "defines.h"
#include "debug.h"
#include "a2bus.h"
#include "busloop.h"
#include "busloop_wa.h"
#include "flash.h"
#include "flashunitmapper.h"
#include "dmamemops.h"
#include "misc.h"
#include "userconfig.h"
#include "cmdhandler.h"
#include "terminal.h"
#include "slinky.h"
#include "ipc.h"
#include "network.h"
#include "tftpstate.h"

static inline void InitActLed() {
  gpio_init(ACT_LED_PIN);
  gpio_set_dir(ACT_LED_PIN, GPIO_OUT);
  gpio_put(ACT_LED_PIN, 1); //turn it off
}

void gpio_callback(uint gpio, uint32_t events){
  //Abort the network task if Apple is reset.
  if (gpio==nRESET_PIN && IsUDPTaskRunning()) {
    //Kill any running UDPTask except NTPTask
    if (!IsNTPTaskRunning()) {
      UDPTask_RequestAbortIfRunning(); 
    }
  }
}

static void EnableAppleResetInterrupt() {
  gpio_init(nRESET_PIN);
  gpio_set_irq_enabled_with_callback(nRESET_PIN, GPIO_IRQ_EDGE_FALL, true /*enabled*/, &gpio_callback);
}

//
//Use Core 1 to run Bus Loop
//
void __no_inline_not_in_flash_func(core1Main)() {
  //No interrupt on this core
  //Bus Loop is time critical
  save_and_disable_interrupts();  

  //Initalize data
  CommandHandlerInit();
   
#ifdef PICO_RP2040
  //RP2040 does not have enough memory to emulate a slinky (min: 256kB)
  BusLoopWaitActiviation();
#else
  SlinkyInit();
  BusLoopSlinky();
#endif
  
  //Start actual bus loop
  BusLoopDataInit();    //It takes 8us to complete the initialization
  BusLoop();
}

volatile bool updateNTPNow = false;


void __no_inline_not_in_flash_func(core0Loop)() {
  const uint32_t NEXTUPDATE_SUCCESS = (24*60*60*1000);  //If last NTP update is successful, re-sync in 24hr
  const uint32_t NEXTUPDATE_FAILED  = (5*60*1000);      //If last NTP update failed, try again in 5 min
  absolute_time_t nextUpdateTime;
  
  //wait awhile for core 1 to load user configuration
  sleep_ms(100);  

  if (CheckPicoW()) {
    do {
      updateNTPNow = false;
      int err = GetNetworkTime();
      DEBUG_PRINTF("GetNTP err=%d (%d=NETERR_NONE)\n",err,NETERR_NONE);
      if (err==NETERR_NONE) nextUpdateTime = make_timeout_time_ms(NEXTUPDATE_SUCCESS);
      else nextUpdateTime = make_timeout_time_ms(NEXTUPDATE_FAILED);

        //wait until nextUpdateTime or msg from other core
        do {
          uint32_t param;
          bool msgReceived = multicore_fifo_pop_timeout_us(50*1000,&param);
          if (msgReceived) {
            struct IpcMsg* msg=(struct IpcMsg*)param;
            if (msg->command == IPCCMD_WIFITEST) {
              TestWifi((TestResult_t*)msg->data);
            } else if (msg->command == IPCCMD_TFTP) {
              ExecuteTFTP(msg->data /*taskid*/);
            }
          }
        }while (!time_reached(nextUpdateTime) && !updateNTPNow);
      
    } while(1);
  } else {
    //Not running on PicoW
    //Keep popping fifo queue to avoid blocking
    while(1) multicore_fifo_pop_blocking();
  }
}

int main() {
  InitPIO();  
  InitSpi();
  InitFlash();
  InitActLed();
  InitDMAChannel();
  InitTFTPState();
  EnableAppleResetInterrupt();
  
  //Enable Pull-down resistors of unused GPIOs
  gpio_pull_down(0);
  gpio_pull_down(1);
  gpio_pull_down(26);
  gpio_pull_down(27);  
  
  
#ifndef NDEBUG
  //For sending Debug Message to UART
  stdio_uart_init();    //Default baud: 115200

  //Disable stdout buffering
  //otherwise, text is not printed to uart or usb correctly.
  setbuf(stdout, NULL); 
#else
  //Disable stdio_uart for Release Build
  stdio_set_driver_enabled(&stdio_uart, false);
#endif

  //Load userConfig and Wifi Settings from security registers
  LoadAllConfigs();  
  
  //Setup Flash Units Mapping Data
  SetupFlashUnitMapping();  
  EnableFlashUnitMapping();

  //Check if we are connecting to Apple IIc
  bool appleConnected = IsAppleConnected();
  
  //
  // Core1: Apple Bus Loop
  //
  // For DEBUG Build, we want the Bus Loop is always running
  // so that we can turn on Apple computer to do testing at
  // any time. So, we run Bus Loop and User Terminal on different
  // CPU core.
  //
  // For Release Build, Bus Loop is not executed if Pico is not 
  // connected to Apple.
  //
#ifdef NDEBUG
  if (appleConnected) multicore_launch_core1(core1Main);  
#else
  multicore_launch_core1(core1Main);  //DEBUG build
#endif

  //
  //Save initialized setting from memory to flash if needed
  //See LoadAllConfigs() in userconfig.c
  //
  SaveConfigs();

  //
  //Print Debug Infomation to serial port
  //
  DEBUG_PRINTF("\nMegaflash DEBUG Firmware Version %d\n",FIRMWAREVER);
  DEBUG_PRINTF("CPU Clock Speed =%dMHz\n",clock_get_hz(clk_sys)/1000000);
  DEBUG_PRINTF("clk_peri =%dMHz\n",clock_get_hz(clk_peri)/1000000);
  DEBUG_PRINTF("SPI Speed = %dMHz\n",spi_get_baudrate(spi0)/1000000);
  DEBUG_PRINTF("WIFI Supported = %s\n",CheckPicoW()?"Yes":"No");
  DEBUG_PRINTF("Total heap = %d\n",GetTotalHeap());
  DEBUG_PRINTF("Free heap  = %d\n",GetFreeHeap());
  
  //
  //Core0: Running Wifi if connected to Apple IIc
  //       User Terminal if not.
  //
  if (appleConnected) {
    core0Loop();  //Running Wifi
  } else {
    stdio_usb_init();
    InitPicoLed();
    
    while(1) {
      if (stdio_usb_connected()) {
        UserTerminal();
      } else {
        sleep_ms(1000);
      }
    }
  }

  while(true);
  return 0;
}

