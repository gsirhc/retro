.include "via.s"
.include "acia.s"
.include "vt100.s"

COMMANDS_INIT:
    JSR reset_acia
    RTS

LCDINIT:
    JSR reset_via_irq
    RTS

LCDCLEAR:              ; defined in token.s
    JSR clear_lcd
    RTS

LCDPRINT:              ; defined in token.s
    jsr FRMEVL
    bit VALTYP
    bmi lcd_print
    jsr FOUT
    jsr STRLIT
lcd_print:
    jsr FREFAC
    tax
    ldy #0
lcd_print_loop:
    lda (INDEX),y
    jsr print_char_lcd
    iny
    dex
    bne lcd_print_loop
    rts
    RTS

DEBUG_WOZ:                 ; defined in token.s
    JMP $FE00              ; wozmon
    RTS