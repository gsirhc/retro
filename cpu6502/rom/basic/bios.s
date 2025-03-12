.setcpu "65C02"
.debuginfo           ; Generates symbol table

.zeropage
                .org ZP_START0
READ_PTR:       .res 1
WRITE_PTR:      .res 1

.segment "INPUT_BUFFER"
INPUT_BUFFER:   .res $100

.segment "BIOS"

.include "commands.s"

ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CTRL = $5003

RESET:
    LDA READ_PTR           ; init buffer pointers
    STA WRITE_PTR
    CLI
    LDA #$1F               ; ACIA: 8-N-1, 19200 baud.
    STA ACIA_CTRL
    LDA #$89               ; ACIA: No parity, no echo, interrupts.
    STA ACIA_CMD
    LDA #$1B               ; ACIA: Begin with escape.
    JMP COLD_START         ; start BASIC

LOAD:
    RTS

SAVE:
    RTS

MONRDKEY:
CHRIN:
    PHX
    JSR BUFFER_SIZE
    BEQ buffer_empty
    JSR READ_BUFFER
    CMP #$08           ; Backspace key (ignore)
    BEQ backspace
    JSR CHROUT
    SEC
    PLX
    RTS
backspace:
    LDA #$08           ; Backspace for the terminal
    JSR CHROUT
    LDA #$5F           ; send underscore to basic for its backspace (don't echo)
    SEC
    PLX
    RTS
buffer_empty:
    CLC
    PLX
    RTS

MONCOUT:
CHROUT:
    PHA
    STA ACIA_DATA
    LDA #$FF
@txdelay:              ; delay loop for known ACIA bug not updating status register 
    DEC
    BNE @txdelay
    PLA
    RTS

WRITE_BUFFER:
    LDX WRITE_PTR
    STA INPUT_BUFFER,x
    INC WRITE_PTR
    RTS

READ_BUFFER:
    LDX READ_PTR
    LDA INPUT_BUFFER,x
    INC READ_PTR
    RTS

BUFFER_SIZE:
    LDA WRITE_PTR
    SEC
    SBC READ_PTR
    RTS

IRQ_HANDLER:
    PHA
    PHX
    LDA ACIA_STATUS
    AND #$08
    BEQ ignore_irq
    LDA ACIA_DATA
    JSR WRITE_BUFFER
ignore_irq:
    PLX
    PLA
    RTI

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; VT100 commands, see http://www.braun-home.net/michael/info/misc/VT100_commands.htm
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

CLEAR_TERMINAL:
  PHA
  JSR PRINT_LEADER
  LDA #'2'
  JSR CHROUT
  LDA #'J'              
  JSR CHROUT
  JSR PRINT_LEADER
  LDA #'f'              ; reset cursor to "home" (top-left of screen)
  JSR CHROUT
  PLA
  RTS

PRINT_LEADER:
  LDA #$1B              ; ESC
  JSR CHROUT
  LDA #'['
  JSR CHROUT
  RTS

.include "wozmon.s"

.segment "RESETVEC"
    .word   $0F00           ; NMI vector
    .word   RESET           ; RESET vector (wozmon.s)
    .word   IRQ_HANDLER     ; IRQ vector