  .include "rom/os/load.asm"
  .include "rom/os/printPrg.asm"
  .include "rom/os/text.asm"
  .include "rom/os/exec.asm"

reset_cmd:
  jsr clear_terminal
  jsr text_cmd_os_start

  ; Clear user program (print error)
  lda #$4C                  ; jmp command
  sta PROGRAM_START_ADDR
  lda #$09                  ; lo byte of (see BIOS)
  sta PROGRAM_START_ADDR + 1
  lda #$fe                  ; hi byte of (see BIOS)
  sta PROGRAM_START_ADDR + 2
  lda #0
  sta PROGRAM_SIZE         ; init program size
  sta PROGRAM_SIZE + 1     ; init program size
  rts

soft_reset_cmd:
  lda #$00                 
  sta PROMPT_CHAR_CNT      ; init prompt char counter
  jsr print_command_prompt_symbol 
  jsr print_input_status_via
  rts

; Call from the main program loop
command_prompt_loop:
  jsr check_acia_no_irq
  cpx #$01
  bne command_prompt_loop_end
  cmp #$0D                            ; check if CR
  bne handle_char
  ldx PROMPT_CHAR_CNT              
  beq reset_prompt                    ; CR typed with no prompt (count 0)
  lda #$0D                            ; print CR to start command
  jsr print_char_acia
  jsr execute_command
  jmp reset_prompt
handle_char:
  cmp #$08                            ; check if backspace
  bne process_char
  lda PROMPT_CHAR_CNT              
  beq command_prompt_loop_end         ; check if prompt is 0 length       
  dec PROMPT_CHAR_CNT
  jsr print_backspace_destruct_acia
  jmp command_prompt_loop_end
process_char:
  ldx PROMPT_CHAR_CNT
  sta PROMPT_START, x                 ; store char (must be upper case)
  inx                                 ; increment prompt char counter
  stx PROMPT_CHAR_CNT
  jmp command_prompt_loop_end
reset_prompt:
  jmp return_os                     
command_prompt_loop_end:
  rts

irq_timer_check_loop:
  jsr check_acia_rx
  cpx #$01
  bne irq_timer_check_loop_end
  cmp #$03                            ; CTRL-C in ASCII
  bne irq_timer_check_loop_end
  jmp return_os
irq_timer_check_loop_end:
  rts

execute_command:
  jsr CHECK_LOAD
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_LIST
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_RUN
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_CLEAR
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_SOFT_RST
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_HARD_RST
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_HELP
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_ADDR
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_BREAK
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_EXEC
  cpy #$01
  beq command_not_found
  jsr CHECK_ABOUT
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_TIME
  cpy #$01
  beq interpret_cmd_end
  jsr CHECK_JIFFIES
  cpy #$01
  beq interpret_cmd_end
;;;;; HIDDEN COMMANDS BELOW HERE
  jsr CHECK_TEST
  cpy #$01
  beq interpret_cmd_end
command_not_found:
  jsr print_invalid_cmd_acia
interpret_cmd_end:
  rts

print_command_prompt_symbol:
  lda #$0D                ; new line
  jsr print_char_acia
  lda #"$"
  jsr print_char_acia
  lda #" "
  jsr print_char_acia
  rts

CHECK_HELP:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"P"
  bne cmd_not_help
  ;
  dex
  lda PROMPT_START, x
  cmp #"L"
  bne cmd_not_help
  ;
  dex
  lda PROMPT_START, x
  cmp #"E"
  bne cmd_not_help
  ;
  dex
  lda PROMPT_START, x
  cmp #"H"
  bne cmd_not_help
  ;
  jsr print_help_acia
  ldy #$01            ; return true
cmd_not_help:
  rts 

CHECK_ADDR:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"R"
  bne cmd_not_addr
  ;
  dex
  lda PROMPT_START, x
  cmp #"D"
  bne cmd_not_addr
  ;
  dex
  lda PROMPT_START, x
  cmp #"D"
  bne cmd_not_addr
  ;
  dex
  lda PROMPT_START, x
  cmp #"A"
  bne cmd_not_addr
  ;
  jsr print_addresses_acia
  ldy #$01            ; return true
cmd_not_addr:
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
  jsr shell_rx_main         ; start loader
  ldy #$01            ; return true
cmd_not_load:
  rts 

CHECK_LIST:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"T"
  bne cmd_not_list
  ;
  dex
  lda PROMPT_START, x
  cmp #"S"
  bne cmd_not_list
  ;
  dex
  lda PROMPT_START, x
  cmp #"I"
  bne cmd_not_list
  ;
  dex
  lda PROMPT_START, x
  cmp #"L"
  bne cmd_not_list
  ;
  jsr print_program
  ldy #$01            ; return true
cmd_not_list:
  rts

CHECK_CLEAR:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"S"
  bne cmd_not_clear
  ;
  dex
  lda PROMPT_START, x
  cmp #"L"
  bne cmd_not_clear
  ;
  dex
  lda PROMPT_START, x
  cmp #"C"
  bne cmd_not_clear
  ;
  jsr clear_terminal
  ldy #$01            ; return true
cmd_not_clear:
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
  lda #1
  sta ENABLE_CRTL_C
  jsr print_run_status_via
  jmp PROGRAM_START_ADDR
cmd_not_run:
  rts 

CHECK_BREAK:
  ldy #$00             ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #$5C             ; \ char
  bne cmd_not_break
  ;
  jmp WOZMON_PRG_START ; start Wozmon, kills the OS, must reset
  ldy #$01             ; return true
cmd_not_break:
  rts 

CHECK_SOFT_RST:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #$1b
  bne cmd_not_soft
  ;
  jmp return_os
cmd_not_soft:
  rts 

CHECK_HARD_RST:
  ldy #$00            ; return false if falls though
  ldx PROMPT_CHAR_CNT
  dex
  lda PROMPT_START, x
  cmp #"T"
  bne cmd_not_hard
  ;
  dex
  lda PROMPT_START, x
  cmp #"S"
  bne cmd_not_hard
  ;
  dex
  lda PROMPT_START, x
  cmp #"R"
  bne cmd_not_hard
  ;
  jmp reset
  ldy #$01            ; return true
cmd_not_hard:
  rts 

CHECK_EXEC:
  ldy #$00            ; return false if falls though
  ldx #$00
  lda PROMPT_START, x
  cmp #"E"
  bne cmd_not_exec
  ;
  inx
  lda PROMPT_START, x
  cmp #"X"
  bne cmd_not_exec
  ;
  inx
  lda PROMPT_START, x
  cmp #"E"
  bne cmd_not_exec
  ;
  inx
  lda PROMPT_START, x
  cmp #"C"
  bne cmd_not_exec
  ;
  jsr exec_address
  ldy #$01            ; return true
cmd_not_exec:
  rts

CHECK_ABOUT:
  ldy #$00            ; return false if falls though
  ldx #$00
  lda PROMPT_START, x
  cmp #"A"
  bne cmd_not_about
  ;
  inx
  lda PROMPT_START, x
  cmp #"B"
  bne cmd_not_about
  ;
  inx
  lda PROMPT_START, x
  cmp #"T"
  bne cmd_not_about
  ;
  jsr print_about_acia
  ldy #$01            ; return true
cmd_not_about:
  rts
