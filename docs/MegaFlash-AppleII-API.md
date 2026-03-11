## MegaFlash Apple IIc API Reference

This document catalogs the **features MegaFlash exposes to the Apple IIc/IIc+**, and how to call them:

- From **65C02 assembly** (using the `$C0C0` command interface), and
- From **Applesoft BASIC**, using `POKE`/`PEEK` or normal BASIC keywords when MegaFlash has patched the ROM.

Where a feature is already integrated into ROM (e.g. FPU, clock, SmartPort), the **underlying mechanism** is also described so you can bypass it from other languages.

---

## 1. Hardware interface and calling convention

### 1.1 Registers and status bits

All direct MegaFlash commands use four soft‑switches in the slot‑4 I/O range:

```0:0:/Users/eositis/Documents/GitHub/MegaFlash/common/defines.inc
cmdreg   := $C0C0   ; Command register (write) / Status register (read)
statusreg:= $C0C0   ; Alias for cmdreg when reading status
paramreg := $C0C1   ; Parameter stream
datareg  := $C0C2   ; Data stream
idreg    := $C0C3   ; Device ID / reserved
```

Status bits (read from `$C0C0`):

- **Bit 7** `BUSYFLAG` – 1 while command is running.
- **Bit 6** `ERRORFLAG` – 1 if the last command failed.
- **Bits 0–4** `ERRORCODEFIELD` – Pico error code.

### 1.2 Generic assembly calling pattern

```asm
        ; Optional: reset both parameter and data pointers
        LDA #CMD_RESETBOTHPTRS      ; = $00
        STA cmdreg

        ; Write parameters (if any)
        LDA unitNum
        STA paramreg
        ; ... more STA paramreg / datareg as needed ...

        ; Issue the command
        LDA #CMD_GETDEVSTATUS       ; example
        STA cmdreg

@wait:  BIT statusreg               ; BUSYFLAG in bit 7
        BMI @wait                   ; loop while busy

        BIT statusreg               ; test ERRORFLAG (bit 6)
        BVC @ok                     ; error flag clear -> OK
        ; on error, A still holds status; AND #ERRORCODEFIELD if needed
@ok:
        ; read results from paramreg / datareg as defined per command
```

### 1.3 Generic Applesoft calling pattern

Applesoft has no direct bit operations; use `PEEK` ranges instead. Define:

```basic
10 CMDREG = 49344  : REM $C0C0
20 PARAMREG = 49345: REM $C0C1
30 DATAREG = 49346 : REM $C0C2
```

Then issue a command like:

```basic
100 REM CMD_GETDEVSTATUS = 17
110 POKE CMDREG,17
120 IF PEEK(CMDREG) >= 128 THEN 120   : REM wait while BUSYFLAG (bit7) set
130 REM PEEK(CMDREG) >= 64 means ERRORFLAG set; not checked here
140 U = PEEK(PARAMREG)                : REM first status byte = unit count
```

You can wrap these in `GOSUB` blocks to create higher‑level Applesoft “functions”.

---

## 2. Storage and device status

### 2.1 Get device status / unit count (`CMD_GETDEVSTATUS = $11`)

**Purpose**: Return global device status; first byte is the **number of SmartPort units**.

#### Assembly

```asm
        LDA #CMD_GETDEVSTATUS        ; = $11
        STA cmdreg
@wait:  BIT statusreg
        BMI @wait

        LDA paramreg                 ; unit count
        ; A = number of units
```

#### Applesoft

```basic
10 CMDREG=49344:PARAMREG=49345
20 POKE CMDREG,17          : REM CMD_GETDEVSTATUS
30 IF PEEK(CMDREG)>=128 THEN 30
40 U = PEEK(PARAMREG)
50 PRINT "UNITS:";U
```

#### ROM / higher‑level usage

The ROM driver uses this internally when building SmartPort DIBs and reporting device count to ProDOS. From other languages you typically query via ProDOS’s `STATUS` or SmartPort, rather than calling `CMD_GETDEVSTATUS` directly.

---

### 2.2 Get unit status (`CMD_GETUNITSTATUS = $12`)

**Purpose**: Per‑unit status, including **block count**, online state, and medium type.

#### Assembly (get 16‑bit block count)

```asm
        ; A = unit number on entry
        STA paramreg

        LDA #CMD_GETUNITSTATUS
        STA cmdreg
@wait:  BIT statusreg
        BMI @wait
        BIT statusreg
        BVS @error

        ; First two bytes of paramreg: block count (lo, hi)
        LDA paramreg        ; low byte
        LDX paramreg        ; high byte
        ; X:A = block count
        RTS

@error: LDA #0
        LDX #0
        RTS
```

#### Applesoft

```basic
100 CMDREG=49344:PARAMREG=49345
110 REM Get block count for unit 4 (low 16 bits)
120 U = 4
130 POKE PARAMREG,U
140 POKE CMDREG,18          : REM CMD_GETUNITSTATUS
150 IF PEEK(CMDREG)>=128 THEN 150
160 IF PEEK(CMDREG)>=64 THEN PRINT "ERROR":END

170 LO = PEEK(PARAMREG)
180 HI = PEEK(PARAMREG)
190 BC = LO + 256*HI
200 PRINT "UNIT";U;"BLOCKS (16‑bit):";BC
```

#### ROM mechanism

The firmware’s SmartPort/ProDOS driver calls `CMD_GETUNITSTATUS` under the hood whenever the OS asks for device or unit status (e.g. during `CAT`); from other environments you can either:

- Use the standard SmartPort/ProDOS `STATUS` call, or
- Call `CMD_GETUNITSTATUS` as shown above and parse the returned bytes.

---

### 2.3 Get device information block (`CMD_GETDIB = $13`)

**Purpose**: Return a SmartPort‑style **Device Information Block (DIB)** describing a unit or controller.

#### Assembly

```asm
        ; A = unit number (or 0 for controller, depending on firmware)
        STA paramreg

        LDA #CMD_GETDIB
        STA cmdreg
@wait:  BIT statusreg
        BMI @wait
        BIT statusreg
        BVS @error

        ; 25‑byte DIB follows in paramreg, copy out to your buffer
        LDY #0
@loop:  LDA paramreg
        STA dibbuf,Y
        INY
        CPY #25
        BNE @loop
        RTS
```

#### Applesoft

A full DIB parser is verbose in Applesoft; for most programs it’s easier to rely on ProDOS’s interpretation. If you still want direct access, you can pattern‑match the assembly sequence with `PEEK(PARAMREG)` in a loop similar to the `GetVolInfo` example below.

#### ROM mechanism

The patched SmartPort driver builds its DIB from the data returned here. From any language that can issue SmartPort calls, you can retrieve the DIB via the standard SmartPort `STATUS` command (`SP_STATUS` with code $00/$01), without touching C0C0 directly.

---

### 2.4 Get volume info (`CMD_GETVOLINFO = $14`)

**Purpose**: Query a unit’s volume type, block count, medium, and volume name.

#### Assembly

```asm
; On entry:
;   A/X = pointer to destination VolInfo struct (21 bytes)
;
_GetVolInfo:
        STA @stainst+1
        STX @stainst+2

        STZ cmdreg                 ; reset pointers

        JSR popa                   ; unitNum from caller
        STA paramreg

        LDX #0
        LDA #CMD_GETVOLINFO
        JSR execute
        BVS @error

        ; Copy 21 bytes from paramreg to dest
        LDY #0
@loop:  LDA paramreg
@stainst:
        STA $FFFF,Y                ; patched to dest
        INY
        CPY #21
        BNE @loop

        LDA #1                     ; success
        RTS

@error: LDA #0
        RTS
```

#### Applesoft (print volume name)

```basic
100 CMDREG=49344:PARAMREG=49345
110 U = 4
120 POKE CMDREG,0          : REM CMD_RESETBOTHPTRS
130 POKE PARAMREG,U        : REM unitNum
140 POKE CMDREG,20         : REM CMD_GETVOLINFO
150 IF PEEK(CMDREG)>=128 THEN 150

160 REM VolInfo: type, blockLo, blockHi, medium, nameLen, name...
170 T  = PEEK(PARAMREG)          : REM type
180 BL = PEEK(PARAMREG)          : REM block low
190 BH = PEEK(PARAMREG)          : REM block high
200 M  = PEEK(PARAMREG)          : REM medium
210 NL = PEEK(PARAMREG)          : REM name length
220 N$ = ""
230 FOR I=0 TO NL-1
240   N$ = N$ + CHR$(PEEK(PARAMREG))
250 NEXT
260 PRINT "UNIT";U;"TYPE";T;"MED";M;"NAME";N$
```

#### ROM mechanism

Control‑panel code and some SmartPort status requests use this to present human‑readable information. Other languages can either:

- Use ProDOS `STAT`/`CAT` to show volume info, or
- Call `CMD_GETVOLINFO` directly and parse the struct as above.

---

### 2.5 Block I/O (`CMD_READBLOCK = $15`, `CMD_WRITEBLOCK = $16`)

**Purpose**: Low‑level 512‑byte block read/write.

These are the primitives used by the ProDOS/SmartPort driver; normally you go through ProDOS’s block device API, not these directly.

#### Assembly – read one block

```asm
; In:  A = unit number, Y/X = 24‑bit block number (you choose mapping)
; Out: 512 bytes streamed from datareg

        STZ cmdreg

        STA paramreg              ; unit
        TYA
        STA paramreg              ; block low
        TXA
        STA paramreg              ; block mid
        LDA #0
        STA paramreg              ; block high (if needed)

        LDA #CMD_READBLOCK
        STA cmdreg
@wait:  BIT statusreg
        BMI @wait
        BIT statusreg
        BVS @error

        LDY #0
@loop:  LDA datareg
        STA (bufptr),Y
        INY
        BNE @loop
        INC bufptr+1              ; next page, repeat until 512 bytes read
        ; (omitted for brevity)
```

#### Applesoft – read block 0 of unit 4 into $0800

```basic
300 CMDREG=49344:PARAMREG=49345:DATAREG=49346
310 BUF = 2048                   : REM $0800

320 POKE CMDREG,0                : REM reset pointers
330 POKE PARAMREG,4              : REM unit 4
340 POKE PARAMREG,0              : REM block low
350 POKE PARAMREG,0              : REM block mid
360 POKE PARAMREG,0              : REM block high

370 POKE CMDREG,21               : REM CMD_READBLOCK
380 IF PEEK(CMDREG)>=128 THEN 380

390 FOR I=0 TO 511
400   POKE BUF+I,PEEK(DATAREG)
410 NEXT I
420 PRINT "BLOCK 0 READ INTO $0800"
```

#### ROM mechanism

The patched SmartPort / ProDOS driver calls these from within `readblock` / `writeblock` routines; from other environments:

- Use **ProDOS MLI** `READ_BLOCK` / `WRITE_BLOCK`, or
- Use SmartPort commands `SP_READBLOCK` / `SP_WRITEBLOCK`, and let the firmware route them to these opcodes internally.

Direct use of `CMD_READBLOCK`/`CMD_WRITEBLOCK` from high‑level code is rarely needed.

---

## 3. Time and timers

### 3.1 ProDOS clock driver (ROM‑patched)

**High‑level interface (recommended):**

- Under ProDOS, just call the standard clock driver via **MLI**:
  - `GET_TIME` (`$81`)
  - `SET_TIME` (`$80`)

MegaFlash patches ProDOS’s clock vector so those calls go through the firmware’s `clockdriver`, which uses the Pico’s RTC.

**Low‑level mechanism:**

- Entry point `clockdriver` lives in the slot‑4 ROM area and is registered into ProDOS’s clock vector during coldstart (see `firmware/megaflash.s` around `clockdriver` and installation code).
- It ultimately issues:
  - `CMD_GETPRODOSTIME` ($17) / `CMD_GETPRODOS25TIME` ($18)
  - `CMD_SETRTC_PRODOS` ($1A)  / `CMD_SETRTC_PRODOS25` ($1B)
  over the C0C0 protocol and converts between ProDOS’s time/date format and the Pico’s.

**From other languages:** implement the ProDOS clock driver ABI and call these four commands as needed, or reuse the ROM clockdriver entry via its documented entry address.

---

### 3.2 Human‑readable time (`CMD_GETTIMESTR = $19`)

**Purpose**: 8‑byte ASCII time string (`HH:MM AM` style) used for the on‑screen clock.

#### Assembly

```asm
        LDA #CMD_GETTIMESTR
        JSR execute
        BVS @error

        LDX #0
@loop:  LDA paramreg
        STA timestr,X
        INX
        CPX #8
        BNE @loop
        RTS
```

#### Applesoft

```basic
500 CMDREG=49344:PARAMREG=49345
510 POKE CMDREG,25                 : REM CMD_GETTIMESTR
520 IF PEEK(CMDREG)>=128 THEN 520
530 T$ = ""
540 FOR I=0 TO 7
550   T$ = T$ + CHR$(PEEK(PARAMREG))
560 NEXT I
570 PRINT "TIME:";T$
```

#### ROM mechanism

The Control Panel’s `_DisplayTime` routine uses `CMD_GETFIRMWAREVER` and `CMD_GETTIMESTR` to paint version + time into screen RAM (`$7D0+20..39`). Programs can bypass that and draw their own clock using this command.

---

### 3.3 High‑resolution timers (`CMD_RESETTIMER_*`, `CMD_GETTIMER_*`)

Commands:

- `CMD_RESETTIMER_US = $40`
- `CMD_GETTIMER_US   = $41`
- `CMD_RESETTIMER_MS = $42`
- `CMD_GETTIMER_MS   = $43`
- `CMD_RESETTIMER_S  = $44`
- `CMD_GETTIMER_S    = $45`

**Purpose**: Start and read Pico‑maintained timers in microseconds, milliseconds, or seconds.

#### Assembly (microseconds, low 32 bits)

```asm
; Reset timer
        LDA #CMD_RESETTIMER_US
        STA cmdreg
@w1:    BIT statusreg
        BMI @w1

; Later: read timer
        LDA #CMD_GETTIMER_US
        STA cmdreg
@w2:    BIT statusreg
        BMI @w2

        LDA paramreg        ; byte 0 (LSB)
        LDX paramreg        ; byte 1
        LDY paramreg        ; byte 2
        ; next byte via another load from paramreg, etc.
```

#### Applesoft (16‑bit microsecond counter example)

```basic
600 CMDREG=49344:PARAMREG=49345

610 POKE CMDREG,64                 : REM CMD_RESETTIMER_US
620 IF PEEK(CMDREG)>=128 THEN 620

630 REM ... code to be timed ...

640 POKE CMDREG,65                 : REM CMD_GETTIMER_US
650 IF PEEK(CMDREG)>=128 THEN 650

660 LO = PEEK(PARAMREG)
670 HI = PEEK(PARAMREG)
680 T  = LO + 256*HI               : REM lower 16 bits of elapsed microseconds
690 PRINT "ELAPSED US (LOW 16 BITS):";T
```

#### ROM mechanism

These timers are not directly wired into ROM; they are utility commands for higher‑level code (e.g. diagnostics, profiling) and must be called via C0C0 as shown.

---

## 4. FPU and math

### 4.1 Applesoft FPU integration (ROM‑patched)

**From Applesoft BASIC:**

You write normal code:

```basic
10 A = SIN(1)
20 B = COS(0.5)
30 C = TAN(0.5)
40 D = ATN(1)
50 E = LOG(10)
60 F = EXP(1)
70 G = SQR(2)
80 PRINT A,B,C,D,E,F,G
```

If MegaFlash’s firmware was built with `FPUSUPPORT` and enabled, the ROM:

- Patches Applesoft’s math entry points (`FADD`, `FMUL`, `FDIV`, `FSIN`, `FCOS`, `FTAN`, `FATN`, `FLOG`, `FEXP`, `FSQR`, `FOUT`).
- Each stub JSRs into `fpu_exec`, which:
  - Marshals Applesoft FAC/ARG into a C0C0 command using:
    - `CMD_FADD`  ($30)
    - `CMD_FMUL`  ($31)
    - `CMD_FDIV`  ($32)
    - `CMD_FSIN`  ($33)
    - `CMD_FCOS`  ($34)
    - `CMD_FTAN`  ($35)
    - `CMD_FATN`  ($36)
    - `CMD_FLOG`  ($37)
    - `CMD_FEXP`  ($38)
    - `CMD_FSQR`  ($39)
    - `CMD_FOUT`  ($3A)
    - `CMD_FMUL10`($3B)
    - `CMD_FDIV10`($3C)
  - Issues the command via `cmdreg/paramreg/datareg`.
  - On success, writes the result back into the FAC and returns to Applesoft.
  - On error, falls back to the original Applesoft routine.

**From other languages:**

- You can:
  - Call the patched Applesoft math entry points by their ROM addresses, or
  - Reimplement the marshalling used in `firmware/fpu.s` and talk to the FPU using the C0C0 commands listed above.

Direct Applesoft‑level use of the raw C0C0 FPU opcodes is not practical without duplicating the FAC/ARG encoding logic.

---

## 5. Network and TFTP

### 5.1 Uthernet II emulation (W5100 at $C0C4–$C0C7)

**Interface:**

- MegaFlash exposes a Uthernet II‑compatible device at:
  - Slot 4 C0x4–C0x7 → `$C0C4–$C0C7`.
- From the Apple IIc’s point of view, you talk to it exactly as you would a real Uthernet II:
  - Socket registers, TX/RX buffer windows, etc.

**ROM mechanism:**

- The Pico firmware maps $C0C4–$C0C7 to a W5100 emulation layer (`U2_HandleBusAccess` / `U2_Poll` in the Pico code), using lwIP and the CYW43 WiFi driver.

**From other languages:**

- Use any Uthernet II‑aware TCP/IP stack or driver, pointed at **slot 4**.
- Applesoft usually interacts with Uthernet II via an IP/TCP library; MegaFlash drops in under that library with compatible behavior.

---

### 5.2 Test WiFi (`CMD_TESTWIFI = $09`)

**Purpose**: Run a Pico WiFi self‑test.

#### Assembly

```asm
        STZ cmdreg
        LDA #WE_KEY
        STA paramreg

        LDA #CMD_TESTWIFI
        JSR execute

        LDA paramreg        ; result code (0 = OK)
        RTS
```

#### Applesoft

```basic
800 CMDREG=49344:PARAMREG=49345
810 POKE CMDREG,0              : REM reset pointers
820 POKE PARAMREG,113          : REM WE_KEY = $71
830 POKE CMDREG,9              : REM CMD_TESTWIFI
840 IF PEEK(CMDREG)>=128 THEN 840
850 ERR = PEEK(PARAMREG)
860 PRINT "TEST WIFI ERR=";ERR
```

#### ROM mechanism

The Control Panel calls this to provide “Test WiFi” functionality. Other languages can call it directly as shown.

---

### 5.3 TFTP control (`CMD_TFTPRUN = $50`, `CMD_TFTPSTATUS = $51`)

These are primarily driven by the **Control Panel**, but can be called from other languages.

**High‑level behavior:**

- `CMD_TFTPRUN` starts an upload or download of ProDOS blocks to/from a TFTP server.
- `CMD_TFTPSTATUS` returns status/progress and error codes.

**Assembly outline for `CMD_TFTPRUN`:**

```asm
; Precondition: hostname and filename already copied into datareg
; (using a routine similar to _CopyStringToDataBuffer).
;
; A  = unitNum
; stack: ... flag, dir, unitNum (fastcall layout)

_StartTFTP:
        JSR resetBufferPointer

        STA paramreg         ; unitNum

        JSR popa
        STA paramreg         ; dir (0=download, 1=upload)

        JSR popa
        STA paramreg         ; flag (bit0: save hostname)

        LDA #WE_KEY
        STA paramreg

        LDA #CMD_TFTPRUN
        JSR execute

        LDA statusreg
        AND #ERRORCODEFIELD  ; TFTP error code
        RTS
```

**Applesoft concept:**

In practice, you would:

1. Use an ML helper to write hostname and filename into `datareg`.
2. Write `unitNum`, `dir`, `flag`, `WE_KEY` into `paramreg`.
3. POKE `CMDREG,80` and wait for completion.
4. Read `PEEK(CMDREG) AND 31` for the error code.

Because of the need to pack multiple strings and structured parameters, TFTP control is best exposed to Applesoft via a dedicated `USR()`‑based ML routine, not raw PEEK/POKE from BASIC.

**ROM mechanism:**

- The Control Panel’s TFTP tasks (`tftptask.*`) use these commands to implement menu‑driven upload/download.
- Other environments can either:
  - Call those ML routines directly, or
  - Re‑implement the above parameter packing on top of the C0C0 interface.

---

## 6. Settings and configuration

Commands:

- `CMD_SAVEUSERSETTINGS`   = $20
- `CMD_GETUSERSETTINGS`    = $21
- `CMD_SAVEWIFISETTINGS`   = $22
- `CMD_GETCONFIGBYTES`     = $23
- `CMD_ERASEUSERSETTINGS`  = $24
- `CMD_ERASEWIFISETTINGS`  = $25
- `CMD_ERASEADVSETTINGS`   = $26
- `CMD_ERASEALLSETTINGS`   = $27
- `CMD_DRIVEMAPPING`       = $28
- `CMD_GETFIRMWAREVER`     = $29

### 6.1 Get firmware version (`CMD_GETFIRMWAREVER = $29`)

#### Assembly

```asm
        LDA #CMD_GETFIRMWAREVER
        JSR execute
        BVS @error

        LDX #0
@loop:  LDA paramreg
        STA verstr,X
        INX
        CPX #12
        BNE @loop
        RTS
```

#### Applesoft

```basic
900 CMDREG=49344:PARAMREG=49345
910 POKE CMDREG,41              : REM CMD_GETFIRMWAREVER
920 IF PEEK(CMDREG)>=128 THEN 920
930 V$ = ""
940 FOR I=0 TO 11
950   V$ = V$ + CHR$(PEEK(PARAMREG))
960 NEXT I
970 PRINT "FIRMWARE:";V$
```

### 6.2 Drive mapping (`CMD_DRIVEMAPPING = $28`)

**Purpose**: Enable or disable MegaFlash’s drive‑mapping logic.

#### Assembly

```asm
; In: A = enable flag (0=off, non‑zero=on)
_DriveMapping:
        STZ cmdreg

        LDY #WE_KEY
        STA paramreg            ; enable flag

        LDA #CMD_DRIVEMAPPING
        STY paramreg            ; WE_KEY
        JSR execute
        RTS
```

#### Applesoft

```basic
1000 CMDREG=49344:PARAMREG=49345
1010 REM Enable drive mapping
1020 POKE CMDREG,0              : REM reset
1030 POKE PARAMREG,1            : REM enable = 1
1040 POKE PARAMREG,113          : REM WE_KEY
1050 POKE CMDREG,40             : REM CMD_DRIVEMAPPING (decimal)
1060 IF PEEK(CMDREG)>=128 THEN 1060
```

### 6.3 Erase all settings (`CMD_ERASEALLSETTINGS = $27`)

#### Assembly

```asm
_EraseAllSettings:
        STZ cmdreg
        LDA #WE_KEY
        STA paramreg
        LDA #CMD_ERASEALLSETTINGS
        JSR execute
        RTS                     ; assumes success
```

#### Applesoft

```basic
1100 CMDREG=49344:PARAMREG=49345
1110 POKE CMDREG,0
1120 POKE PARAMREG,113          : REM WE_KEY
1130 POKE CMDREG,39             : REM CMD_ERASEALLSETTINGS
1140 IF PEEK(CMDREG)>=128 THEN 1140
1150 PRINT "ALL SETTINGS ERASED"
```

---

## 7. ROM disk control

Commands:

- `CMD_ENABLEROMDISK`   = $06
- `CMD_DISABLEROMDISK`  = $07

### 7.1 Enable ROM disk (last or first)

#### Assembly

```asm
; Enable ROM disk at last SmartPort unit (param=0) or first (param=1)

_EnableRomdiskAtLast:
        STZ cmdreg
        STZ paramreg            ; 0 = last
        LDA #CMD_ENABLEROMDISK
        JSR execute
        RTS

_BootToRomdisk:
        STZ cmdreg
        LDA #1
        STA paramreg            ; 1 = first unit (for boot)
        LDA #CMD_ENABLEROMDISK
        JSR execute
        JSR Reboot              ; no return
```

#### Applesoft – enable last

```basic
1200 CMDREG=49344:PARAMREG=49345
1210 POKE CMDREG,0
1220 POKE PARAMREG,0            : REM 0 = last unit
1230 POKE CMDREG,6              : REM CMD_ENABLEROMDISK
1240 IF PEEK(CMDREG)>=128 THEN 1240
```

#### Applesoft – enable first (then reboot via monitor)

```basic
1300 CMDREG=49344:PARAMREG=49345
1310 POKE CMDREG,0
1320 POKE PARAMREG,1            : REM 1 = first
1330 POKE CMDREG,6
1340 IF PEEK(CMDREG)>=128 THEN 1340
1350 PRINT "RESET TO BOOT FROM ROM DISK"
1360 CALL -151                  : REM enter monitor, user types 3D0G or equivalent
```

### 7.2 Disable ROM disk

#### Assembly

```asm
_DisableRomdisk:
        STZ cmdreg
        LDA #CMD_DISABLEROMDISK
        JSR execute
        RTS
```

#### Applesoft

```basic
1400 CMDREG=49344
1410 POKE CMDREG,7              : REM CMD_DISABLEROMDISK
1420 IF PEEK(CMDREG)>=128 THEN 1420
```

#### ROM mechanism

The Control Panel uses these commands to toggle the ROM disk and record preference in settings. Other languages can either:

- Call the ML wrappers (`_EnableRomdiskAtLast`, `_BootToRomdisk`, `_DisableRomdisk`), or
- Use the raw command sequences above.

---

## 8. “Plain Apple II” helpers and ROM hooks

These features are **already patched into the ROM**; from Applesoft or ProDOS they appear as normal system behavior:

- **Block devices**:
  - ProDOS sees MegaFlash volumes as ordinary SmartPort block devices.
  - You use standard ProDOS MLI calls (`READ_BLOCK`, `WRITE_BLOCK`, `STATUS`, etc.).
  - Under the hood, the driver calls `CMD_READBLOCK`/`CMD_WRITEBLOCK`/`CMD_GET*STATUS`.

- **Clock driver**:
  - ProDOS `GET_TIME` / `SET_TIME` are wired to MegaFlash’s `clockdriver`.
  - That uses `CMD_GETPRODOSTIME` / `CMD_SETRTC_PRODOS` and friends.

- **Applesoft FPU**:
  - `SIN`, `COS`, `TAN`, `ATN`, `LOG`, `EXP`, `SQR`, arithmetic, and `FOUT` are patched to call the Pico FPU commands (`CMD_F*`).
  - If FPU is disabled or an error occurs, control falls back to original Applesoft code.

From **other languages** (Pascal, Forth, custom 6502 code), you can:

- Use the **standard OS ABIs** (ProDOS MLI, SmartPort, Applesoft entry points), or
- Bypass them and use the **C0C0 protocol** directly as documented above.

