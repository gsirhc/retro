  .include "rom/interpreter/interpret.asm"
  .include "rom/cmd/list.asm"

reset_cmd:
  pha
  lda #0                ; initial character mem location from start
  sta PROGRAM_COUNTER
  lda #0                ; unprocessed chars
  sta UNPROCESS_CHARS
  jsr text_cmd_os_start
  pla
  rts

; Char in A register, IRQ handler, so keep fast
handle_char_irq:
  ;ldx PROGRAM_COUNTER
  ;sta START_MEM, x     ; store the char in memory
  ;inx
  ;stx PROGRAM_COUNTER  ; increment the program address cpounter
  ;ldx UNPROCESS_CHARS  
  ;inx
  ;stx UNPROCESS_CHARS  ; increment unprocessed chars count
  ;rts

; Call from the main program loop
handle_char_loop:
  lda UNPROCESS_CHARS  
  cmp #0                    
  beq handle_char_loop_end ; bail if there are no unprocessed chars
  jsr check_command        ; if unprocessed, check command
handle_char_loop_end:
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
