.include "via.s"

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

CLEAR_SCR:
    JSR CLEAR_TERMINAL
    RTS

RETURN_BOOT:
    JMP BOOT
