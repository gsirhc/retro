.data
  start_acia_line1: .asciiz "CG OS - Command Line Interpreter"
                          ;  1234567890123456  
  start_via:        .asciiz "CG OS v1.0"
  input_via:        .asciiz "Input"
  debug_via:        .asciiz "WOZMON: MUST RST"

text_cmd_os_start:
  jsr print_start_acia_line1
  jsr print_start_via
  jsr print_input_status_via
  rts

print_start_acia_line1:
  pha
  txa
  pha
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
