

                .include        "apple2.inc"

                .code
                .importzp sreg,sp
                .importzp tmp1,tmp2,tmp3,tmp4
                .importzp ptr1,ptr2,ptr3,ptr4
                .import popa, VTABZ
                .import putchardirect
                .import _ShowClock
                
                .export _GetBoardType,_GetUnitCount,_DisableROMDisk,_FormatDisk,_GetVolInfo
                .export _cgetchar,_cputchar,_fillchar        
                .export _newlinex,_newline,_newline2,_cursordown,_cursorup,_cursorupx,_cursorleft
                .export _SaveSetting,_LoadSetting,_Reboot
                .export _DisplayTime,_IsAppleIIcplus
                .export _TestWifi,_PrintIPAddrFromDataBuffer
                .export _setwndlft,_resetwndlft
                .export _ToUppercase
                .export _EraseAllConfig,_GetUnitBlockCount
                .export _HasFPUSupport

;
; Constants Definitions
; The .inc file is shared with Firmware Project
;                
.include "../common/megaflash_defines.inc"

;
; ROM Entry Point
;
appleii         :=      $FB60   ;Clear screen and show Apple IIc
        

     
;/////////////////////////////////////////////////////////
; uint8_t fastcall GetBoardType()
; Get Pico Board type as defined in BoardType enum
;
; Output: Board Type
;     
_GetBoardType:  lda #CMD_GETDEVINFO
                jsr execute
                
                ;Skip 5 bytes from parameter buffer
                ldx #5
:               lda paramreg
                dex
                bne :-
                
                ;Get Board Type
                lda paramreg
                ;x already = 0
                rts

;/////////////////////////////////////////////////////////
; uint8_t fastcall DisableROMDisk()
; Disable MegaFlash ROMDisk
;         
_DisableROMDisk:
                lda #CMD_DISABLEROMDISK
                jmp execute

                
;/////////////////////////////////////////////////////////
; uint8_t fastcall GetUnitCount()
; Get number of units
;
; Output: Unit Count
;             
_GetUnitCount:  
                lda #CMD_GETDEVSTATUS
                jsr execute
                
                lda paramreg    ;Get Unit Count
                ldx #0
                rts        
        
;/////////////////////////////////////////////////////////
; void fastcall Reboot()
; Reboot the machine immediately
;
_Reboot:        bit $c082       ;Switch in ROM
                jsr appleii     ;Show Apple II on screen to give immediate feedback to user
                sta $3f3        ;Destory the PWRUP bytes
                sta $3f4        ;
                jmp ($fffc)     ;Reset!



;/////////////////////////////////////////////////////////
; uint8_t fastcall IsAppleIIcplus()
; return non-zero if running on IIc+ (ROM5)
; return zero if not ROM5
;
_IsAppleIIcplus:
                ldx #0  
                bit $c082       ;Switch in ROM
                lda $fbbf       ;$fbbf = 05 for IIc+
                bit $c080       ;Restore to LC bank 2
                cmp #$05
                beq :+          ;If equal, return a=5, x=0
                txa             ;else return a=0, x=0
:               rts     
        
        
;/////////////////////////////////////////////////////////
; void fastcall cputchar(char c)
; Display character on screen without moving cursor
;
; Input - c char to be displayed
;
; Note: cputc() inverts the high bit before the character
;       is put into screen memory. So, we do the same here.
_cputchar:       eor #$80
                jmp putchardirect

;/////////////////////////////////////////////////////////
; void fastcall fillchar(char c,uint8_t count)
; fill characters from cursor position
; cursor position is not changed.
;
; Input - c     character to be filled
;         count number of characters
;
_fillchar:      pha             ;Push count to stack
                jsr popa        ;a=char c

                plx             ;Restore count to x and update z-flag
                beq rts0        ;Do nothing if count==0

                eor #$80        ;Invert high bit of c            
                ldy CH          ;Cursor Horizontal Position
:               sta (BASL),y
                iny
                dex
                bne :-
rts0:           rts

;/////////////////////////////////////////////////////////
; void __fastcall__ newline();
; void __fastcall__ newline2();
; void __fastcall__ newlinex(uint8_t x);
; void __fastcall__ newliney();
; 
; newline()    - Move cursor to next line, set cursor x postion to 0
; newline2()   - Call newline() twice
; newlinex()   - Move cursor to next line, set cursor x position
; cursordown() - Move cursor to next line, cursor x position unchanged
;
_newline2:      jsr _newline
_newline:       lda #0
_newlinex:      sta CH     
_cursordown:    lda CV
                cmp #23
                bcs rts0
                inc a
                sta CV
                jmp VTABZ

;/////////////////////////////////////////////////////////
;void __fastcall__ cursorup();
;void __fastcall__ cursorupx();
;void __fastcall__ cursorleft();
;
; cursorup()   - Move cursor up, cursor x position unchanged
; cursorupx()  - Move cursor up, set cursor x position 
; cursorleft() - Move cursor left
;
_cursorupx:     sta CH
_cursorup:      lda CV
                beq rts0
                dec a
                sta CV
                jmp VTABZ

_cursorleft:    dec CH
                rts

;/////////////////////////////////////////////////////////
;void __fastcall__ setwndlft(uint8_t x);
;void __fastcall__ resetwndlft();
;
; setwndlft: Set WNDLFT variable
;
; resetwndlft: Reset WNDLFT variable to 0
;
_resetwndlft:   lda #0        
_setwndlft:     sta WNDLFT
                jmp VTABZ
       
        
;/////////////////////////////////////////////////////////
; char fastcall getchar(char cursor)
; Wait for keyboard input and display a blinking cursor
; It also calls ShowClock() regularly to update the clock.
;
; Input - c cursor character
;
; Output - ASCII of key pressed (with high bit cleared)
;
; The cursor alternates between a checkerboard and specified
; cursor character.
_cgetchar:
                prevchar  = tmp1
                whichchar = tmp2        ;The cursor is blinking.
                                        ;It alternates betwen a checkerboard
                                        ;and the cursor character input
                                        ;whichchar = 0 to show cursor char
                                        ;whichchar = 1 to show checkerboard
                cursorchar= tmp3

                eor #$80                ;Invert high bit
                sta cursorchar          ;store the input to cursorchar
                stz whichchar           ;1=show old cursor, 0=show checkerboard

                ; Show caret.
                lda #$7F | $80          ; Checkerboard, screen code
                jsr putchardirect       ; Returns old character in X
                stx prevchar            ; Store in prevchar			

                ;Wait for keyboard strobe while blinking cursor
loop:           ;Delay loop, no need to initalize x register since
                ;it only affects the first run of inner loop
                ldy #80         ;blinking delay
:               lda KBD
                bmi keypressed
                dex
                bne :-          ;inner loop
.ifndef TESTBUILD                
                lda $c09a       ;Touch Serial Port 1 ACIA cmd register ($C09A)
                                ;to slow down on IIc+
.endif                
                dey
                bne :-          ;outer loop
        
                ;Flash Cursor
                lda whichchar        
                eor #$01            ;toggle it
                sta whichchar
        
                beq showchecker     
                lda cursorchar
                bra :+
        
showchecker:    ;Update Clock before show checkerboard char
                jsr _ShowClock

                lda #$7F | $80          ;Checkerboard
:               jsr putchardirect	

                bra loop
        
keypressed:     ; Restore old character.
                pha                     ;save keycode
                lda prevchar            ;Restore original character
                jsr putchardirect

                ; At this time, the high bit of the key pressed is set.
                bit KBDSTRB             ; Clear keyboard strobe
                pla
                and #$7F                ; clear high bit
                ldx #0
                rts
                
;/////////////////////////////////////////////////////////
; void __fastcall__ ToUppercase(char*)
; Convert a string to uppercase
; Lenght of string <256
;                
_ToUppercase:
                sta ptr1
                stx ptr1+1

                ldy #0
@loop:          lda (ptr1),y
                beq rts1
                cmp #'a'
                bcc :+
                cmp #'z'+1
                bcs :+
                and #%11011111
                sta (ptr1),y
:               iny
                bne @loop       ;bne instead of bra to avoid dead-loop
rts1:           rts	


;/////////////////////////////////////////////////////////
; bool fastcall savesetting(uint8_t cmd, uint8_t len, void* src)
; Save User Config and Wifi Setting to MegFlash
; The code of saving user config and setting are the same.
; The only difference is the command code.
;
; Input:  cmd - MegaFlash command to save the setting
;         len - length of the data structure
;         src - pointer to data
;
; Output: bool - success
;
_SaveSetting:   ;save src to ptr1
                sta ptr1
                stx ptr1+1
        
                jsr popa        ; Get len
                tax             ; x = length
 
                ;Switch to linear mode
                lda #CMD_MODELINEAR
                sta cmdreg
                
                ;Reset buffer pointers    
                stz cmdreg          
                
                ;Put Write Enable Key to parameter buffer
                lda #WE_KEY
                sta paramreg
                
                ;Copy data to data buffer, x is the loop counter
                ldy #0
:               lda (ptr1),y
                sta datareg
                iny
                dex
                bne :-
        
                ;Execute the Save command
                jsr popa        ; a = command        
                jsr execute 
                        
                lda #$01        ;Assume success, return $0001
                ldx #$00
                
                bvc noerr
                txa             ;Error, set a to 0
noerr:          rts
        
;/////////////////////////////////////////////////////////
; void fastcall LoadSetting(uint8_t cmd, uint8_t len, void* dest)
; Loading User Config from MegaFlash
; Assume no error would occur.
;
; Input:  cmd  - MegaFlash command to load the setting
;         len  - length of the data structure
;         dest - pointer to data
;
_LoadSetting:   ;save dest to ptr1
                sta ptr1
                stx ptr1+1
        
                jsr popa        ; Get len
                pha             ; Save len to stack        
                
                ;Execute the Load command
                jsr popa        ; a = command        
                jsr execute     
        
                ;Switch to linear mode and reset buffer pointer
                lda #CMD_MODELINEAR
                sta cmdreg      
                       
                ;Copy data from data buffer, x is the loop counter
                plx             ; Get len from stack
                ldy #0
:               lda datareg
                sta (ptr1),y
                iny
                dex
                bne :-
                
                rts
         
       
;/////////////////////////////////////////////////////////
; void fastcall DisplayTime()
; Get Time String from MegaFlash and display
; the time at the bottom right corner of the screen
; All the formatting of the string is performed by Pico.
; The routine just copies the result to screen memory.
;       
_DisplayTime:   lda #CMD_GETTIMESTR
                jsr execute
 
                ;Copy the result to screen memory
                ldx #0
:               lda paramreg
                sta $7D0+32,x   ;x=32, y=23
                inx
                cpx #8
                bne :-
                rts
        
;/////////////////////////////////////////////////////////
; Subroutine to execute MegaFlash command
; Input:  a = command
;
execute:        
.ifndef TESTBUILD
                sta cmdreg
:               bit statusreg   ;wait until the command is executed
                bmi :-    
                rts
.else 
                clv             ;clear error flag
                rts
.endif        

;/////////////////////////////////////////////////////////
; uint8_t __fastcall__ TestWifi()
; Send CMD_TESTWIFI to MegaFlash
;
; Output: uint8_t - errorcode
;  
_TestWifi:      ;Put Write Enable Key to parameter buffer
                stz cmdreg
                lda #WE_KEY
                sta paramreg

                lda #CMD_TESTWIFI
                jsr execute

                lda paramreg    ;Get test result
                ldx #0
                rts
       
;/////////////////////////////////////////////////////////
; void __fastcall__ PrintIPAddrFromDataBuffer()
; Print pre-formatted IP Address from data buffer to screen
;
_PrintIPAddrFromDataBuffer:
.ifndef TESTBUILD
                ldy CH
                ldx #16         ;Max len=16, avoid dead loop
        
:               lda datareg
                beq exit
                sta (BASL),y
                iny
                dex
                bne :-
exit:           rts

.else 
                ;For testing on Emulator
                ldy CH
                ldx #0
        
:               lda ipstr,x
                beq exit
                ora #$80        
                sta (BASL),y
                iny
                inx
                cpx #16         ;Max len=16, avoid dead loop
                bne :-
exit:           rts   
ipstr:          .asciiz "192.168.100.111"       
.endif        

                
                
                
;/////////////////////////////////////////////////////////		
; uint8_t __fastcall__ FormatDisk();
; Paraeter is passed by global variables
;   selectedUnit - Unit to be formatted
;   blockCount   - Number of blocks
;   volName      - Volume Name
;
; Output: uint8_t - ProDOS/SP error code
                .import _selectedUnit,_blockCount,_volName
_FormatDisk:       
                stz cmdreg              ;Reset buffers pointer
                
                lda _selectedUnit
                sta paramreg
                
                lda _blockCount         ;Low Byte
                sta paramreg
                lda _blockCount+1       ;Mid Byte
                sta paramreg
                stz paramreg            ;High Byte
                
                lda #WE_KEY             ;Write Enable Key
                sta paramreg

                ;Volume Name
                ldx #0
:               lda _volName,x
                sta paramreg
                beq :+
                inx
                cpx #16                 ;Avoid Dead loop
                bne :-
                
:               lda #CMD_FORMATDISK
                jsr execute
                
                ;Get the result code from parameter buffer
                lda paramreg
                ldx #0
                rts                
            
;/////////////////////////////////////////////////////////	            
;bool __fastcall__ GetVolInfo(uint8_t unitNum);            
; Get VolInfo struct from MegaFlash
;
; Input: unitNum
                .import _volInfo
_GetVolInfo:
                stz cmdreg;     ;reset buffer pointers
                sta paramreg;
                ldx #0          ;Preload x = 0
                
                lda #CMD_GETVOLINFO
                jsr execute
                bvs @error
                
                ;Copy 21 bytes from parameter buffer to volInfo
                ldy #0
:               lda paramreg
                sta _volInfo,y
                iny
                cpy #21
                bne :-
                
                ;return 1
                lda #1  ;return a=1, x=0
                rts

@error:         ;return 0
                txa     ;return x=0, a=0
                rts
                
;/////////////////////////////////////////////////////////		
; void __fastcall__ EraseAllConfig();    
; Assume no error would occur.
; 
_EraseAllConfig:
                ;Reset buffer pointers    
                stz cmdreg          
                
                ;Put Write Enable Key to parameter buffer
                lda #WE_KEY
                sta paramreg          

                lda #CMD_ERASEALLCONFIG
                jmp execute     ;jsr+rts
           
;/////////////////////////////////////////////////////////           
; uint16_t __fastcall__ GetUnitBlockCount(uint8_t unitNum);
; Get Block Count of a unitNum
;
; return 0 if error 
_GetUnitBlockCount:
                ;Reset buffer pointers    
                stz cmdreg 
                
                ;Put Unit Number to parameter buffer
                sta paramreg
                
                lda #CMD_GETUNITSTATUS
                jsr execute
                bvs @error
                
                ;return block count
                lda paramreg    ;Block Count Low Byte
                ldx paramreg    ;Block Count High Byte
                rts
                
@error:         ;return 0
                lda #0
                tax
                rts
                
                
                
;/////////////////////////////////////////////////////////           
; bool __fastcall__ HasFPUSupport();
; Check if the firmware has FPU support
;
; $C7FF = FPU_SUPPORT_SIGNATURE if FPU Support is enabled
; in the firmware
_HasFPUSupport:
.ifndef TESTBUILD                
                ldx #0          ;Preload X=0, A=0 (false)
                txa             ;
                ldy $C7FF
                cpy #FPU_SUPPORT_SIGNATURE
                bne :+
                inc a           ;X=0, A=1 (true)        
:               rts
.else
                ldx #0
                lda #1
                rts
.endif     
               
               
               