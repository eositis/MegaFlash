#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "pico/multicore.h"
#include "defines.h"
#include "build_id.h"
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
#include "ramdisk.h"
#include "ipc.h"
#include "network.h"
#include "tftpstate.h"
#include "uthernet2.h"
#include "uthernet2_net.h"
#include "u2_monitor.h"

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
// Core 0: lwIP/CYW43 poll + IPC from core 1 (Test WiFi, TFTP, NET_WAKE).
// Must run whenever Pico W is used: both inside core0Loop and on the
// USB-terminal path when appleConnected was false at boot (otherwise
// IPC is never popped and Test WiFi hangs; WiFi LED/stack stay idle).
//
// Poll network stack before blocking on the FIFO so inbound MACRAW/TCP and
// deferred MACRAW TX are serviced immediately (§10x); IPCCMD_NET_WAKE only
// needs to unblock the wait — dispatch is a no-op after poll.
//
static void PicoW_DispatchIpc(struct IpcMsg *msg) {
  if (msg->command == IPCCMD_WIFITEST) {
    TestWifi((TestResult_t *)msg->data);
  } else if (msg->command == IPCCMD_TFTP) {
    ExecuteTFTP(msg->data /* taskid */);
  }
}

static void PicoW_ServiceCore0IpcAndNetwork(uint64_t fifo_timeout_us) {
  U2_Net_Poll();
  U2_MonPollFlush();
  uint32_t param;
  while (multicore_fifo_pop_timeout_us(0, &param)) {
    PicoW_DispatchIpc((struct IpcMsg *)param);
  }
  if (fifo_timeout_us > 0 &&
      multicore_fifo_pop_timeout_us(fifo_timeout_us, &param)) {
    PicoW_DispatchIpc((struct IpcMsg *)param);
  }
}

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
      U2_MonPollFlush();
      int err = GetNetworkTime();
      DEBUG_PRINTF("GetNTP err=%d (%d=NETERR_NONE)\n",err,NETERR_NONE);
      if (err==NETERR_NONE) nextUpdateTime = make_timeout_time_ms(NEXTUPDATE_SUCCESS);
      else nextUpdateTime = make_timeout_time_ms(NEXTUPDATE_FAILED);

        //wait until nextUpdateTime or msg from other core
        do {
#if defined(NDEBUG)
          ReleaseUpdateBusUsbGate();
#endif
          PicoW_ServiceCore0IpcAndNetwork(0);
        } while (!time_reached(nextUpdateTime) && !updateNTPNow);
      
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
  InitRamdisk();
  InitTFTPState();
  
  //Enable Pull-down resistors of unused GPIOs
  gpio_pull_down(0);
  gpio_pull_down(1);
  gpio_pull_down(26);

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

  /* After UART: U2_Init / U2_MonInit use printf — if this ran earlier, [u2]/[u2m] boot text was lost. */
  U2_Init();

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

#if defined(NDEBUG)
  ReleaseInitBusUsbGate(appleConnected);
#endif
  
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
  DEBUG_PRINTF("\nMegaflash DEBUG Firmware Version %d (%s)\n",
               FIRMWAREVER, FIRMWAREVERSTR);
  /* Always print: build scripts set Unix time + UTC string; plain CMake leaves 0 / "unknown". */
  DEBUG_PRINTF("Firmware build: %s  (%lu Unix s)\n",
               FIRMWARE_BUILD_TIMESTAMP_STR,
               (unsigned long)(uint32_t)FIRMWARE_BUILD_TIMESTAMP);
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
    /* USB before core0Loop so Release stdio_usb_connected() / gate are valid in core0Loop. */
    stdio_usb_init();
    InitPicoLed();
#if defined(NDEBUG)
    ReleaseUpdateBusUsbGate();
#endif

    // If no Apple II is detected, keep USB stdio responsive by running the
    // interactive terminal instead of the network loop. This restores the
    // previous "USB console works when powered from USB" behaviour.
    //
    // When Apple is detected, switch into the network loop (NTP/TFTP/WiFi).
    if (appleConnected) {
      core0Loop();  // NTP, TFTP, WiFi test
    }

    absolute_time_t nextAppleCheck = make_timeout_time_ms(2000);
    while (true) {
#if defined(NDEBUG)
      ReleaseUpdateBusUsbGate();
#endif
      /* lwIP + IPC (Test WiFi / TFTP) when not in core0Loop; non-blocking FIFO. */
      PicoW_ServiceCore0IpcAndNetwork(0);
      // If Apple becomes connected later, enable reset interrupt and start
      // the network loop. Note: this only triggers between terminal sessions.
      if (time_reached(nextAppleCheck)) {
        nextAppleCheck = make_timeout_time_ms(2000);
        if (IsAppleConnected()) {
          EnableAppleResetInterrupt();
          core0Loop();  // NTP, TFTP, WiFi test
        }
      }

#if defined(NDEBUG)
      if (stdio_usb_connected() && !IsAppleConnected()) {
        UserTerminal();
      } else
#else
      if (stdio_usb_connected()) {
        UserTerminal();
      } else
#endif
      {
        /* Poll network stack while idle; a plain sleep_ms(1000) starved cyw43. */
        absolute_time_t sleep_until = make_timeout_time_ms(1000);
        while (!time_reached(sleep_until)) {
          PicoW_ServiceCore0IpcAndNetwork(0);
          sleep_ms(1);
        }
      }
    }
  } else {
    stdio_usb_init();
    InitPicoLed();
#if defined(NDEBUG)
    ReleaseUpdateBusUsbGate();
#endif

    while(1) {
#if defined(NDEBUG)
      ReleaseUpdateBusUsbGate();
#endif
#if defined(NDEBUG)
      if (stdio_usb_connected() && !IsAppleConnected()) {
        UserTerminal();
      } else if (!stdio_usb_connected()) {
        sleep_ms(1000);
      } else {
        /* USB cable connected but Apple II bus mode: do not run USB terminal. */
        sleep_ms(10);
      }
#else
      if (stdio_usb_connected()) {
        UserTerminal();
      } else {
        sleep_ms(1000);
      }
#endif
    }
  }

  while(true);
  return 0;
}

