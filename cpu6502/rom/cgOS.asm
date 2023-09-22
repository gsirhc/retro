; CG OS (Chris Gall Operating System)
; Command line RS232 6502 programmer

  .org $8000       ; must preceed the includes

  .include "rom/mem.asm"
  .include "rom/utils/general.asm"
  .include "rom/hardware/via.asm"
  .include "rom/hardware/acia.asm"
  .include "rom/interpreter/text.asm"
  .include "rom/interpreter/cmd.asm"
  .include "rom/cmd/load.asm"

reset:
  ldx #$ff
  txs                    ; init stack
  jsr reset_via_irq
  jsr reset_acia_no_irq
  jsr reset_cmd          
  cli                    ; clear interrupt disable

return_os:               ; jmp here to "exit" a program
  jsr soft_reset_cmd 

loop:
  jsr command_prompt_loop
  jmp loop

irg:
  jsr print_break_status_via
  jmp WOZMON_PRG_START ; This kills the OS but allows for debugging, must reset
  rti

nmi:
  rti

bios:
  .org $fede            ; 13 bytes
  lda #"H"              ; 2 bytes
  jsr print_char_acia  ; 3 bytes
  lda #"I"              ; 2 bytes
  jsr print_char_acia  ; 3 bytes
  jmp return_os         ; 3 bytes

  .org $feeb      ; 4 bytes
  jsr print_char_acia
  rts

  .org $feef      ; 3 bytes
  jmp reset
  .org $fefc      ; 1 byte
  rti
  .org $fefd      ; 3 bytes
  jmp return_os

WOZMON_PRG_START = $ff00 ; not sure this can be changed

  ; programs must be here, in .org order, 
  ; OS is at the beginning of ROM, program at the end
  ; Must set each program .org relative to the others.
  .include "rom/programs/wozmon.asm"

  .org $fffa
  .word nmi
  .word reset
  .word irg
