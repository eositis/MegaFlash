;
; This files is shared with firmware and cpanel projects  
;


;
;Peripheral Registers
;
cmdreg          := $c0c0        ;Command Register
statusreg       := $c0c0        ;Status Register
paramreg        := $c0c1        ;Parameter Register
datareg         := $c0c2        ;Data Register
idreg           := $c0c3        ;ID Register

;
;MegaFlash Command/Constant
;
CMD_RESETBOTHPTRS       =       $00
CMD_RESETDATAPTR        =       $01
CMD_RESETPARAMPTR       =       $02
CMD_MODELINEAR          =       $03
CMD_MODEINTERLEAVED     =       $04
CMD_COLDSTART           =       $05
CMD_ENABLEROMDISK       =       $06
CMD_DISABLEROMDISK      =       $07
CMD_LOAD_CPANEL         =       $08
CMD_TESTWIFI            =       $09

CMD_RESETTIMER_US       =       $0A
CMD_GETTIMER_US         =       $0B
CMD_RESETTIMER_MS       =       $0C
CMD_GETTIMER_MS         =       $0D
CMD_RESETTIMER_S        =       $0E
CMD_GETTIMER_S          =       $0F

CMD_GETDEVINFO          =       $10
CMD_GETDEVSTATUS        =       $11
CMD_GETUNITSTATUS       =       $12
CMD_GETDIB              =       $13
CMD_GETVOLINFO          =       $14
CMD_READBLOCK           =       $15
CMD_WRITEBLOCK          =       $16
CMD_GETPRODOSTIME       =       $17
CMD_GETPRODOS25TIME     =       $18
CMD_GETTIMESTR          =       $19
CMD_SETRTC_PRODOS       =       $1A
CMD_SETRTC_PRODOS25     =       $1B
CMD_WRITEBLOCKSIZETOVDH =       $1C
CMD_FORMATDISK          =       $1D
   
     
CMD_SAVEUSERCONFIG      =       $20
CMD_GETUSERCONFIG       =       $21
CMD_SAVEWIFISETTING     =       $22
CMD_GETCONFIGBYTES      =       $23
CMD_ERASEUSERCONFIG     =       $24
CMD_ERASEWIFISETTING    =       $25        
CMD_ERASEALLCONFIG      =       $26  
        
CMD_FADD                =       $30 
CMD_FMUL                =       $31
CMD_FDIV                =       $32
CMD_FSIN                =       $33 
CMD_FCOS                =       $34
CMD_FTAN                =       $35
CMD_FATN                =       $36
CMD_FLOG                =       $37
CMD_FEXP                =       $38
CMD_FSQR                =       $39
CMD_FOUT                =       $3A
CMD_FMUL10              =       $3B
CMD_FDIV10              =       $3C


WE_KEY                  =       $71     ;Write Enable Key
SIGNATURE1              =       $88     ;MegaFlash Device Signature Byte #1
SIGNATURE2              =       $74     ;MegaFlash Device Signature Byte #2

;
;Status Registers Fields
;
BUSYFLAG                = %10000000
ERRORFLAG               = %01000000
ERRORCODEFIELD          = %00001111 ;Last four bits are error code field


;
;MegaFlash Activation Magic Address Sequence
;
MAGIC1                  =       $C0C2
MAGIC2                  =       $C0C0
MAGIC3                  =       $C0C0
MAGIC4                  =       $C0C3
MAGIC5                  =       $C0C1

;
; Bits definition of configbyte1
;
CPUSPEEDFLAG            =       %10000000
AUTOBOOTFLAG            =       %01000000
RAMDISKFLAG             =       %00100000
NTPCLIENTFLAG           =       %00010000
FPUFLAG                 =       %00001000