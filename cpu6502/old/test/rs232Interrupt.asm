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
  jsr clear_lcd

  ; ACIA setup
  lda #%00000000
  sta ACIA_STATUS

  lda #%00000101
  sta ACIA_CMD
  
  lda #%00001111
  sta ACIA_CRTL

  jsr print_new_line

  cli             ; clear interrupt disable

loop:
  lda #"."
  jsr print_char_lcd
  jmp loop

; print A register to ACIA
; Based on http://forum.6502.org/viewtopic.php?f=4&t=2543&start=30#p29795
;print_char_acia:
;  pha
;  sta ACIA_DATA
;tx_wait:
;  lda ACIA_STATUS
;  and #$10        ; check tx buffer status flag
;  beq tx_wait
;  pla
;  rts

print_char_lcd:
  pha
  jsr lcd_wait
  sta PORTB
  lda #RS         ; Set RS; Clear RW/E bits
  sta PORTA
  lda #(RS | E)   ; Set E bit to send instruction
  sta PORTA
  lda #RS         ; Clear E bits
  sta PORTA
  pla
  rts

print_new_line:
  jsr clear_lcd
  ;lda #$0A
  ;jsr print_char_acia
  lda #">"
  ;jsr print_char_acia
  jsr print_char_lcd
  rts

delay_6551:
delay_loop:
  ldy #6 ; inflated from numbers in original code.
minidly:
  ldx #$68
delay_1:
  dex
  bne delay_1
  dey
  bne minidly
delay_done:
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
  jsr print_char_lcd
  tya
  jsr print_char_lcd
  rts

clear_lcd:
  lda #%00000001 ; Clear display
  jsr lcd_instruction
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

irg:
  pha
  lda #">"
  jsr print_char_lcd
  ; TEST TO DISPLAY STATUS AND DATA REGISTERS ON ACIA
  lda ACIA_STATUS
  cmp #$D0
  beq clearStatus
  cmp #$10
  beq clearStatus
  cmp #$90
  bne dontClearStatus
clearStatus
  lda #"C"
  jsr print_char_lcd
  lda #%00000000      ; clear the status interrupt flag
  sta ACIA_STATUS
  jmp irg
dontClearStatus:
  jsr print_binary
  lda #" "
  jsr print_char_lcd
  lda ACIA_DATA
  jsr print_binary
  jsr delayMax
  jsr clear_lcd
  ; END TEST

  ;lda #"I"
  ;jsr print_char_lcd
  ;lda ACIA_STATUS
  ;and #$08            ; interrupt, receive full
  ;beq ignore_irq
  ;lda ACIA_DATA
  ;jsr print_char_lcd
  ;jsr print_char_acia
  ;jsr clear_lcd
  ;lda ACIA_DATA       ; read the data and output
  ;jsr print_char_acia
  ;jsr print_char_lcd
  ;lda #%00000000      ; clear the status interrupt flag
  ;sta ACIA_STATUS
  pla
  rti

nmi:
  rti

  .org $fffa
  .word nmi
  .word reset
  .word irg