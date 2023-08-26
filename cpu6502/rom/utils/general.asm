push_all_stack:
  pha             ; store existing values in A / X / Y to stack
  txa
  pha
  tya
  pha 

pull_all_stack:
  pla             ; restore A / X / Y values from stack
  tay
  pla             
  tax
  pla

delayMax:
  ldy #$ff
delay2:
  ldx #$ff
delay1:
  nop
  dex
  bne delay1 
  dey
  bne delay2
  rts