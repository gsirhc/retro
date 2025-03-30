;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; VT100 commands, see http://www.braun-home.net/michael/info/misc/VT100_commands.htm
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

CLEAR_TERMINAL:
  PHA
  JSR PRINT_LEADER
  LDA #'2'
  JSR CHROUT
  LDA #'J'              
  JSR CHROUT
  JSR PRINT_LEADER
  LDA #'f'              ; reset cursor to "home" (top-left of screen)
  JSR CHROUT
  PLA
  RTS

PRINT_LEADER:
  LDA #$1B              ; ESC
  JSR CHROUT
  LDA #'['
  JSR CHROUT
  RTS