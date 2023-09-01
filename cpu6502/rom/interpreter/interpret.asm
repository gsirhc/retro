process_char:
  ldx PROGRAM_COUNTER
  sta START_MEM, x 
  cmp #$61              ; a
  bcc not_lower_case    ; < a
  cmp #$7A              ; z
  bcs not_lower_case    ; > z
  sbc #$1F              ; subtract hex 20 to capetalize
  sta START_MEM, x
not_lower_case:
  jsr print_char_acia
  inc PROGRAM_COUNTER
  rts

interpret_next_char_into_a:
  clc
  ldx PROGRAM_COUNTER
  dex
  lda START_MEM, x
  cmp #$0D              ; CR
  beq LIST
  cmp #$0A              ; LF
  bne cmd_not_list
LIST:
  dex
  cpx #$04
  bcc cmd_not_list    ; less then 4 bytes of memory used, bail
  lda START_MEM, x
  cmp #"T"
  bne cmd_not_list
  dex
  lda START_MEM, x
  cmp #"S"
  bne cmd_not_list
  dex
  lda START_MEM, x
  cmp #"I"
  bne cmd_not_list
  dex
  lda START_MEM, x
  cmp #"L"
  bne cmd_not_list
  dex
  lda #LIST_CMD
  sta COMMAND
  jmp cmd_found
cmd_not_list:
  sta NO_CMD
cmd_found:
  rts 
