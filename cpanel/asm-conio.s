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
                .import VTABZ
                
                .export _cputchar_direct,_fillchar_direct,_cgetchar
                .export _newlinex,_newline,_newline2,_cursordown,_cursorup,_cursorupx,_cursorupx_m1,_cursorleft
                .export _gotoxy00,_clreol,_setwndlft,_resetwndlft


;/////////////////////////////////////////////////////////
; void __fastcall__ cputchar_direct(char c)
; Display character on screen without moving cursor
;
; Input - c char to be displayed
;
; Note: cputc() inverts the high bit before the character
;       is put into screen memory. So, we do the same here.
                .import putchardirect
_cputchar_direct:      
                eor #$80
                jmp putchardirect
                
                
;/////////////////////////////////////////////////////////
; void __fastcall__ fillchar_direct(char c,uint8_t count)
; fill characters from cursor position
; cursor position is not changed.
;
; Input - c     character to be filled
;         count number of characters
;
_fillchar_direct:      
                pha             ;Push count to stack
                jsr popa        ;a=char c

                plx             ;Restore count to x and update z-flag
                beq @rts0       ;Do nothing if count==0

                eor #$80        ;Invert high bit of c            
                ldy CH          ;Cursor Horizontal Position
:               sta (BASL),y
                iny
                dex
                bne :-
@rts0:          rts         


;/////////////////////////////////////////////////////////
; char __fastcall__ getchar(char cursor)
; Wait for keyboard input and display a blinking cursor
; It also calls DisplayTime regularly to update the clock.
;
; Input - c cursor character
;
; Output - ASCII of key pressed (with high bit cleared)
;
; The cursor alternates between a checkerboard and specified
; cursor character.
                .import _DisplayTime
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
                jsr _DisplayTime

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
                bcs rts1
                inc a
                sta CV
                jmp VTABZ

;/////////////////////////////////////////////////////////
;void __fastcall__ cursorup();
;void __fastcall__ cursorupx(uint8_t xpos);
;void __fastcall__ cursorupx_m1(uint8_t xpos);
;void __fastcall__ cursorleft();
;
; cursorup()     - Move cursor up, cursor x position unchanged
; cursorupx()    - Move cursor up, set cursor x position to xpos
; curosrupx_m1() - Move cursor up, set cursor x position to xpos-1
; cursorleft()   - Move cursor left
;
_cursorupx_m1:  dec a
_cursorupx:     sta CH
_cursorup:      lda CV
                beq rts1
                dec a
                sta CV
                jmp VTABZ

_cursorleft:    dec CH
rts1:           rts    

;/////////////////////////////////////////////////////////
;void __fastcall__ gotoxy00();
;equivalent to gotoxy(0,0);
;
_gotoxy00:      stz CH 
                lda WNDTOP
                sta CV
                jmp VTABZ


;/////////////////////////////////////////////////////////
;void __fastcall__ clreol();
;Clear from current cursor position to end of line of
;scroll window
;
_clreol:        
                bit $c082       ;Switch in ROM
                jsr $fc9c       ;clreol routine in Monintor ROM       
                bit $c080       ;Restore to LC bank 2
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
                   