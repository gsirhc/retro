print_program:
  ldy #0
  lda #<PROGRAM_START_ADDR         ; lo byte first
  sta PRT_PROGRAM_PRT
  lda #>PROGRAM_START_ADDR         ; hi byte next
  sta PRT_PROGRAM_PRT + 1
print_program_loop:
  tya                              ; save Y to stack (subroutines use it)
  pha
  lda (PRT_PROGRAM_PRT), y             ; load value from START_ADD + Y
  jsr print_a_hex_acia
  lda #" "                         ; separate values with blank
  jsr print_char_acia
  pla
  tay                              ; restore Y
  iny                              ; inc Y for next value
  cpy #$00                         ; stop when Y rolls over
  beq print_program_done
  tya
  and #$07                         ; For MOD 8 = 0 (8 bytes per line)
  bne print_program_loop
  jsr print_new_line_acia
  jmp print_program_loop
print_program_done:
  jsr print_new_line_acia
  jsr print_program_size_acia
  lda PROGRAM_SIZE
  sta DEC_VALUE
  lda PROGRAM_SIZE + 1
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  rts
