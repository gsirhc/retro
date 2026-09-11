; LCD DEMO -- the real 16x2 HD44780 on the board, not just the
; terminal. Each message is a .BYTE string, walked by its own
; small indexed loop instead of one LDA #imm / JSR pair per
; character -- three loops, not one, since this assembler has no
; indirect addressing to share a single loop across LCD_PUTC and
; PRINT_CHAR (see the Help panel's v1 scope). JMP START skips past
; the raw data. LCD_CLEAR ($800F), LCD_LINE2 ($8015) moves to the
; second row -- see the Help panel's OS calls table. ASM, then
; RUN; watch the LCD, not just the terminal.
JMP START
MSG1: .BYTE "6502 ASSEMBLER",$00
MSG2: .BYTE "IT WORKS!",$00
MSG3: .BYTE "CHECK THE LCD DISPLAY!",$0D,$0A,$00
START: JSR $800F
LDX #$00
LOOP1: LDA MSG1,X
BEQ L1DONE
JSR $8009
INX
BRA LOOP1
L1DONE: JSR $8015
LDX #$00
LOOP2: LDA MSG2,X
BEQ L2DONE
JSR $8009
INX
BRA LOOP2
L2DONE: LDX #$00
LOOP3: LDA MSG3,X
BEQ L3DONE
JSR $8003
INX
BRA LOOP3
L3DONE:
