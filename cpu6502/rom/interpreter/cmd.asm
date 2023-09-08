  .include "rom/interpreter/interpret.asm"

reset_cmd:
  pha
  lda #$00                ; set prompt char counter to 0
  sta PROMPT_CNT
  lda PROMPT_CMD          ; init to command prompt mode
  sta CURRENT_COMMAND
  jsr text_cmd_os_start
  pla
  rts

; Call from the main program loop
command_prompt_loop:
  jsr check_acia_no_irq
  cpx #$01
  bne command_prompt_loop_end
  cmp #$0D                ; check if CR
  bne no_carriage_return
  jsr interpret_command
  jmp command_prompt_loop_end
no_carriage_return:
  jsr process_char
command_prompt_loop_end:
  rts

interpret_command:
  jsr interpret_cmd_into_a ; processes the command into A reg
  cmp #PROMPT_CMD          ; PROMPT
  beq handle_char_end
  cmp #WOZMON_CMD          ; WOZMON
  bne handle_char_end
  jmp WOZMON_PRG_START
handle_char_end:
  rts
