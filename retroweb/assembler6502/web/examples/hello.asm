; HELLO, WORLD -- .BYTE stores the message; a small indexed loop
; (LDA MSG,X / JSR PRINT_CHAR / INX) prints it, instead of one
; LDA #imm / JSR pair per character. JMP START skips past the raw
; data (falling into it would run the string bytes as instructions).
; No <// >> operators yet (see the Help panel), so PRINT_STR's A/Y
; calling convention isn't reachable directly -- this loop is the
; workaround. ASM, then RUN.
JMP START
MSG: .BYTE "HELLO, WORLD!",$0D,$0A,$00
START: LDX #$00
LOOP: LDA MSG,X
BEQ DONE
JSR $8003
INX
BRA LOOP
DONE:
