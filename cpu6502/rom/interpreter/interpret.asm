; **** UPDATE THE HELP TEXT
.data
  break_input: .asciiz "BRK"
  load_input:  .asciiz "LD"
  run_input:   .asciiz "RUN"
; **** UPDATE THE HELP TEXT

; char must be in A register
interpret_process_char_prompt:
  ldx PROMPT_CHAR_CNT
  sta PROMPT_START, x             ; store char (must be upper case)
  inx                             ; increment prompt char counter
  stx PROMPT_CHAR_CNT
  jmp interpret_process_char_prompt_end
interpret_process_char_prompt_end:
  rts

interpret_cmd:
  clc
  jsr CHECK_BREAK
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_LOAD
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_RUN
  cpy #$01
  beq interpret_cmd_end
interpret_cmd_end:
  rts

CHECK_BREAK:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"K"
  bne cmd_not_break
  ;
  dex
  lda PROMPT_START, x
  cmp #"A"
  bne cmd_not_break
  ;
  dex
  lda PROMPT_START, x
  cmp #"E"
  bne cmd_not_break
  ;
  dex
  lda PROMPT_START, x
  ;
  cmp #"R"
  bne cmd_not_break
  dex
  lda PROMPT_START, x
  cmp #"B"
  bne cmd_not_break
  ;
  lda #BREAK_CMD
  sta CURRENT_COMMAND
  ldy #$01            ; return true
cmd_not_break:
  rts 

CHECK_LOAD:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"D"
  bne cmd_not_load
  ;
  dex
  lda PROMPT_START, x
  cmp #"A"
  bne cmd_not_load
  ;
  dex
  lda PROMPT_START, x
  cmp #"O"
  bne cmd_not_load
  ;
  dex
  lda PROMPT_START, x
  cmp #"L"
  bne cmd_not_load
  ;
  lda #LOAD_CMD
  sta CURRENT_COMMAND
  ldy #$01            ; return true
cmd_not_load:
  rts 

CHECK_RUN:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"N"
  bne cmd_not_run
  ;
  dex
  lda PROMPT_START, x
  cmp #"U"
  bne cmd_not_run
  ;
  dex
  lda PROMPT_START, x
  cmp #"R"
  bne cmd_not_run
  ;
  lda #RUN_CMD
  sta CURRENT_COMMAND
  ldy #$01            ; return true
cmd_not_run:
  rts 

interp_run:           
  ldy #$00                 ; return false
  ldx #$00
interp_run_loop:
  lda run_input, x
  beq interp_run_success   ; if reached the null terminator, then is equal
  cmp PROMPT_START, x
  bne interp_run_not       ; if char not equal, bail
  inx
  jmp interp_run_loop      ; check next char
interp_run_success:
  ldy #$01                 ; return true
interp_run_not:
  rts
