;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; BIOS addresses for use by programs
;; Max address space is 8000 - $feff (wozmon) - OS
;;
;; Command categories are broken in to min of 256 byte chunks:
;;    fb00 - fbff - Undocumented commands (256 reserved bytes)
;;        fb00 - Return from Interrupt (rti)
;;    fc00 - fdff - I/O (512 reserved bytes)
;;        fc00 - Print A Reg to terminal
;;        fc04 - Read terminal into A Reg (X Reg = 0 no char, 1 char)
;;        fc08 - Clear Terminal
;;        fc0c - Set Cursor Position (Y - line, X - col)
;;        fc10 - Print new line (CR)
;;        fc14 - Cursor Home - Line
;;        fd00 - Print X (Lo byte), Y (Hi byte) Hex to Decimal ACIA
;;    fe00 - feff - OS commands (256 reserved bytes)
;;        fe00 - Exit Program (Return to OS)
;;        fe03 - Soft Reset System
;;        fe06 - Hard Reset System (same as hitting reset button, mostly)
;;        fe09 - Clear Program
;;        fe10 - print time
;;        fe14 - print jiffies
;;        fe18 - delay 1 second
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;   Undocumented
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

  ; RETURN FROM INTERRUPT (undocumented)
  .org $fb00      ; 1 byte
  rti

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;   I / O
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
  ; Print A Reg to terminal
  .org $fc00
  jsr print_char_acia
  rts

  ; Read terminal into A Reg
  .org $fc04
  jsr check_acia_no_irq
  rts

  ; Clear Terminal
  .org $fc08
  jsr clear_terminal
  rts

  ; Set Cursor Position Y, X coord, (line, col)
  .org $fc0c
  jsr set_cursor_pos
  rts

  .org $fc10
  jsr print_new_line_acia
  rts

  .org $fc14
  jsr set_cursor_home_line
  rts

  .org $fd00
  jsr print_2byte_x_y_decimal_acia
  rts

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;   OS Commands
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
  ; Exit Program (Return to OS)
  .org $fe00      ; 3 bytes
  jmp return_os

  ; Soft Reset System
  .org $fe03      ; 3 bytes
  jmp return_os

  ; Hard Reset System (clears stack and program)
  .org $fe06      ; 3 bytes
  jmp reset

  ; Clear Program (UPDATE RESET_CMD IF CHANGING ADDRESS)
  .org $fe09      ; 6 bytes
  jsr print_no_program_loaded_acia
  jmp return_os

  .org $fe10      ; 3 bytes
  jsr print_time
  rts

  .org $fe14      ; 3 bytes
  jsr print_jiffies
  rts

  .org $fe18      ; 3 bytes
  jsr delay_1_sec
  rts
