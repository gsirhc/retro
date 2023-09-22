  .org $0300

  lda #$76
  sta $01        ; local mem for OS
  lda #"H"
  jsr $feeb
  jmp $fefd      ; exit to os (reversed??)
