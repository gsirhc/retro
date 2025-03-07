;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; VT100 commands, see http://www.braun-home.net/michael/info/misc/VT100_commands.htm
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

clear_terminal:
  pha
  ; clear screen
  jsr print_leader
  lda #'2'
  jsr write_acia
  lda #'J'
  jsr write_acia
  ; reset cursor to "home" (top-left of screen)
  jsr print_leader
  lda #'f'
  jsr write_acia
  pla
  rts

set_cursor_pos:
  pha
  jsr print_leader
  tya                     ; line
  jsr write_acia
  lda #';'                ; separator
  jsr write_acia
  txa                     ; row
  jsr write_acia
  lda #'H'                ; not sure, part of VT100 spec
  jsr write_acia
  pla
  rts

set_cursor_home_line:
  pha
  jsr print_leader
  lda #'H'                ; not sure, part of VT100 spec
  jsr write_acia
  pla
  rts

print_leader:
  lda #$1B              ; ESC
  jsr write_acia
  lda #'['
  jsr write_acia
  rts
