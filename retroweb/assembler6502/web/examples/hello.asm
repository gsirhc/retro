; HELLO, WORLD. An indexed loop prints the .BYTE message. JMP START
; skips the data. No <// >> operators yet, so PRINT_STR's A/Y
; convention isn't reachable and the loop stands in. ASM, then RUN.
JMP START
MSG: .BYTE "HELLO, WORLD!",$0D,$0A,$00
START: LDX #$00
LOOP: LDA MSG,X
BEQ DONE
JSR $8003
INX
BRA LOOP
DONE:
