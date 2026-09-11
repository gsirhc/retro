; PRIMES -- lists every prime below 300 by plain trial division
; (no sqrt bound, no early exit -- the honestly slow way), real
; work for a 1MHz clock. 16-bit candidate/divisor/remainder so
; the range goes past 255 -- one shared 16-bit decimal printer
; (PR16) handles both each found candidate and the final count,
; instead of two separate printers. ASM, then RUN -- real time.
LDA #$02
STA $10
STZ $11
STZ $17
CANDLP: LDA $11
CMP #$01
BCC CTEST
BNE CDONE
LDA $10
CMP #$2C
BCC CTEST
CDONE: JMP ALLDON
CTEST: LDA #$01
STA $16
LDA #$02
STA $12
STZ $13
DIVLP: LDA $13
CMP $11
BCC DTEST
BNE DGOEND
LDA $12
CMP $10
BCC DTEST
DGOEND: JMP DIVEND
DTEST: LDA $10
STA $14
LDA $11
STA $15
MODLP: LDA $15
CMP $13
BCC MODEND
BNE SUBIT
LDA $14
CMP $12
BCC MODEND
SUBIT: SEC
LDA $14
SBC $12
STA $14
LDA $15
SBC $13
STA $15
BRA MODLP
MODEND: LDA $14
BNE NOTDIV
LDA $15
BNE NOTDIV
STZ $16
NOTDIV: LDA $12
CLC
ADC #$01
STA $12
BCC DIVLP2
INC $13
DIVLP2: JMP DIVLP
DIVEND: LDA $16
BNE ISPRIM
JMP SKIP
ISPRIM: INC $17
LDA $10
STA $19
LDA $11
STA $1A
JSR PR16
LDA #$20
JSR $8003
SKIP: LDA $10
CLC
ADC #$01
STA $10
BCC NOINCB
INC $11
NOINCB: JMP CANDLP
ALLDON: LDX #$00
CNTL: LDA CNTMSG,X
BEQ CNTD
JSR $8003
INX
BRA CNTL
CNTD: LDA $17
STA $19
STZ $1A
JSR PR16
LDA #$0D
JSR $8003
LDA #$0A
JSR $8003
JMP DONE2
; PR16 -- prints the 16-bit value in $19/$1A as decimal (no leading
; zeros, no trailing space/CR -- callers add those), shared by ISPRIM
; (each found candidate) and CNTD (the final count, hi byte always 0
; since it never exceeds 255) instead of two near-duplicate printers.
PR16: STZ $1B
STZ $1D
PR16H: LDA $1A
BNE PR16H2
LDA $19
CMP #$64
BCC PR16HD
PR16H2: SEC
LDA $19
SBC #$64
STA $19
LDA $1A
SBC #$00
STA $1A
INC $1B
BRA PR16H
PR16HD: LDA $1B
BEQ PR16NH
LDA $1B
CLC
ADC #$30
JSR $8003
LDA #$01
STA $1C
BRA PR16T
PR16NH: STZ $1C
PR16T: LDA $19
CMP #$0A
BCC PR16TD
SEC
SBC #$0A
STA $19
INC $1D
BRA PR16T
PR16TD: LDA $1D
BNE PR16PT
LDA $1C
BEQ PR16NT
PR16PT: LDA $1D
CLC
ADC #$30
JSR $8003
PR16NT: LDA $19
CLC
ADC #$30
JSR $8003
RTS
CNTMSG: .BYTE "COUNT: ",$00
DONE2:
