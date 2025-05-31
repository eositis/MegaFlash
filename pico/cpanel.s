@ There is no way to embed binary data to C program without conversion.
@ This assembly file is to include the cpanel.bin file as binary data
@ using .incbin directive.
@
@ Two symbols are exported.
@ cpanelData is the location of the data
@ cpanelDataLen is the length of the data


.cpu cortex-m0plus
.thumb

.section .rodata
.global cpanelData
.global cpanelDataLen

@
@ cpanel.bin binary data
@
        .align 4
cpanelData:
        .incbin "../cpanel/cpanel.bin"
cpanelDataEnd:

@
@ length of the data
@
        .align 4
cpanelDataLen:
        .long (cpanelDataEnd-cpanelData)
