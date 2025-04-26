                .include        "apple2.inc"

                ;
                ; Constants Definitions
                ; The .inc file is shared with Firmware Project
                ;                
                .include "../common/megaflash_defines.inc"
                
                .code
                .importzp sreg,sp
                .importzp tmp1,tmp2,tmp3,tmp4
                .importzp ptr1,ptr2,ptr3,ptr4
                .import popa
                

                .export _SendCommand,_GetBoardType,_DisableROMDisk,_GetUnitCount,_FormatDisk,_GetVolInfo
                .export _TestWifi,_EraseAllConfig,_GetUnitBlockCount,_DisplayTime
                .export _SaveSetting,_LoadSetting,_PrintIPAddrFromDataBuffer,_PrintStringFromDataBuffer

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
; void __fastcall__ SendCommand(uint8_t cmd)
; Send a command to MegaFlash and wait until it is executed.
;
; Input: cmd - Command to be sent
;
_SendCommand:=execute           ;same implementation as execute


;/////////////////////////////////////////////////////////
; uint8_t __fastcall__ GetBoardType()
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
; uint8_t __fastcall__ DisableROMDisk()
; Disable MegaFlash ROMDisk
;         
_DisableROMDisk:
                lda #CMD_DISABLEROMDISK
                jmp execute                
                
                
;/////////////////////////////////////////////////////////
; uint8_t __fastcall__ GetUnitCount()
; Get number of units
;
; Output: Unit Count
;             
_GetUnitCount:  
.ifndef TESTBUILD
                lda #CMD_GETDEVSTATUS
                jsr execute
                
                lda paramreg    ;Get Unit Count
                ldx #0
                rts         
.else
                ;return 9
                lda #9
                ldx #0
                rts
.endif


;/////////////////////////////////////////////////////////		
; uint8_t __fastcall__ FormatDisk();
; Paraeter is passed by global variables
;   fmt_selectedUnit - uint8_t Unit to be formatted
;   fmt_blockCount   - uint16_t Number of blocks
;   fmt_volName      - char[] Volume Name
;
; Output: uint8_t - ProDOS/SP error code
                .import _fmt_selectedUnit,_fmt_blockCount,_fmt_volName
_FormatDisk:       
.ifndef TESTBUILD
                stz cmdreg              ;Reset buffers pointer
                
                lda _fmt_selectedUnit
                sta paramreg
                
                lda _fmt_blockCount     ;Low Byte
                sta paramreg
                lda _fmt_blockCount+1   ;Mid Byte
                sta paramreg
                
                ldx #0                  ;Preload X=0 
                stz paramreg            ;High Byte
                
                lda #WE_KEY             ;Write Enable Key
                sta paramreg

                ;Volume Name
                ;x=0
:               lda _fmt_volName,x
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
.else
                ;return 0 (No error)
                lda #0
                tax
                rts
.endif                
                
;/////////////////////////////////////////////////////////	            
;bool __fastcall__ GetVolInfo(uint8_t unitNum,void* dest);            
; Get VolInfo struct from MegaFlash
; Store the result to _volInfo global variable
;
; Input: unitNum - Unit Number
;        dest - Pointer to struct VolInfo to receive the result
;
_GetVolInfo:
.ifndef TESTBUILD
                ;Save dest pointer to ptr4
                sta ptr4
                stx ptr4+1

                stz cmdreg;     ;reset buffer pointers

                jsr popa        ;Get unitNum
                sta paramreg;   ;

                ldx #0          ;Preload x = 0
                lda #CMD_GETVOLINFO
                jsr execute
                bvs @error
                
                ;Copy 21 bytes from parameter buffer to dest
                ldy #0
:               lda paramreg
                sta (ptr4),y
                iny
                cpy #21
                bne :-
                
                ;return 1
                lda #1  ;return a=1, x=0
                rts

@error:         ;return 0
                txa     ;return x=0, a=0
                rts     
.else
                sta ptr4
                stx ptr4+1
                
                ;pop unitNum from stack
                jsr popa
                
                ;Copy 21 bytes from @testdata to dest
                ldy #20
:               lda @testdata,y
                sta (ptr4),y
                dey
                bpl :-
                
                ;return 1
                lda #1
                ldx #0
                rts
                
@testdata:
                .byte 0         ;type = ProDOS
                .word $FFFF     ;blockCount
                .byte 0         ;Reserved
                .byte 15        ;volNameLen
                .byte "MEGAFLASH012345",$00
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
; void __fastcall__ EraseAllConfig()
; Send CMD_ERASEALLCONFIG command to MegaFlash
; Assume no error would occur.
; 
_EraseAllConfig:
.ifndef TESTBUILD
                ;Reset buffer pointers    
                stz cmdreg          
                
                ;Put Write Enable Key to parameter buffer
                lda #WE_KEY
                sta paramreg          

                lda #CMD_ERASEALLCONFIG
                jmp execute     ;jsr+rts
.else
                rts
.endif
                
;/////////////////////////////////////////////////////////           
; uint16_t __fastcall__ GetUnitBlockCount(uint8_t unitNum);
; Get Block Count of a unitNum
;
; return 0 if error 
_GetUnitBlockCount:
.ifndef TESTBUILD
                ;Reset buffer pointers    
                stz cmdreg 
                ldx #0          ;Preload X=0

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
                txa             ;x already = 0 if error
                rts
.else
                ;return $ffff
                lda #$ff
                tax
                rts
.endif

;/////////////////////////////////////////////////////////
; void __fastcall__ DisplayTime()
; Get Time String from MegaFlash and display
; the time at the bottom right corner of the screen
; All the formatting of the string is performed by Pico.
; The routine just copies the result to screen memory.
;       
_DisplayTime:   
.ifndef TESTBUILD
                lda #CMD_GETTIMESTR
                jsr execute
 
                ;Copy the result to screen memory
                ldx #0
:               lda paramreg
                sta $7D0+32,x   ;x=32, y=23
                inx
                cpx #8
                bne :-
                rts
.else
                ldx #0
:               lda @timstr,x
                ora #$80        ;set high-bit
                sta $7D0+32,x
                inx
                cpx #8
                bne :-
                rts
@timstr:        .byte "11:50 AM"   
.endif                
   

;/////////////////////////////////////////////////////////
; bool __fastcall__ SaveSetting(uint8_t cmd, uint8_t len, void* src)
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
_SaveSetting:   
.ifndef TESTBUILD
                ;write src pointer, self-modifying code
                sta @loop+1     
                stx @loop+2

                ;Switch to linear mode
                lda #CMD_MODELINEAR
                sta cmdreg
        
                ;Get len from stack
                jsr popa        
                tax             ; x = length
                
                ;Reset buffer pointers    
                stz cmdreg          
                
                ;Put Write Enable Key to parameter buffer
                lda #WE_KEY
                sta paramreg
                
                ;Copy data to data buffer, x is the loop counter
                ldy #0
@loop:          lda $FFFF,y
                sta datareg
                iny
                dex
                bne @loop
        
                ;Execute the Save command
                jsr popa        ; a = command        
                jsr execute 
                        
                lda #$01        ;Assume success, return $0001
                ldx #$00
                
                bvc noerr
                txa             ;Error, set a to 0
noerr:          rts
.else
                ;return $0001
                lda #$01
                ldx #$00
                rts
.endif        
        
;/////////////////////////////////////////////////////////
; void __fastcall__ LoadSetting(uint8_t cmd, uint8_t len, void* dest)
; Loading User Config from MegaFlash
; Assume no error would occur.
;
; Input:  cmd  - MegaFlash command to load the setting
;         len  - length of the data structure
;         dest - pointer to data
;
_LoadSetting:   
.ifndef TESTBUILD
                ;write dest pointer, self-modifying code
                sta @stainst+1
                stx @stainst+2
        
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
@loop:          lda datareg
@stainst:       sta $ffff,y
                iny
                dex
                bne @loop
                
                rts
.else
                rts
.endif
            
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
; void __fastcall__ PrintStringFromDataBuffer()
; Pull a string from Data Buffer and print to screen
;
                .import _cputc
_PrintStringFromDataBuffer:
.ifndef TESTBUILD
@loop:          lda datareg
                beq :+          ;Null char?
                jsr _cputc
                bra @loop
:               rts          
.else
                rts
.endif     
           

;*****************************************************************************
;
;                     TFTP
;
;*****************************************************************************          
                .import _tftp_dir,_tftp_unitNum,_tftp_hostname,_tftp_filename
                ;.export _Send_CMD_TFTPRUN
                .export _GetParam8,_GetParam16
;ParamBuffer
; WE_KEY
; Dir (TX or RX)
; UnitNum              
.if 0 
_Send_CMD_TFTPRUN:
.ifndef TESTBUILD
                ;Reset buffer pointers    
                stz cmdreg          
                
                ;Put Write Enable Key to parameter buffer
                lda #WE_KEY
                sta paramreg

                ;Direction
                lda _tftp_dir
                sta paramreg
                
                ;UnitNum
                lda _tftp_unitNum
                sta paramreg
                
                ;Copy Hostname to Data Buffer
                ldx #$FF
:               inx
                lda _tftp_hostname,x
                sta datareg
                bne :-
                
                ;Copy Filename to Data Buffer
                ldx #$FF
:               inx
                lda _tftp_filename,x
                sta datareg
                bne :-         

                ;Send CMD_TFTPRUN
                lda #CMD_TFTPRUN
                jsr execute
                rts
.else
                rts
.endif                
 .endif
;--------------------------------- 
;                
_GetParam8:
                lda paramreg
                ldx #0
                rts
                
_GetParam16:
                lda paramreg
                ldx paramreg
                rts
                          