;; 65c22 VIA Datasheet: https://eater.net/datasheets/w65c22.pdf
;; LCD Datasheet: https://eater.net/datasheets/HD44780.pdf

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

; GPIO + LCD init only -- no default IRQ source is armed (IER cleared below,
; bit 7 = 0 disables every enable bit set alongside it, per the datasheet).
; A user's own assembled program can arm CA1/Timer1/etc itself via IER if it
; wants VIA interrupts (routed through J7 -- see IRQ_HANDLER in bios.s).
reset_via:
  lda #%11111111                    ; Set all pins on port B to output
  sta DDRB
  lda #%11100001                    ; Set input/output for port A
  sta DDRA
  lda #$7F
  sta IER                           ; disable all VIA interrupt sources
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

; Prints a NUL-terminated string to the LCD, one print_char_lcd call per
; byte -- the LCD has no analogue of STROUT's own single fast loop (no
; equivalent of CHROUT's ACIA shift register to just keep feeding), so
; this is genuinely a loop over the single-character primitive, not a new
; hardware path. Same calling convention as bios.s's STROUT: A/Y = lo/hi
; of the string pointer. Shares ADDR_PTR (bios.s zeropage) with STROUT --
; safe since the two never run concurrently.
PRINT_STR_LCD:
    sta ADDR_PTR
    sty ADDR_PTR+1
    ldy #0
@loop:
    lda (ADDR_PTR),y
    beq @done
    jsr print_char_lcd
    iny
    bne @loop
@done:
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
