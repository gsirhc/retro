; I/O addresses
PORTB = $6000
PORTA = $6001
DDRB = $6002
DDRA = $6003
PCR = $600c     ; peripheral control reg
IFR = $600d     ; interrupt flag reg
IER = $600e     ; interrupt enable reg

; RAM addresses
CPOS = $1000      ; current (character) position
HIT  = $1001      ; button hit
HITSCORE = $1002  ; hits scored
NHITSCORE = $1003 ; hits missed
NUMPLAYS = $1004  ; hits missed
NUMVAL = $1005   ; max value is 255
NUMSTR = $1006    ; 3 bytes

; global directives
E  = %10000000
RW = %01000000
RS = %00100000

  .org $8000     ; ROM program start location

.data
  ; string consts    1234567890123456   -- 16 char display
  intro:    .asciiz "STOP IT GAME v1"
  startMsg: .asciiz "   GET READY!  "
  zoneMsg:  .asciiz "------>  <------"
  hitMsg:   .asciiz "     HIT!!!    "
  noHitMsg: .asciiz "     no hit    "
  scoreMsg: .asciiz "Score: "          ; score dynamically written

reset:
  ldx #$ff       ; stack pointer
  txs

  lda #%11111111 ; Set all pins on port B to output
  sta DDRB
  lda #%11100000 ; Set top 3 pins on port A to output
  sta DDRA

  lda #%00111000 ; Set 8-bit mode; 2-line display; 5x8 font
  jsr lcd_instruction
  lda #%00001100 ; Display on; cursor on/off; blink off
  jsr lcd_instruction
  lda #%00000110 ; Increment and shift cursor; don't shift display
  jsr lcd_instruction

  jsr clear
  jsr printIntro 
  jsr delayDisplayNorm

  ; init game variables
  lda #0
  sta HITSCORE
  sta NHITSCORE
  sta NUMPLAYS
  sta NUMSTR

resetGame:
  jsr clear  

  jsr printStart 
  jsr delayDisplayNorm

  jsr cursorLine2
  jsr printZone

  lda #%00000010 ; head of top line
  jsr lcd_instruction

  lda #$0
  sta HIT
  cli            ; clear interrupt disable
  lda #$82
  sta IER        ; enable interrupts
  lda #$00
  sta PCR        ; clear PCR (transition to low state for interrupt)
  inc NUMPLAYS

gameLoop:
  ldx #$10        ; 16 spaces
motionRightLoop:
  stx CPOS
  lda #%11111111  ; block (all pixels on)
  jsr print_char
  jsr delayMove
  jsr cursorBack
  lda #" "
  jsr print_char
  jsr checkPress
  bne resetGame
  ldx CPOS
  dex
  bne motionRightLoop

  ldx #$10        ; 16 spaces
motionLeftLoop:
  stx CPOS
  jsr cursorBack
  lda #%11111111  ; block (all pixels on)
  jsr print_char
  jsr delayMove
  jsr cursorBack
  lda #" "
  jsr print_char
  jsr cursorBack
  jsr checkPress
  bne resetGame
  ldx CPOS
  dex
  bne motionLeftLoop

  jmp gameLoop

checkPress:
  ldx HIT
  beq noPress
  lda CPOS
  cmp #$8
  beq hit
  cmp #$9
  beq hit
  jmp noHit
hit 
  inc HITSCORE
  jsr clear
  jsr printHit
  jmp score
noHit
  inc NHITSCORE
  jsr clear
  jsr printNoHit
score:
  jsr cursorLine2
  jsr printScore
  jsr delayDisplayNorm
  ldx #$01         ; return true
  rts
noPress:
  ldx #$00         ; return false
  rts

clear:
  lda #$00000001   ; Clear display
  jsr lcd_instruction
  rts

cursorBack:
  lda #%00010000 ; back 1 space
  jsr lcd_instruction
  rts

cursorLine2:
  lda #%11000000   ; second line
  jsr lcd_instruction
  rts

printIntro:
  ldx #0
printIntroLp:
  lda intro,x
  beq endPrintInfo
  jsr print_char
  inx
  jsr printIntroLp
endPrintInfo:
  rts

printZone:
  ldx #0
printZoneLp
  lda zoneMsg,x
  beq endPrintZone
  jsr print_char
  inx
  jsr printZoneLp
endPrintZone:
  rts

printStart:
  ldx #0
printStartLp:
  lda startMsg,x
  beq endPrintStart
  jsr print_char
  inx
  jsr printStartLp
endPrintStart:
  rts

printHit:
  ldx #0
printHitLp:
  lda hitMsg,x
  beq endPrintHit
  jsr print_char
  inx
  jsr printHitLp
endPrintHit:
  rts

printNoHit:
  ldx #0
printNoWaitLp:
  lda noHitMsg,x
  beq endPrintNoHit
  jsr print_char
  inx
  jsr printNoWaitLp
endPrintNoHit:
  rts

printScore:
  jsr printScoreHeader
  lda NUMVAL
  sta value
  ; initialize value to number
  lda number
  sta value
  jsr delayDisplayLong
  rts

printScoreHeader:
  ldx #0
printScoreHeaderLp:
  lda scoreMsg,x
  beq endPrintScoreHeader
  jsr print_char
  inx
  jsr printScoreHeaderLp
endPrintScoreHeader:
  rts

delayMove:
  ldy #$19
delayMove2:
  ldx #$ff
delayMove1:
  nop
  dex
  bne delay1 
  dey
  bne delay2
  rts

delayDisplayNorm:
  jsr delayMax
  jsr delayMax
  rts

delayDisplayLong:
  jsr delayDisplayNorm
  jsr delayDisplayNorm
  rts

delayMax:
  ldy #$ff
delay2:
  ldx #$ff
delay1:
  nop
  dex
  bne delay1 
  dey
  bne delay2
  rts

lcd_wait:
  pha
  lda #%00000000  ; Port B is input
  sta DDRB
lcdbusy:
  lda #RW
  sta PORTA
  lda #(RW | E)
  sta PORTA
  lda PORTB
  and #%10000000
  bne lcdbusy

  lda #RW
  sta PORTA
  lda #%11111111  ; Port B is output
  sta DDRB
  pla
  rts

lcd_instruction:
  jsr lcd_wait
  sta PORTB
  lda #0         ; Clear RS/RW/E bits
  sta PORTA
  lda #E         ; Set E bit to send instruction
  sta PORTA
  lda #0         ; Clear RS/RW/E bits
  sta PORTA
  rts

print_char:
  jsr lcd_wait
  sta PORTB
  lda #RS         ; Set RS; Clear RW/E bits
  sta PORTA
  lda #(RS | E)   ; Set E bit to send instruction
  sta PORTA
  lda #RS         ; Clear E bits
  sta PORTA
  rts

divide:
  ; init remainder to 0
  lda #0
  sta mod10
  clc  ; clear carry bit

  ldx #8
divloop:
  ; rotate quotient and remainder
  rol value
  rol mod10

  ; dividend - divisor
  sec
  lda mod10
  sbc #10
  bcc ignore_result ; branch if dividend < divisor
  tay
  sty mod10

ignore_result:
  dex
  bne divloop
  rol value ; shift in the last bit of the quotient

  lda mod10
  clc
  adc #"0"
  jsr push_char

  ; if value != 0, the continue dividing
  lda value
  ora value
  bne divide  ; branch if not done

print:
  ldx #0
print1:
  lda numberStr,x
  beq endPrint
  jsr print_char
  inx
  jsr print1
endPrint:
  rts

nmi:
  rti
  
irg:
  pha             ; store existing values in A / X to stack
  txa
  pha
  lda #$1
  sta HIT
  bit PORTA       ; read PORTA to clear interrupt
  ldx #$ff
debounce:         ; debounce button (not idea in an interrupt handler)
  dex
  bne debounce
  pla             ; restore A / X values from stack
  tax
  pla
  rti

  .org $fffa
  .word nmi
  .word reset
  .word irg