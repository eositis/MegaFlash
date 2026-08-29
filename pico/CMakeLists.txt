cmake_minimum_required(VERSION 3.13...3.27)

#PICO_BOARD is set by command line -DPICO_BOARD=
##set(PICO_BOARD pico_w)

#All builds share the same PICOTOOL
set(PICOTOOL_FETCH_FROM_GIT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/picotool)

# Pico SDK: -DPICO_SDK_PATH=... (cache) wins; else ENV{PICO_SDK_PATH}; else $HOME/pico-sdk (same as cmakeall.sh).
# This lets `cmake -B build -S .` work in environments that do not inherit the user's shell profile.
if(NOT PICO_SDK_PATH)
  if(DEFINED ENV{PICO_SDK_PATH} AND NOT "$ENV{PICO_SDK_PATH}" STREQUAL "")
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
  else()
    set(PICO_SDK_PATH "$ENV{HOME}/pico-sdk")
  endif()
endif()

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(megaflash C CXX ASM)
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
set(PICO_CXX_ENABLE_RTTI 1)

# Keep Release codegen the same as Debug (-Og); only NDEBUG differs (disables assert + UART logging).
# Avoids -O3-induced differences (reordering, inlining) that can cause release-only bugs (e.g. TFTP stall).
set(CMAKE_C_FLAGS_RELEASE "-g -Og -DNDEBUG" CACHE STRING "Release C flags" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-g -Og -DNDEBUG" CACHE STRING "Release CXX flags" FORCE)

pico_sdk_init()

# Unix time + human-readable UTC string at configure time; build-both.sh / cmakeall.sh pass both.
# Default 0 / empty = not set (firmware falls back to __DATE__/__TIME__ in device info string).
set(FIRMWARE_BUILD_TIMESTAMP "0" CACHE STRING "Unix build timestamp (set by build scripts)")
set(FIRMWARE_BUILD_TIMESTAMP_STR "" CACHE STRING "Human-readable UTC build time (set by build scripts)")

#add source file here
add_executable(${CMAKE_PROJECT_NAME}
    main.c
    network_pump.cpp
    a2bus.c
    busloop.c
    busloop_wa.c
    cmdhandler.c
    flash.c
    flashunitmapper.c
    mediaaccess.c
    romdisk.c
    romdisk.s       
    ramdisk.c
    usbserial.c
    filetransfer.c
    dmamemops.c
    formatter.c
    rtc.c
    misc.c
    cpanel.s
    userconfig.c
    encryption.c
    terminal.c
    slinky.c
    fpu.c
    uthernet2.c
    u2_monitor.c
    uthernet2_net.cpp
    network.cpp
    udptask.cpp
    ntptask.cpp
    testwifitask.cpp
    tftptask.cpp
    tftprxtask.cpp
    tftptxtask.cpp
    tftpstate.c
    smb/smbdisk.c
)

set(FIRMWARE_BUILD_TIMESTAMP_STR_DISPLAY "${FIRMWARE_BUILD_TIMESTAMP_STR}")
if(FIRMWARE_BUILD_TIMESTAMP_STR_DISPLAY STREQUAL "")
  set(FIRMWARE_BUILD_TIMESTAMP_STR_DISPLAY "unknown")
endif()
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/build_id.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/build_id.h
  @ONLY
)

#standard C lib printf for FPU fout() function
pico_set_printf_implementation(${CMAKE_PROJECT_NAME} compiler)

#extra file dependencies
set_source_files_properties(romdisk.s OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/romdisk.po)
set_source_files_properties(cpanel.s  OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../cpanel/cpanel.bin)

#to generate .uf2 file
pico_add_extra_outputs(${CMAKE_PROJECT_NAME})

#to compile .pio file
pico_generate_pio_header(${CMAKE_PROJECT_NAME}
	${CMAKE_CURRENT_SOURCE_DIR}/a2bus_rp2040.pio
)

pico_generate_pio_header(${CMAKE_PROJECT_NAME}
	${CMAKE_CURRENT_SOURCE_DIR}/a2bus_rp2350.pio
)

#include directories
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
)

#linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
  pico_stdlib
  pico_multicore    
  pico_mem_ops  
  pico_aon_timer
  pico_float
  pico_double
  hardware_pio
  hardware_spi
  hardware_dma
  hardware_divider
  hardware_adc
  hardware_watchdog
  pico_cyw43_arch_lwip_poll
)

#RP2040: Set system speed to 150MHz
#SPI does not work at 100MHz. So, set the speed to 150MHz
#and run SPI at 75MHz
if (PICO_BOARD STREQUAL pico_w)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE SYS_CLK_MHZ=150)
endif()

# RP2350: extra SPI /CS nops in flash.c when CPU is overclocked (Thomas upstream OC_RP2350).
if (PICO_BOARD STREQUAL pico2_w)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE OC_RP2350=1)
endif()

# U2 / ip65 debug (Debug builds only): default low noise — use checkpoints to bisect init.
# Convenience: ./build-debug-both.sh  (sets CMAKE_BUILD_TYPE=Debug; optional env U2_IP65_CHECKPOINT, U2_IP65_TRACE_DATA, U2_MON_LOG_BUS)
# Manual: cmake -B pico2_debug -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug -DU2_IP65_CHECKPOINT=3
set(U2_IP65_CHECKPOINT "1" CACHE STRING "ip65 bisect: 0=off, 1=MODE(0x03), 2=RTR0, 3=RTR1, 4=RMSR, 5=MACRAW OPEN ok (default 1)")
set(U2_MON_LOG_BUS "0" CACHE STRING "1=verbose [u2m] per bus cycle at C0C4-C0C7 (floods UART)")
set(U2_IP65_TRACE_DATA "0" CACHE STRING "1=verbose 48 DATA reads after MODE=0x03 ([u2])")
set(U2_ETH_HEADER_TRACE "0" CACHE STRING "1=UART [u2eth] hex dump first 64B of STA TX/RX Ethernet frames (bring-up; not pcap/tcpdump)")
set(U2_MACRAW_COMPAT_DROP_OLDEST "0" CACHE STRING "1=MACRAW RX: discard unread once when full to admit new frame (compat; not strict W5100)")
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE U2_ETH_HEADER_TRACE=${U2_ETH_HEADER_TRACE})
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE U2_MACRAW_COMPAT_DROP_OLDEST=${U2_MACRAW_COMPAT_DROP_OLDEST})
# Native SMB3 share disk. Pico W (RP2040) overflows RAM (~8 KiB) with the client BSS;
# default off there. Pico 2 W keeps it on. Override: -DMEGAFLASH_SMB=0|1
if(PICO_BOARD STREQUAL "pico_w")
  set(MEGAFLASH_SMB_DEFAULT "0")
else()
  set(MEGAFLASH_SMB_DEFAULT "1")
endif()
set(MEGAFLASH_SMB "${MEGAFLASH_SMB_DEFAULT}" CACHE STRING "1=enable native SMB3 SmartPort volume")
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE MEGAFLASH_SMB=${MEGAFLASH_SMB})
if(MEGAFLASH_SMB STREQUAL "1")
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    smb/smb_crypto.c
    smb/smb_prodos.c
    smb/smb_client.cpp
  )
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    UTHERNET2_DEBUG=1
    U2_ACTIVITY_MONITOR=1
    U2_IP65_CHECKPOINT=${U2_IP65_CHECKPOINT}
    U2_MON_LOG_BUS=${U2_MON_LOG_BUS}
    U2_IP65_TRACE_DATA=${U2_IP65_TRACE_DATA}
  )
endif()

#enable stdio 
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000)
pico_enable_stdio_usb(${CMAKE_PROJECT_NAME} 1)
pico_enable_stdio_uart(${CMAKE_PROJECT_NAME} 1)

# Optional: standalone SPI NOR tests (read sweep + destructive XMODEM-like DMA program/verify). Not megaflash.uf2.
option(MEGAFLASH_BUILD_FLASH_SI_TEST "Build flash_si_test target (SPI characterization)" OFF)
if(MEGAFLASH_BUILD_FLASH_SI_TEST)
  add_executable(flash_si_test
    flash_si_test/main.c
    flash_si_test/flash_si_pins.c
    flash_si_test/flash_si_softspi.c
    flash_si_test/xmodem_like.c
    dmamemops.c
  )
  target_link_libraries(flash_si_test PRIVATE pico_stdlib hardware_spi hardware_dma pico_mem_ops)
  target_include_directories(flash_si_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  pico_enable_stdio_usb(flash_si_test 1)
  pico_enable_stdio_uart(flash_si_test 1)
  pico_add_extra_outputs(flash_si_test)
  target_compile_definitions(flash_si_test PRIVATE PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000)
  if(PICO_BOARD STREQUAL pico_w)
    target_compile_definitions(flash_si_test PRIVATE SYS_CLK_MHZ=150)
  endif()
  if(PICO_BOARD STREQUAL pico2_w)
    target_compile_definitions(flash_si_test PRIVATE OC_RP2350=1)
  endif()
endif()
