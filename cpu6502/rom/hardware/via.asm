reset_via_irq:
  jsr reset_via
  
  rts

reset_via:
  lda #%11111111 ; Set all pins on port B to output
  sta DDRB
  lda #%11100000 ; Set top 3 pins on port A to output for LCD, other bits available for input
  sta DDRA
  lda #$82
  sta IER        ; enable interrupts
  lda #$00
  sta PCR        ; clear PCR (transition to low state for interrupt)

  lda #%00111000 ; Set 8-bit mode; 2-line display; 5x8 font
  jsr lcd_instruction
  lda #%00001110 ; Display on; cursor on; blink off
  jsr lcd_instruction
  lda #%00000110 ; Increment and shift cursor; don't shift display
  jsr lcd_instruction
  jsr clear_lcd
  rts

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

cursorLine1:
  lda #%00000010
  jsr lcd_instruction
  rts

cursorLine2:
  lda #%11000000   ; second line
  jsr lcd_instruction
  rts

;  A = entry value
print_a_hex:
  pha
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
  pla
  rts

print_carry_flag:
  pha
  bcc print_carry_clear
  lda #"1"
  jsr print_char_lcd
  jmp print_carry_flag_end
print_carry_clear:
  lda #"0"
  jsr print_char_lcd
print_carry_flag_end:
  pla
  rts

print_cpu_state:
  pha
  lda #" "
  jsr print_char_lcd
  lda #"P"
  jsr print_char_lcd
  lda #":"
  jsr print_char_lcd
  lda DEBUG_PROG
  jsr print_a_hex
  lda #" "
  jsr print_char_lcd
  lda #"A"
  jsr print_char_lcd
  lda #":"
  jsr print_char_lcd
  lda DEBUG_A_VAL
  jsr print_a_hex
  jsr cursorLine2
  lda #" "
  jsr print_char_lcd
  lda #"X"
  jsr print_char_lcd
  lda #":"
  jsr print_char_lcd
  lda DEBUG_X_VAL
  jsr print_a_hex
  lda #" "
  jsr print_char_lcd
  lda #"Y"
  jsr print_char_lcd
  lda #":"
  jsr print_char_lcd
  lda DEBUG_Y_VAL
  jsr print_a_hex
  lda #" "
  jsr print_char_lcd
  lda #"C"
  jsr print_char_lcd
  lda #":"
  jsr print_char_lcd
  lda DEBUG_C_VAL
  jsr print_a_hex
  pla
  rts

clear_lcd:
  pha
  lda #%00000001 ; Clear display
  jsr lcd_instruction
  pla
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