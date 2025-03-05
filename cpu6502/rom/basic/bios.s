.setcpu "65C02"
.debuginfo                  ; Generates symbol table
.segment "BIOS"

.include "../hardware/via.asm"
.include "../hardware/acia.asm"

RESET:
  LDA     #$1F           ; 8-N-1, 19200 baud.
  STA     ACIA_CTRL
  LDA     #$0B           ; No parity, no echo, no interrupts.
  STA     ACIA_CMD
  LDA     #$1B           ; Begin with escape.
  JMP     COLD_START     ; start BASIC

LOAD:
    rts

SAVE:
    rts

MONRDKEY:
CHRIN:
    LDA ACIA_STATUS
    AND #$08
    BEQ no_key
    LDA ACIA_DATA
    CMP #$08           ; Backspace key (ignore)
    BEQ backspace
    JSR CHROUT
    SEC
    RTS
no_key:
    CLC
    RTS
backspace:
    LDA #$08           ; Backspace for the terminal
    JSR CHROUT
    LDA #$5F           ; print underscore (basic's "backspace")
    SEC
    RTS

MONCOUT:
CHROUT:
    PHA
    STA ACIA_DATA
    LDA #$FF
@txdelay:
    DEC
    BNE @txdelay
    PLA
    RTS

IRQ_HANDLER:
    RTI

.include "wozmon.s"

.segment "RESETVEC"
    .word   $0F00           ; NMI vector
    .word   RESET           ; RESET vector (wozmon.s)
    .word   IRQ_HANDLER     ; IRQ vector