  .include "rom/interpreter/interpret.asm"

; COMMAND values
PROMPT_CMD   = $00
LOAD_CMD     = $01
RUN_CMD      = $02
BREAK_CMD    = $FF

reset_cmd:
  jsr clear_terminal
  jsr text_cmd_os_start   
  rts

soft_reset_cmd:
  lda #PROMPT_CMD          ; init command
  sta CURRENT_COMMAND
  lda #$00                 ; init prompt char counter
  sta PROMPT_CHAR_CNT
  jsr print_command_prompt_symbol 
  rts

; Call from the main program loop
command_prompt_loop:
  jsr check_acia_no_irq
  cpx #$01
  bne command_prompt_loop_end
  cmp #$0D                          ; check if CR
  bne process_char                  ; prompt or load
  ldx PROMPT_CHAR_CNT              
  beq enter_blank_line              ; check if prompt count 0
  jsr interpret_execute_command
  jmp command_prompt_loop_end
process_char:
  ldx CURRENT_COMMAND
  cpx #PROMPT_CMD
  bne load_prompt
  jsr interpret_process_char_prompt
  jmp command_prompt_loop_end
enter_blank_line:
  jsr print_command_prompt_new_line ; if nothing entered with CR, just print new line
  jmp command_prompt_loop_end
load_prompt:
  jsr return_os                     ; receive program via xmodem, sub-routines returns to cmd loop
command_prompt_loop_end:
  rts

interpret_execute_command:
  jsr interpret_cmd      
  lda CURRENT_COMMAND
  cmp #BREAK_CMD            ; BREAK (Debug Wazmon)
  bne cmd1
  brk                       ; program interrupt 
  jmp handle_char_end 
cmd1:
  cmp #LOAD_CMD             ; LOAD 
  bne cmd2
  jsr print_load_status_via
  lda #">"
  jsr print_char_acia
  jsr shell_rx_main         ; start loader
  jmp reset_prompt
cmd2
  cmp #RUN_CMD              ; RUN 
  bne cmd3
  lda #"."
  jsr print_char_acia
  jsr print_run_status_via
  jmp PROGRAM_START_ADDR    ; start the program (blindly)
  jmp handle_char_end
cmd3:
  jsr print_invalid_cmd_acia
reset_prompt:
  lda PROMPT_CMD
  sta CURRENT_COMMAND
handle_char_end:
  lda #$00                  ; reset the program char counter
  sta PROMPT_CHAR_CNT
  jsr print_input_status_via
  jsr print_command_prompt_symbol
  rts

print_command_prompt_symbol:
  lda #$0D                ; new line
  jsr print_char_acia
  lda #"$"
  jsr print_char_acia
  lda #" "
  jsr print_char_acia
  rts

print_command_prompt_new_line:
  pha
  lda #$0D                ; new line
  jsr print_char_acia
  pla
  rts

print_current_cmd_lcd:
  pha
  jsr print_current_cmd_via
  lda #" "
  jsr print_char_lcd
  lda CURRENT_COMMAND
  jsr print_a_hex_lcd
  pla
  rts
