ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CTRL = $5003

reset_acia_no_irq:
  lda #$00
  sta ACIA_STATUS
  lda #$0b
  sta ACIA_CMD
  lda #$1f
  sta ACIA_CTRL
  rts

reset_acia_irq:
  lda #%00000000
  sta ACIA_STATUS
  lda #%00001001     ; 0x0b
  sta ACIA_CMD
  lda #%00001111     ; 0x1f
  sta ACIA_CTRL
  rts

; returns true/false (1|0) in X register
check_acia_no_irq:
  ldx #0                              ; return false
  lda ACIA_STATUS
  and #$08                            ; received char status
  beq check_acia_no_irq_end
  lda ACIA_DATA
  cmp #$08                            ; check if backspace, don't echo
  beq check_acia_no_irq_true
  cmp #$0D                            ; check if CR, don't echo
  beq check_acia_no_irq_true
  jsr print_char_acia
check_acia_no_irq_true:
  ldx #1                              ; return true
check_acia_no_irq_end:
  rts

; returns true/false (1|0) in X register
check_acia_rx:
  ldx #$00                            ; return false
  lda ACIA_STATUS
  and #$08                            ; received char status
  beq no_data_rx
  lda ACIA_DATA
  ldx #$01
no_data_rx:
  rts

check_acia_irq:
  ldx #$00                            ; return false
  lda ACIA_DATA
  and #%10001000
  bne no_char
  lda ACIA_STATUS                     ; clear interrupt
  ldx #$01                            ; return true
no_char:
  rts

; print A register to ACIA
print_char_acia:
  pha
  lda ACIA_STATUS
  pla
  sta ACIA_DATA
  jsr delay_6551_tx ; delay because the Tx status doesn't work (bug: always 1)
  rts

print_backspace_destruct_acia:
  lda #$08              ; backspace
  jsr print_char_acia
  lda #$20              ; space
  jsr print_char_acia
  lda #$08              ; backspace
  jsr print_char_acia
  rts

print_new_line_acia:
  pha
  lda #$0D
  jsr print_char_acia
  pla
  rts

print_a_hex_acia:
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
  jsr print_char_acia
  tya
  jsr print_char_acia
  pla
  rts

; Delay to fix the ACIA Tx status bug (status is always 1 meaning Tx finished even though it hasn't)
; Required delay calculated by: 
;      ((1 / baud) * Clock * (8 bits + 2 start/stop bits))
;      ((1 / 19200) * 10,000,000) = 520 clock cycles to delay long enough
;
; Code cycles break down:
;       jsr = 6 cycles
;       pha = 3 cycles
;       pla = 4 cycles
; txa / tax = 2 cycles
;       ldx = 2 cycles (load immediate)
;       dex = 2 cycles
;       bne ~ 3 cycles (depends on the memory page the target address is on)
;       rts = 6 cycles
;
; total entry/exit cycles = (2pha) + (2pla) + (2t*) + (1ldx) + (1jsr) + (1rts)
;                         = 6 + 8 + 4 + 3 + 6 + 6
;                         = 33
; Cycles per loop = (dex) + (bn2)
;                 = 4 per loop
; Loop cycles = (520 total cycles - 33 entry/exit cycles) / 4 cycles per loop
;             = 487 / 4
;             = 121.75 (122) loop cycles needed
delay_6551_tx:
  pha
  txa
  pha
  ldx #122                 ; 122 decimal (max is 255)
delay_6551_tx_loop:
  dex
  bne delay_6551_tx_loop
  pla
  tax
  pla
  rts

print_a_digits:
  pha
  tax                     ; use X to keep track of the digits
Loop:
  txa
  ; Convert hex digit to ASCII
  and #$0F                ; Mask out all but the lower nibble
  tax                     ; Transfer the result to X
  ; Convert X (hex digit) to ASCII ('0' to '9' or 'A' to 'F')
  cmp #$0A                ; Compare X to 10 (hex 'A')
  bcc Digit               ; Branch if X < 10
  adc #$07                ; Adjust X to convert to 'A' to 'F'
Digit:
  adc #$30                ; Convert X to ASCII ('0' to '9' or 'A' to 'F')
  jsr print_char_acia 
  ; Decrement loop counter
  dex
  ; Check if we've processed all hex digits
  bne Loop
  pla
  rts