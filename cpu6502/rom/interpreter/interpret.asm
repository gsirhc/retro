; char must be in A register
process_char:
  ldx CURRENT_COMMAND
  cmp PROMPT_CMD
  bne store_in_memory
  ldx PROMPT_CNT
  sta PROMPT_PTR, x 
  cmp #$61              ; a
  bcc not_lower_case    ; < a
  cmp #$7A              ; z
  bcs not_lower_case    ; > z
  sbc #$1F              ; subtract hex 31 to capetalize
  sta PROMPT_PTR, x
not_lower_case:
  inx 
  stx PROMPT_CNT
store_in_memory:
  ; TODO
  rts

interpret_cmd_into_a:
  clc
  ldx PROMPT_CNT
  dex
  lda PROMPT_PTR, x
  cmp #$0D              ; CR
  jsr WOZMON
  cpy #$01
  lda PROMPT_CNT
  dex
  bne interpret_cmd_end
interpret_cmd_end:
  rts

WOZMON:
  ldy #$00            ; return false if falls though
  dex
  cpx #$04
  bcc cmd_not_list    ; less then 4 bytes of memory used, bail
  lda PROMPT_PTR, x
  cmp #"N"
  bne cmd_not_list
  dex
  lda PROMPT_PTR, x
  cmp #"O"
  bne cmd_not_list
  dex
  lda PROMPT_PTR, x
  cmp #"M"
  bne cmd_not_list
  dex
  lda PROMPT_PTR, x
  cmp #"Z"
  bne cmd_not_list
  dex
  lda PROMPT_PTR, x
  cmp #"O"
  bne cmd_not_list
  dex
  lda PROMPT_PTR, x
  cmp #"W"
  bne cmd_not_list
  lda #WOZMON_CMD
  sta CURRENT_COMMAND
  ldy #$01            ; return true
cmd_not_list:
  rts 
