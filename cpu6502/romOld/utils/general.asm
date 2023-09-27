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

delay_max:
  jsr push_all_stack
  ldy #$ff
delay2:
  ldx #$ff
delay1:
  nop
  dex
  bne delay1 
  dey
  bne delay2
  jsr pull_all_stack
  rts

delay_short:
  pha
  txa
  pha
  ldx #$ff
delay_short_loop:
  nop
  dex
  bne delay_short_loop
  pla
  tax
  pla
  rts

debounce_button:
  pha
  txa 
  pha
  ldx #$ff
debounce:         ; debounce button (not idea in an interrupt handler)
  dex
  bne debounce
  pla
  tax
  pla
  rts
