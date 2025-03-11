.include "via.s"
.include "acia.s"
.include "vt100.s"

COMMANDS_INIT:
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

CLEAR_SCR:
    JSR clear_terminal
    RTS

CHECK_KEY_NAV:
    CMP #$1E
    BEQ up_arrow
    CMP #39
    BEQ right_arrow
    RTS
up_arrow:
    JSR cursor_up
    LDA #'U'
    JSR print_char_lcd
    JMP clear_key
right_arrow:
    JSR cursor_right
    LDA #'R'
    JSR print_char_lcd
clear_key:
    CLC
    RTS
