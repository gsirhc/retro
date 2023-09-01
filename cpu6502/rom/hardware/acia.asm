reset_acia_no_irq:
  lda #$00
  sta ACIA_STATUS
  lda #$0b
  sta ACIA_CMD
  lda #$1f
  sta  ACIA_CTRL
  rts

reset_acia_irq:
  lda #%00000000
  sta ACIA_STATUS
  lda #%00001001    ; 0x0b
  sta ACIA_CMD
  lda #%00001111     ; 0x1f
  sta  ACIA_CTRL
  rts

; return true/false (1|0) in X register
check_acia_no_irq:
  ldx #$00         ; return false
  lda ACIA_STATUS
  and #$08
  beq no_char_in_irq
  lda ACIA_DATA
  ldx #$01         ; return true
no_char_in_irq:
  rts

check_acia_irq:
  ldx #$00         ; return false
  lda ACIA_DATA
  and #%10001000
  bne no_char
  lda ACIA_STATUS  ; clear interrupt
  ldx #$01         ; return true
no_char:
  rts

; print A register to ACIA
; Based on http://forum.6502.org/viewtopic.php?f=4&t=2543&start=30#p29795
print_char_acia:
  pha
  sta ACIA_DATA
tx_wait:
  lda ACIA_STATUS
  and #$10        ; check tx buffer status flag
  beq tx_wait
  pla
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