;; 65c22 VIA Datasheet: https://eater.net/datasheets/w65c22.pdf 
;; LCD Datasheet: https://eater.net/datasheets/HD44780.pdf

;; Timer 1 interval
TIMER_INT_LO_BYTE = $06            ; 16666 ($4106) = 60.003 hz (with 1mhz CPU), not exact but close enough
TIMER_INT_HI_BYTE = $41            ; NOTE, if these are changed, change the time subroutines

PORTB = $6000     ; Port B data
PORTA = $6001     ; Port A data
DDRB  = $6002     ; Data Direction B
DDRA  = $6003     ; Data Direction A
T1CL  = $6004     ; timer 1 control Lo byte
T1CH  = $6005     ; timer 1 control Hi byte
ACR   = $600B     ; aux control register
PCR   = $600C     ; peripheral control reg
IFR   = $600D     ; interrupt flag reg
IER   = $600E     ; interrupt enable reg

E  = %10000000
RW = %01000000
RS = %00100000

reset_via_irq:
  lda #%11111111                    ; Set all pins on port B to output
  sta DDRB
  lda #%11100001                    ; Set input/output for port A
  sta DDRA
  
  lda #%01000000                    ; Free Run mode Timer 1 (continuous interrupts)
  sta ACR
  lda #TIMER_INT_LO_BYTE
  sta T1CL                          ; T1 ctrl lo byte
  lda #TIMER_INT_HI_BYTE       
  sta T1CH                          ; T1 ctrl hi byte
  lda #%11000010                    ; Set/Clear (first bit) IRQ CA1 and Timer 1 enabled
  sta IER                           ; enable interrupts
  lda #$00
  sta PCR                           ; clear PCR (transition to low state for interrupt)

  lda #%00111000                    ; Set 8-bit mode; 2-line display; 5x8 font
  jsr lcd_instruction
  lda #%00001110                    ; Display on; cursor on; blink off
  jsr lcd_instruction
  lda #%00000110                    ; Increment and shift cursor; don't shift display
  jsr lcd_instruction
  jsr clear_lcd
  
  rts

disable_via_irq:
  lda $00
  sta IER
  rts

enable_via_irq:
  lda IER
  ora #%00000010
  sta IER
  rts

print_char_lcd:
  pha
  jsr lcd_wait
  sta PORTB
  lda #RS                           ; Set RS; Clear RW/E bits
  sta PORTA
  lda #(RS | E)                     ; Set E bit to send instruction
  sta PORTA
  lda #RS                           ; Clear E bits
  sta PORTA
  pla
  rts

cursorLine1_lcd:
  lda #%00000010
  jsr lcd_instruction
  rts

cursorLine2_lcd:
  lda #%11000000                     ; second line
  jsr lcd_instruction
  rts

clear_lcd:
  pha
  lda #%00000001                     ; Clear display
  jsr lcd_instruction
  pla
  rts

lcd_wait:
  pha
  lda #%00000000                     ; Port B is input
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
  lda #%11111111                    ; Port B is output
  sta DDRB
  pla
  rts

lcd_instruction:
  jsr lcd_wait
  sta PORTB
  lda #0                            ; Clear RS/RW/E bits
  sta PORTA
  lda #E                            ; Set E bit to send instruction
  sta PORTA
  lda #0                            ; Clear RS/RW/E bits
  sta PORTA
  rts
