print_program:
  ldy #$00
  sta LOCAL_MEM_BYTE1
  sta LOCAL_MEM_BYTE2
print_program_loop:
  tya                              ; save Y to stack (subroutines use it)
  pha
  lda (>PROGRAM_START_ADDR), y     ; load value from START_ADD + Y
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
  bne stay_line
  jsr print_new_line_acia
stay_line:
  jmp print_program_loop
print_program_done:
  rts
