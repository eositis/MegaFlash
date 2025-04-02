
.include "macros.inc"


;------------------------------------------
;Debug Build
;Default: FALSE
;
;Print Smartport and ProDOS call info to
;serial port
DEBUG 	        EQU     FALSE


;------------------------------------------
;Restore Zero Page Scratch Area
;Default: TRUE
;
; According to ProDOS technical reference, $3A-$3F are used by ProDOS
; as temporary variables and does not need to be restored.
; If this flag is set to TRUE, the memory region is restored
RESTOREZPSCRATCH EQU    TRUE

;--------------------------------------------
;Boot Any Drive with IN#4 command
;Default FALSE
;
;If this flag is set to TRUE, users can boot from
;any drive with IN#4 command. It does not work well 
;with Prodos V2.4 or before. Even ProDOS is booted,
;it can't properly find and execute .SYSTEM file.
;
;Since ProDOS 2.5 is not quite ready, this flag
;is disabled
BOOTANY         EQU     FALSE    ;To boot from drive 3-8, only works on ProDOS 2.5

;------------------------------------------
;Fix Applesoft ONERR GOTO Bug
;Default: TRUE
ONERR_FIX       EQU     TRUE

;------------------------------------------
;Fix Applesoft Integer Conversion Bug
;Default: TRUE
INT_FIX         EQU     TRUE

;------------------------------------------
;Fix Applesoft Slow PRINT Speed on IIc Plus
;running at 4MHz
;Default: TRUE
PRINTSPEED_FIX  EQU     TRUE


;--------------------------------------------
;Applesoft FPU Support
;Default: TRUE
FPUSUPPORT       EQU     FALSE


