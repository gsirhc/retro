.data
  start_acia_line1: .asciiz "CG OS - COMMAND LINE INTERPRETER"
  start_acia_line2: .asciiz "VT100 TERMINAL - COMMANDS: LOAD, RUN, BREAK"
  invalid_cmd_acia: .asciiz "COMMAND NOT FOUND"

                          ;  1234567890123456  
  start_via:        .asciiz "CG OS v1.0"
  input_via:        .asciiz "Input"
  break_via:        .asciiz "Break"
  load_via:         .asciiz "Load"
  run_via:          .asciiz "Run"
  current_cmd_via:  .asciiz "Last cmd:"
  debug_via:        .asciiz "WOZMON: MUST RST"

text_cmd_os_start:
  jsr print_start_via
  jsr print_start_acia_line1
  jsr print_start_acia_line2
  jsr print_input_status_via
  rts

print_start_via:
  pha
  txa
  pha
  ldx #0
print_start_via_loop:
  lda start_via,x
  beq print_start_via_end
  jsr print_char_lcd
  inx
  jmp print_start_via_loop
print_start_via_end:
  pla             
  tax
  pla
  rts

print_start_acia_line1:
  pha
  txa
  pha
  lda #$0D
  jsr print_char_acia
  ldx #0
print_start_acia_line1_loop:
  lda start_acia_line1,x
  beq print_start_acia_line1_end
  jsr print_char_acia
  inx
  jmp print_start_acia_line1_loop
print_start_acia_line1_end:
  pla             
  tax
  pla
  rts

print_start_acia_line2:
  pha
  txa
  pha
  lda #$0D
  jsr print_char_acia
  ldx #0
print_start_acia_line2_loop:
  lda start_acia_line2,x
  beq print_start_acia_line2_end
  jsr print_char_acia
  inx
  jmp print_start_acia_line2_loop
print_start_acia_line2_end:
  pla             
  tax
  pla
  rts

print_input_status_via:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_input_status_via_loop:
  lda input_via,x
  beq print_input_status_via_end
  jsr print_char_lcd
  inx
  jmp print_input_status_via_loop
print_input_status_via_end:
  pla             
  tax
  pla
  rts

print_break_status_via:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_break_status_via_loop:
  lda break_via,x
  beq print_break_status_via_end
  jsr print_char_lcd
  inx
  jmp print_break_status_via_loop
print_break_status_via_end:
  pla             
  tax
  pla
  rts

print_load_status_via:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_load_status_via_loop:
  lda load_via,x
  beq print_load_status_via_end
  jsr print_char_lcd
  inx
  jmp print_load_status_via_loop
print_load_status_via_end:
  pla             
  tax
  pla
  rts

print_run_status_via:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_run_status_via_loop:
  lda run_via,x
  beq print_run_status_via_end
  jsr print_char_lcd
  inx
  jmp print_run_status_via_loop
print_run_status_via_end:
  pla             
  tax
  pla
  rts

print_current_cmd_via:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_current_cmd_via_loop:
  lda current_cmd_via,x
  beq print_current_cmd_via_end
  jsr print_char_lcd
  inx
  jmp print_current_cmd_via_loop
print_current_cmd_via_end:
  pla             
  tax
  pla
  rts

print_debug_via:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_debug_via_loop:
  lda debug_via,x
  beq print_debug_via_end
  jsr print_char_lcd
  inx
  jmp print_debug_via_loop
print_debug_via_end:
  pla             
  tax
  pla
  rts

print_invalid_cmd_acia:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_invalid_cmd_acia_loop:
  lda invalid_cmd_acia,x
  beq print_invalid_cmd_acia_end
  jsr print_char_acia
  inx
  jmp print_invalid_cmd_acia_loop
print_invalid_cmd_acia_end:
  pla             
  tax
  pla
  rts
