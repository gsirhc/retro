;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; VT100 commands, see http://www.braun-home.net/michael/info/misc/VT100_commands.htm
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

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
  tya                     ; line
  jsr print_char_acia
  lda #";"                ; separator
  jsr print_char_acia
  txa                     ; row
  jsr print_char_acia
  lda #"H"                ; not sure, part of VT100 spec
  jsr print_char_acia
  pla
  rts

set_cursor_home_line:
  pha
  jsr print_leader
  lda #"f"                ; not sure, part of VT100 spec
  jsr print_char_acia
  pla
  rts

print_leader:
  lda #$1B              ; ESC
  jsr print_char_acia
  lda #"["
  jsr print_char_acia
  rts
