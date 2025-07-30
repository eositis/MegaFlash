                .include        "apple2.inc"

                ;
                ; Constants Definitions
                ; The .inc file is shared with Firmware Project
                ;                
                .include "../common/defines.inc"
                
                .code
                .importzp sreg,sp
                .importzp tmp1,tmp2,tmp3,tmp4
                .importzp ptr1,ptr2,ptr3,ptr4
                .import popa,incsp2
                

                .export _SendCommand,_GetInfoString,_GetUnitCount,_EraseDisk,_FormatDisk,_GetVolInfo
                .export _TestWifi,_EraseAllSettings,_GetUnitBlockCount,_DriveMapping,_DisplayTime
                .export _SaveSetting,_LoadSetting,_PrintStringFromDataBuffer
                .export _CopyStringToDataBuffer,_CopyStringFromDataBuffer
                .export _StartTFTP,_GetTFTPStatus
                .export _GetParam8Offset,_GetParam8,_GetParam16



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
; Subroutine to reset MegaFlash param and data buffer pointer
; Note: The rts instruction acts a delay for MegaFlash to
; complete the command
;
resetBufferPointer:
                stz cmdreg
                rts



;///////////////////////////////////////////////////////// 
; void __fastcall__ SendCommand(uint8_t cmd)
; Send a command to MegaFlash and wait until it is executed.
;
; Input: cmd - Command to be sent
;
_SendCommand:=execute           ;same implementation as execute


;//////////////////////////////////////////////////////////
; bool __fastcall__ GetInfoString(uint8_t type) 
; Send CMD_GETINFOSTR to get Information string
; The result is returned in data buffer
;
; Input: type - Info String Type
;
; Output: bool - success
;
_GetInfoString:
                stz cmdreg      ;reset buffer pointer
                ldx #0          ;preload =0
                
                ;store type into parameter Buffer
                sta paramreg
                
                lda #CMD_GETINFOSTR
                jsr execute
                bvs @error
                lda #1          ;a=1, x=0 (true)
                rts
@error:         txa             ;a=0, x=0 (false)
                rts

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
                ;return 9 for testing
                lda #9
                ldx #0
                rts
.endif

;//////////////////////////////////////////////////////////
; uint8_t __fastcall__ EraseDisk();
; Parameter is passed by global variable
;   fmt_selectedUnit - uint8_t Unit to be erased
;
_EraseDisk:
.ifndef TESTBUILD
                stz cmdreg              ;Reset buffers pointer
                
                lda _fmt_selectedUnit
                sta paramreg
                
                lda #WE_KEY             ;Write Enable Key
                sta paramreg      

                lda #CMD_ERASEDISK
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
; uint8_t __fastcall__ FormatDisk();
; Parameters are passed by global variables
;   fmt_selectedUnit - uint8_t Unit to be formatted
;   fmt_blockCount   - uint16_t Number of bslocks
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
                ;Write dest pointer. Self-Modifying code
                sta @stainst+1
                stx @stainst+2

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
@stainst:       sta $ffff,y     ;sta dest,y
                iny
                cpy #21         ;size of VolInfo_t
                bne :-
                
                ;return 1
                lda #1  ;return a=1, x=0
                rts

@error:         ;return 0
                txa     ;return x=0, a=0
                rts     
.else
                sta @stainst+1
                stx @stainst+2
                
                ;pop unitNum from stack
                jsr popa
                
                ;Copy 21 bytes from @testdata to dest
                ldy #20
:               lda @testdata,y
@stainst:       sta $ffff,y     ;sta dest,y
                dey
                bpl :-
                
                ;return 1
                lda #1
                ldx #0
                rts
                
@testdata:
                .byte 0         ;type = ProDOS
                .word $FFFF     ;blockCount
                .byte 0         ;Medium = Flash
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
; void __fastcall__ EraseAllSettings()
; Send CMD_ERASEALLSETTINGS command to MegaFlash
; Assume no error would occur.
; 
_EraseAllSettings:
.ifndef TESTBUILD
                ;Reset buffer pointers    
                stz cmdreg          
                
                ;Put Write Enable Key to parameter buffer
                lda #WE_KEY
                sta paramreg          

                lda #CMD_ERASEALLSETTINGS
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
                jsr resetBufferPointer

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
.else
                ;return $ffff
                lda #$ff
                tax
                rts
.endif

;/////////////////////////////////////////////////////////
; void __fastcall__ DriveMapping(bool enable)
; Enable/Disable Drive Mapping
;
_DriveMapping:
.ifndef TESTBUILD
                ;Reset buffer pointers    
                stz cmdreg
                
                ldy #WE_KEY             ;Preload Y=WE_KEY
                sta paramreg            ;Enable Flag

                lda #CMD_DRIVEMAPPING   ;Preload A=CMD_DRIVEMAPPING
                sty paramreg            ;Write Enable Key
                jmp execute
                
.else
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
@loop:          lda $FFFF,y     ;lda src,y
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
                jsr incsp2      ;Remove arguments from stack

                ;return $0001
                lda #$01
                ldx #$00
                rts
.endif        
        
;/////////////////////////////////////////////////////////
; bool __fastcall__ LoadSetting(uint8_t cmd, uint8_t len, void* dest)
; Loading User Settings from MegaFlash
;
; Input:  cmd  - MegaFlash command to load the setting
;         len  - length of the data structure
;         dest - pointer to data
;
; Output: bool - success
;
_LoadSetting:   
.ifndef TESTBUILD
                ;write dest pointer, self-modifying code
                sta @stainst+1
                stx @stainst+2
                
                jsr popa        ; Get len from C-Stack
                pha             ; Save len to stack        
                
                ;Execute the Load command
                jsr popa        ; Get command from C-Stack      
                jsr execute     
                plx             ; Get len from stack
                bvs @error
 
                ;Copy data from data buffer, x is the loop counter
                ldy #0
@loop:          lda datareg
@stainst:       sta $ffff,y     ;sta dest,y
                iny
                dex
                bne @loop
                ;return 1
                lda #1
                ldx #0
                rts
@error:         ;return 0
                lda #0
                tax
                rts
.else
                jsr incsp2      ;Remove arguments from stack
                ;return 1
                lda #1
                ldx #0
                rts
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
           


;///////////////////////////////////////////////////////// 
; uint8_t __fastcall__ CopyStringToDataBuffer(void* src) 
; Copy a string to MegFlash Data Buffer
; Max length of the string is 255
;
; Input: src - Pointer to source buffer
;
; Output: uint8_t - Length of the string
;                   =0 if the len>255
;
_CopyStringToDataBuffer:
                ;write src pointer, self-modifying code
                sta @loop+1
                stx @loop+2
                
                ldy #0
@loop:          lda $ffff,y     ;lda src,y
                sta datareg
                beq @exit       ;Null Char?
                iny
                bne @loop       ;Avoid dead loop
@exit:          tya             ;a=len
                ldx #0          ;x=0
                rts                


;///////////////////////////////////////////////////////// 
; uint8_t __fastcall__ CopyStringFromDataBuffer(void* dest) 
; Copy a string from MegaFlash Data Buffer
; Max length of the string is 255
;
; Input: dest - Pointer to dest buffer
;
; Output: uint8_t - Length of the string
;
_CopyStringFromDataBuffer:
                ;write dest pointer, self-modifying code
                sta @stainst+1
                stx @stainst+2
                
                ldy #0
@loop:          lda datareg
@stainst:       sta $ffff,y     ;sta dest,y
                beq @exit       ;Null Char?
                iny
                bne @loop       ;Avoid dead loop
@exit:          tya             ;a=len
                ldx #0          ;x=0
                rts


;///////////////////////////////////////////////////////// 
; uint8_t __fastcall__ GetParam8Offset(uint8_t offset);
; Get a 8-bit value from parameter buffer at offset
;
; input: offset   - offset of parameter buffer
;
; Output: uint8_t - value from parameter buffer
;                
_GetParam8Offset:
                tax             ;Move offset to x and update z-flag
                beq @exit       ;Offset = 0?        
@loop:          lda paramreg
                dex
                bne @loop
@exit:          ;fall into _GetParam8      

;///////////////////////////////////////////////////////// 
; uint8_t __fastcall__ GetParam8();
; Get a 8-bit value from parameter buffer
;
; Output: uint8_t - value from parameter buffer
;                
_GetParam8:
                lda paramreg
                ldx #0
                rts

;///////////////////////////////////////////////////////// 
; uint16_t __fastcall__ GetParam8();
; Get a 16-bit value from parameter buffer
;
; Output: uint16_t - value from parameter buffer
;                   
_GetParam16:
                lda paramreg
                ldx paramreg
                rts

;*****************************************************************************
;
;                     TFTP
;
;*****************************************************************************          


;///////////////////////////////////////////////////////// 
; uint8_t __fastcall__ StartTFTP(uint8_t flag,uint8_t dir,uint8_t unitNum)
; Send CMD_TFTPRUN command
; Assume hostname and filename is already in data buffer
;
; Input: unitNum - Unit Number
;        dir     - 0=Download From Server, 1=Upload to Server
;        flag    - Bit0: To save hostname to User Config
;
; Output: uint8_t - Error Code
;
_StartTFTP:
.ifndef TESTBUILD
                ;Reset buffer pointers    
                jsr resetBufferPointer
                
                ;unitNum
                sta paramreg

                ;Direction
                jsr popa
                sta paramreg
                
                ;flag
                jsr popa
                sta paramreg
                
                ;Write Enable Key
                lda #WE_KEY
                sta paramreg
                
                ;Send CMD_TFTPRUN
                lda #CMD_TFTPRUN
                jsr execute             
                
                ;Return Error Code from status register
                lda statusreg
                and #ERRORCODEFIELD
                ldx #0
                rts
.else
                jsr incsp2      ;Remove Arguments from stack        
                lda #0          ;return 0
                tax
                rts
.endif                



;///////////////////////////////////////////////////////// 
; void  __fastcall__ GetTFTPStatus(uint8_t pbMaxValue)
; Get TFTP Status with CMD_TFTPSTATUS command
;
_GetTFTPStatus:
                jsr resetBufferPointer;
                
                stz paramreg            ;version = 0
                stz paramreg            ;reserved = 0
                sta paramreg            ;pbMaxValue
                
                lda #CMD_TFTPSTATUS     
                jmp execute


