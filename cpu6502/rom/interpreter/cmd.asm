  .include "rom/interpreter/interpret.asm"
  .include "rom/cmd/list.asm"

reset_cmd:
  pha
  lda #0                ; initial character mem location from start
  sta PROGRAM_COUNTER
  jsr text_cmd_os_start
  pla
  rts

; Call from the main program loop
handle_char_loop:
  jsr check_acia_no_irq
  cpx #$01
  bne no_new_char
  jsr process_char
  jsr check_command
no_new_char:
  rts

check_command:
  jsr interpret_next_char_into_a ; processes the command into A reg
  cmp #NO_CMD
  beq handle_char_end
  cmp #LIST_CMD
  bne handle_char_end
  jsr handle_list_cmd
handle_char_end:
  rts
