; CG OS (Chris Gall Operating System)
; Command line RS232 6502 programmer

  .org $8000       ; must preceed the includes

  .include "rom/utils/general.asm"
  .include "rom/hardware/via.asm"
  .include "rom/hardware/acia.asm"
  .include "rom/interpreter/text.asm"
  .include "rom/interpreter/mem.asm"
  .include "rom/interpreter/cmd.asm"

reset:
  ldx #$ff
  txs                    ; init stack
  jsr reset_via
  jsr reset_acia_no_irq
  jsr reset_cmd

loop:
  jsr check_acia_no_irq
  cpx #$01
  bne no_new_char
  jsr process_char
  jsr handle_char_loop
no_new_char:
  jmp loop

irg:
  jsr push_all_stack
  jsr check_acia_irq
  cpx #$00
  bne ignore_irq
  jsr handle_char_irq
ignore_irq
  jsr pull_all_stack
  rti

nmi:
  rti

  .org $fffa
  .word nmi
  .word reset
  .word irg