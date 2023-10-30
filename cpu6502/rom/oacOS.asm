; OAC OS (Old Ass Computer Operating System)
; Command line RS232 6502 programmer

  .org $8000       ; must preceed the includes

  ; directories relative to the compiler exe (.sh) file
  .include "rom/mem.asm"
  .include "rom/hardware/via.asm"
  .include "rom/hardware/acia.asm"
  .include "rom/hardware/vt100.asm"
  .include "rom/os/math.asm"
  .include "rom/os/time.asm"
  .include "rom/test/test.asm"
  .include "rom/os/cmd.asm"

reset:
  jsr reset_via_irq
  jsr reset_acia_no_irq
  jsr reset_cmd         
  lda #0 
  sta TIME_JIFFIES
  sta TIME_SECONDS
  sta TIME_MINUTES
  sta TIME_HOURS
  lda #$ff                        ; FF disables ther time
  sta JIFFY_TIMER

return_os:                        ; jmp here to "exit" a program
  lda #0
  sta ENABLE_CRTL_C
  cli                             ; clear interrupt disable
  ldx #$ff                        ; Clear the stack
  txs                             ; x to stack
  jsr soft_reset_cmd 

loop:
  jsr command_prompt_loop
  jmp loop

irg:
  pha
  txa
  pha
  tya
  pha
  lda IFR
  and #%00000010                  ; check CA1
  bne break                       ; bne if AND is (true)
  bit T1CL                        ; read timer lo byte to clear timer interrupt
  inc TIME_JIFFIES                ; increment jiffies (50 hz = 1 sec)
  lda JIFFY_TIMER                 ; max jiffies that can be timed is 254 (@50hz ~= 5 sec)
  cmp #$ff                        ; FF disables the timer, or stops it at FF if a prg doesn't
  beq jiffy_timer_off
  inc JIFFY_TIMER
jiffy_timer_off:
  lda TIME_JIFFIES
  cmp #50
  bne check_ctrl_c
  lda #0
  sta TIME_JIFFIES
  inc TIME_SECONDS                ; increment seconds
  lda TIME_SECONDS
  cmp #60
  bne check_ctrl_c
  lda #0
  sta TIME_SECONDS
  inc TIME_MINUTES                ; increment minutes
  lda TIME_MINUTES
  cmp #60
  bne check_ctrl_c
  lda #0
  sta TIME_MINUTES
  inc TIME_HOURS                  ; increment hours
  lda TIME_HOURS
  cmp #24
  bne check_ctrl_c
  lda #0
  sta TIME_JIFFIES                ; rollover clock to 00:00:00:00 after 24 hours
  sta TIME_SECONDS
  sta TIME_MINUTES
  sta TIME_HOURS
check_ctrl_c:
  lda ENABLE_CRTL_C               ; if ctrl-c enabled, check of it
  beq end_irq
  jsr check_acia_rx
  cpx #$01
  bne end_irq
  cmp #$03                        ; CTRL-C
  bne end_irq
  jmp return_os                   ; basically soft reset the os
break:
  bit PORTA                       ; read port A (PA) to clear interrupt flag
  jmp WOZMON_PRG_START
end_irq:  
  pla
  tay
  pla
  tax
  pla
  rti

nmi:
  rti

; Includes must be in Address order

  ; bios must be here to fit in the propert ROM memory space
  .include "rom/bios.asm"

WOZMON_PRG_START = $ff00 ; not sure this can be changed

  ; programs must be here, in .org order, 
  ; OS is at the beginning of ROM, program at the end
  ; Must set each program .org relative to the others.
  .include "rom/wozmon.asm"

  .org $fffa
  .word nmi
  .word reset
  .word irg
