interpret_next_char_into_a:
  clc
  lda PROGRAM_COUNTER
  sbc UNPROCESS_CHARS
  tax                 ; current position to process
  lda START_MEM, x
  jsr is_enter_key    ; enter key must follow all commands
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
  ldx UNPROCESS_CHARS
  dex
  stx UNPROCESS_CHARS
  rts 

process_char:
  jsr clear_lcd
  ldx PROGRAM_COUNTER
  lda START_MEM, x  
  jsr print_a_hex
  cmp #$61              ; a
  bcc not_lower_case
  cmp #$7A              ; z
  beq not_lower_case
  sbc #$20              ; subtract hex 20 to capetalize
  sta START_MEM, x
not_lower_case:
  rts

is_enter_key:
  cmp #$0D              ; CR
  beq is_enter_key_end
  cmp #$0A              ; LF
is_enter_key_end:
  rts
