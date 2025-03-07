PORTB = $6000
PORTA = $6001
DDRB = $6002
DDRA = $6003

E  = %10000000
RW = %01000000
RS = %00100000

ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CRTL = $5003

  .org $8000

reset:
  ldx #$ff
  txs

  lda #%11111111 ; Set all pins on port B to output
  sta DDRB
  lda #%11100000 ; Set top 3 pins on port A to output
  sta DDRA

  lda #%00111000 ; Set 8-bit mode; 2-line display; 5x8 font
  jsr lcd_instruction
  lda #%00001110 ; Display on; cursor on; blink off
  jsr lcd_instruction
  lda #%00000110 ; Increment and shift cursor; don't shift display
  jsr lcd_instruction
  lda #%00000001 ; Clear display
  jsr lcd_instruction

  lda #$00
  sta ACIA_STATUS ; soft reset (value not important)

  lda #$1f        ; N-8-1, 19200 baud
  sta ACIA_CRTL   

  lda #$0b        ; no parity, no echo, no interrupts
  sta ACIA_CMD

  lda #">"
  jsr print_char
  ;jsr send_char

rx_wait:
  lda ACIA_STATUS
  jsr print_binary
  jsr delayMove
  lda #%00000001 ; Clear display
  jsr lcd_instruction
  ;lda ACIA_STATUS
  ;and #$08
  lda #"."
  jsr send_char
  beq rx_wait

  ;lda ACIA_DATA
  ;jsr print_binary
  jmp rx_wait

send_char:
  sta ACIA_DATA
tx_wait:
  lda ACIA_STATUS
  and #$10        ; check tx buffer status flag
  beq tx_wait
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

;  A = entry value
print_binary:
  sed        ;2  @2
  tax        ;2  @4
  and #$0F   ;2  @6
  cmp #9+1   ;2  @8
  adc #$30   ;2  @10
  tay        ;2  @12
  txa        ;2  @14
  lsr        ;2  @16
  lsr        ;2  @18
  lsr        ;2  @20
  lsr        ;2  @22
  cmp #9+1   ;2  @24
  adc #$30   ;2  @26
  cld        ;2  @28
  ;  A = MSN ASCII char
  ;  Y = LSN ASCII char
  jsr print_char
  tya
  jsr print_char
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

  .org $fffc
  .word reset
  .word $0000