; I/O addresses
PORTB = $6000
PORTA = $6001
DDRB = $6002
DDRA = $6003
PCR = $600c     ; peripheral control reg
IFR = $600d     ; interrupt flag reg
IER = $600e     ; interrupt enable reg

value = $2000   ; max value is 255
mod10 = $2001
numberStr = $2002 ; 3 bytes

; global directives
E  = %10000000
RW = %01000000
RS = %00100000

  .org $8000     ; ROM program start location

.data
  number: .byte 234

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

  lda #$00000001   ; Clear display
  jsr lcd_instruction

  lda #0
  sta numberStr

  ; initialize value to number
  lda number
  sta value

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

loop:
  jmp loop

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

; Add the character in the A reg to the beginning of the null terminated string
push_char:
  pha ; push new char onto stack
  ldy #0

char_loop:
  lda numberStr,y ; get char from string and put into x
  tax
  pla
  sta numberStr,y ; pull chat off the stach and add to the string
  iny
  txa
  pha ; push char from string onto stack
  bne char_loop
  pla
  sta numberStr,y ; add null terminator 
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

  .org $fffc
  .word reset
  .word $0000