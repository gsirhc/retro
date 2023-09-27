PRINT_CHAR = $fc00
READ_CHAR  = $fc04
CLEAR_TERM = $fc08
EXIT       = $fe00 
HARD_RST   = $fe06

  .org $0300

loop:
  lda #$0D
  jsr PRINT_CHAR
  lda #"?"
  jsr PRINT_CHAR
wait_char:
  jsr READ_CHAR
  cpx #$00
  beq wait_char
  cmp #"C"
  bne next_1
  jsr CLEAR_TERM
  jmp loop
next_1:
  cmp #"E"
  bne next_2
  jmp EXIT
next_2:
  cmp #"R"
  bne next_3
  jmp HARD_RST
next_3:
  lda #"B"
  jsr PRINT_CHAR
  lda #"A"
  jsr PRINT_CHAR
  lda #"D"
  jsr PRINT_CHAR
  jmp loop
