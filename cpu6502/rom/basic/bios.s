.setcpu "65C02"
.debuginfo           ; Generates symbol table
.segment "BIOS"

.include "commands.s"

RESET:
    JMP COMMANDS_INIT  ; initialize custom commands
    JMP COLD_START     ; start BASIC
    RTS

LOAD:
    RTS

SAVE:
    RTS

MONRDKEY:
CHRIN:
    JSR read_acia
    RTS

MONCOUT:
CHROUT:
    JSR write_acia
    RTS

IRQ_HANDLER:
    RTI

.include "wozmon.s"

.segment "RESETVEC"
    .word   $0F00           ; NMI vector
    .word   RESET           ; RESET vector (wozmon.s)
    .word   IRQ_HANDLER     ; IRQ vector