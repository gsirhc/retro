.data
   start_acia_line1:  .asciiz "CG OS - COMMAND LINE INTERPRETER"
   start_acia_line2:  .asciiz "VT100 TERMINAL - TYPE HELP FOR COMMANDS"
        invalid_cmd:  .asciiz "COMMAND NOT FOUND"
       invalid_addr:  .asciiz "INVALID ADDRESS (MUST BE 2 BYTES: NNNN)"
    not_implemented:  .asciiz "NOT IMPLEMENTED"
  no_program_loaded:  .asciiz "MUST LOAD A PROGRAM TO RUN"
       program_size:  .asciiz "PROGRAM SIZE: 0X" ; size added after here
               help:  .asciiz "LOAD - UPLOAD BIN", $0D, " RUN - RUN PROG", $0D, "LIST - PRINT 256 PROG BYTES", $0D, " CLS - CLEAR SC", $0D, "   ", $5C, " - BREAK", $0D, " ESC - SOFT RST", $0D, " RST - HARD RST", $0D, "EXEC - EXECUTE ADDRESS", $0D, "ADDR - OS COMMAND ADDRESSES"
       os_addresses:  .asciiz "FE00 - EXIT TO OS", $0D, "FE03 - SOFT RESET", $0D, "FE06 - HARD / SYS RESET", $0D, "FE09 - ERASE PROGRAM", $0D, "FEFC - OS LOOP (CHECK FOR CTRL-C)"
                          ;  1234567890123456  
  start_via:        .asciiz "CG OS v1.0"
  input_via:        .asciiz "Prompt          "
  break_via:        .asciiz "Break           "
  load_via:         .asciiz "Load            "
  run_via:          .asciiz "Run             "
  exec_via:         .asciiz "Exec: " ; Address added by command
  current_cmd_via:  .asciiz "Last cmd:"

text_cmd_os_start:
  jsr print_start_via
  jsr print_start_acia_line1
  jsr print_start_acia_line2
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

print_exec_status_via:
  pha
  txa
  pha
  jsr cursorLine2
  ldx #0
print_exec_status_via_loop:
  lda exec_via,x
  beq print_exec_status_via_end
  jsr print_char_lcd
  inx
  jmp print_exec_status_via_loop
print_exec_status_via_end:
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

print_invalid_cmd_acia:
  pha
  txa
  pha
  ldx #0
print_invalid_cmd_acia_loop:
  lda invalid_cmd,x
  beq print_invalid_cmd_acia_end
  jsr print_char_acia
  inx
  jmp print_invalid_cmd_acia_loop
print_invalid_cmd_acia_end:
  pla             
  tax
  pla
  rts

print_invalid_addr_acia:
  pha
  txa
  pha
  ldx #0
print_invalid_addr_acia_loop:
  lda invalid_addr,x
  beq print_invalid_addr_acia_end
  jsr print_char_acia
  inx
  jmp print_invalid_addr_acia_loop
print_invalid_addr_acia_end:
  pla             
  tax
  pla
  rts

print_not_implemented_acia:
  pha
  txa
  pha
  ldx #0
print_not_implemented_acia_loop:
  lda invalid_cmd,x
  beq print_not_implemented_acia_end
  jsr print_char_acia
  inx
  jmp print_not_implemented_acia_loop
print_not_implemented_acia_end:
  pla             
  tax
  pla
  rts

print_no_program_loaded_acia:
  pha
  txa
  pha
  ldx #0
print_no_program_loaded_acia_loop:
  lda no_program_loaded,x
  beq print_no_program_loaded_acia_end
  jsr print_char_acia
  inx
  jmp print_no_program_loaded_acia_loop
print_no_program_loaded_acia_end:
  pla             
  tax
  pla
  rts

print_help_acia:
  pha
  txa
  pha
  ldx #0
print_help_acia_loop:
  lda help,x
  beq print_help_acia_end
  jsr print_char_acia
  inx
  jmp print_help_acia_loop
print_help_acia_end:
  pla             
  tax
  pla
  rts

print_addresses_acia:
  pha
  txa
  pha
  ldx #0
print_addresses_acia_loop:
  lda os_addresses,x
  beq print_addresses_acia_end
  jsr print_char_acia
  inx
  jmp print_addresses_acia_loop
print_addresses_acia_end:
  pla             
  tax
  pla
  rts

print_program_size_acia:
  pha
  txa
  pha
  ldx #0
print_program_size_acia_loop:
  lda program_size,x
  beq print_program_size_acia_end
  jsr print_char_acia
  inx
  jmp print_program_size_acia_loop
print_program_size_acia_end:
  pla             
  tax
  pla
  rts
