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
#include "uthernet2.h"

static inline void InitActLed() {
  gpio_init(ACT_LED_PIN);
  gpio_set_dir(ACT_LED_PIN, GPIO_OUT);
  gpio_put(ACT_LED_PIN, 1); //turn it off
}

//
//GPIO Interrupt call back. 

void gpio_intr_callback(uint gpio, uint32_t events){
  if (gpio==nRESET_PIN) {
    //Abort TFTP network task if Apple is reset
    //during TFTP transfer
    NetworkPump_RequestAbortAll();
    
    //Abort Erase Flash Disk
    //Read the note at AbortEraseFlashDisk() for more info
    AbortEraseFlashDisk();
  }
}

//
// Interrupt if Apple Reset signal is active and calloc
// gpio_intr_callback()
static void EnableAppleResetInterrupt() {
  gpio_init(nRESET_PIN);
  gpio_set_irq_enabled_with_callback(nRESET_PIN, GPIO_IRQ_EDGE_FALL, true /*enabled*/, &gpio_intr_callback);
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

//
//Use Core 0 to run background task such as TFTP or NTP Time sync
//
void __no_inline_not_in_flash_func(core0Loop)() {
  const uint32_t NEXTUPDATE_SUCCESS = (24*60*60*1000);  //If last NTP update is successful, re-sync in 24hr
  const uint32_t NEXTUPDATE_FAILED  = (5*60*1000);      //If last NTP update failed, try again in 5 min
  absolute_time_t nextUpdateTime;
  
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
  
  //Enable Pull-down resistors of unused GPIOs
  gpio_pull_down(0);
  gpio_pull_down(1);
  gpio_pull_down(26);

  U2_Init();

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
  
  //Enable Apple Reset Interrupt only if we are connected to Apple IIc
  //The /Reset signal at the expansion slot connector is floating if
  //MegaFlash is not connected to Apple. There is no pull-up resistor
  //at the transceiver input on Rev 1.0PCB. So, enable interrupt only 
  // when appleConnected is true
  if (appleConnected) {
    EnableAppleResetInterrupt();
  }
  
  //
  // Core1: Apple Bus Loop
  // Always launch Core1 so the bus loop runs. In Release we previously
  // only launched when appleConnected; that made the network stack appear
  // broken when IsAppleConnected() was false at boot (e.g. timing/PHI0).
  // Matching Debug (always run bus loop) fixes Release network behaviour.
  //
  multicore_launch_core1(core1Main);

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
  // Core0: On Pico W run the network loop (NTP/TFTP/WiFi test); otherwise
  // run User Terminal. Use CheckPicoW() so the network stack runs on Pico W
  // regardless of appleConnected (avoids Release breakage when IsAppleConnected()
  // is false at boot due to timing).
  //
  if (CheckPicoW()) {
    // If no Apple II is detected, keep USB stdio responsive by running the
    // interactive terminal instead of the network loop. This restores the
    // previous "USB console works when powered from USB" behaviour.
    //
    // When Apple is detected, switch into the network loop (NTP/TFTP/WiFi).
    if (appleConnected) {
      core0Loop();  // NTP, TFTP, WiFi test
    }

    stdio_usb_init();
    InitPicoLed();

    absolute_time_t nextAppleCheck = make_timeout_time_ms(2000);
    while (true) {
      // If Apple becomes connected later, enable reset interrupt and start
      // the network loop. Note: this only triggers between terminal sessions.
      if (time_reached(nextAppleCheck)) {
        nextAppleCheck = make_timeout_time_ms(2000);
        if (IsAppleConnected()) {
          EnableAppleResetInterrupt();
          core0Loop();  // NTP, TFTP, WiFi test
        }
      }

      if (stdio_usb_connected()) {
        UserTerminal();
      } else {
        sleep_ms(1000);
      }
    }
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

