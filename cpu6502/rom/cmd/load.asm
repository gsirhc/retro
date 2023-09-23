
shell_rx_main:
  ldy #$00
  sty LOCAL_MEM_BYTE1               ; init the counter
  lda #<PROGRAM_START_ADDR         ; Low byte first
  sta PROGRAM_PRT
  lda #>PROGRAM_START_ADDR         ; High byte next
  sta PROGRAM_PRT + 1
shell_rx_loop:
  jsr check_acia_rx
  cpx #$01
  bne no_char_rx
  sta (PROGRAM_PRT), Y
  stx LOCAL_MEM_BYTE1
  iny
  jmp shell_rx_loop
no_char_rx:
  lda LOCAL_MEM_BYTE1
  cmp #$00
  beq shell_rx_loop       
  adc #$01
  bcs end_rx
  sta LOCAL_MEM_BYTE1
  jmp shell_rx_loop
end_rx:
  jmp return_os
  rts