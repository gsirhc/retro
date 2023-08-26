.data
  start_acia_line1: .asciiz "CG OS - Command Line Interpreter"
  start_via:        .asciiz "CG OS v1.0"
  input_via:        .asciiz "Input"

text_cmd_os_start:
  jsr print_start_acia_line1
  jsr print_start_via
  jsr print_input_status_via
  rts

print_start_acia_line1:
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
  rts

print_start_via:
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
  rts

print_input_status_via:
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
  rts