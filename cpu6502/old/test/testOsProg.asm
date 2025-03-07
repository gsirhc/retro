PRINT_CHAR = $fc00
READ_CHAR  = $fc04
CLEAR_TERM = $fc08
MOVE_CURS  = $fc0c
NEW_LINE   = $fc10
CURS_HOME  = $fc14
EXIT       = $fe00 
HARD_RST   = $fe06
TIME       = $fe10
DELAY_1_S  = $fe18

  .org $0300   ; OAC OS program start addres,s required to run

loop:
  jsr NEW_LINE
  jsr print_help
  lda #$0D
  jsr PRINT_CHAR
  lda #"?"
  jsr PRINT_CHAR
  lda #" "
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
  cmp #"I"
  bne next_4
infloop:
  jmp infloop
next_4:
  cmp #"M"
  bne next_5
  jsr prompt_move_cursor
  jmp loop
next_5:
  cmp #"T"
  bne next_6
  jsr print_time_loop
  jmp loop
next_6:
  lda #" "
  jsr PRINT_CHAR
  lda #"E"
  jsr PRINT_CHAR
  lda #"R"
  jsr PRINT_CHAR
  lda #"R"
  jsr PRINT_CHAR
  jmp loop

  help:  .asciiz "E - Exit, R - Rst, C - cls, M - Cursor, I - Inf. Loop, T - Time Loop"

print_help:
  pha
  txa
  pha
  ldx #0
print_help_loop:
  lda help,x
  beq print_help_end
  jsr PRINT_CHAR
  inx
  jmp print_help_loop
print_help_end:
  pla             
  tax
  pla
  rts

prompt_move_cursor:
  lda #$0D
  jsr PRINT_CHAR
  lda #"R"
  jsr PRINT_CHAR
  lda #"?"
  jsr PRINT_CHAR
  lda #" "
  jsr PRINT_CHAR
wait_r:
  jsr READ_CHAR
  cpx #$00
  beq wait_r
  tax
  lda #$0D
  jsr PRINT_CHAR
  lda #"C"
  jsr PRINT_CHAR
  lda #"?"
  jsr PRINT_CHAR
  lda #" "
  jsr PRINT_CHAR
wait_c:
  jsr READ_CHAR
  cpx #$00
  beq wait_c
  tay
  jsr MOVE_CURS
  rts

print_time_loop:
  jsr CLEAR_TERM
print_time_loop_lp:
  jsr CURS_HOME
  jsr TIME
  jsr READ_CHAR
  cmp #"E"
  beq print_time_loop_end
  jsr DELAY_1_S
  jmp print_time_loop_lp
print_time_loop_end:
  rts