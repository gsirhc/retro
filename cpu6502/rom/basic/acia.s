ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CTRL = $5003

reset_acia:
    LDA #$1F           ; 8-N-1, 19200 baud.
    STA ACIA_CTRL
    LDA #$0B           ; No parity, no echo, no interrupts.
    STA ACIA_CMD
    LDA #$1B           ; Begin with escape.

read_acia:
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
    LDA #$5F           ; send underscore to basic for its backspace (don't echo)
    SEC
    RTS

write_acia:
    PHA
    STA ACIA_DATA
    LDA #$FF
@txdelay:
    DEC
    BNE @txdelay
    PLA
    RTS

