;;;;;;;;;
;; VT100 commands, see http://www.braun-home.net/michael/info/misc/VT100_commands.htm
;;;;;;;

clear_terminal:
  pha
  ; clear screen
  jsr print_leader
  lda #"2"
  jsr print_char_acia
  lda #"J"
  jsr print_char_acia
  ; reset cursor to "home" (top-left of screen)
  jsr print_leader
  lda #"f"
  jsr print_char_acia
  pla
  rts

set_cursor_pos:
  pha
  jsr print_leader
  lda #"p"
  jsr print_char_acia
  txa
  lda #"l"
  jsr print_char_acia
  txa                     ; transfer X to A to print digits
  jsr print_a_digits      ; this uses the X reg so do it first
  lda #";"
  jsr print_char_acia
  lda #"p"
  jsr print_char_acia
  txa
  lda #"c"
  jsr print_char_acia
  txa                     ; transfer Y to A to print digits
  jsr print_a_digits 
  pla
  rts

print_leader:
  lda #$1B              ; ESC
  jsr print_char_acia
  lda #"["
  jsr print_char_acia
  rts
