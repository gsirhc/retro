print_program_256:
  pha
  tya
  pha
  txa
  pha
  ldx #$00
  ldy #$00
  lda #<PROGRAM_START_ADDR         ; High byte
  sta LOCAL_MEM_BYTE1
print_program_256_loop:
  txa
  pha
  tya
  pha
  lda (LOCAL_MEM_BYTE1), y
  jsr print_a_hex_acia
  lda #" "
  jsr print_char_acia
  pla
  tay
  pla
  tax
  iny
  ;AND     #$07                     ; For MOD 8 = 0
  beq print_program_256_loop
  inx
  beq print_program_256_loop_end
  inc LOCAL_MEM_BYTE1
  jmp print_program_256_loop
print_program_256_loop_end:
  pla
  tax
  pla
  tay
  pla
  rts