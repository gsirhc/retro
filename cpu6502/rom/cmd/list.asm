handle_list_cmd:
  pha
  lda #$0D             ; enter key
  sta print_char_acia  
  sta print_char_acia  ; send twice to start on fresh line
  ldx PROGRAM_COUNTER
list_loop:
  ldx START_MEM
  jsr print_char_acia
  inx
  cpx PROGRAM_COUNTER
  bcs handle_list_cmd_end
  jmp list_loop
handle_list_cmd_end:
  pla
  rts