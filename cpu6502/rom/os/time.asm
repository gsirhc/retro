CHECK_TIME:
  ldy #$00            ; return false if falls though
  ldx #$00
  lda PROMPT_START, x
  cmp #"T"
  bne cmd_not_time
  ;
  inx
  lda PROMPT_START, x
  cmp #"I"
  bne cmd_not_time
  ;
  inx
  lda PROMPT_START, x
  cmp #"M"
  bne cmd_not_time
  ;
  inx
  lda PROMPT_START, x
  cmp #"E"
  bne cmd_not_time
  jsr print_time_cmd
  ldy #$01            ; return true
cmd_not_time:
  rts

CHECK_JIFFIES:
  ldy #$00            ; return false if falls though
  ldx #$00
  lda PROMPT_START, x
  cmp #"J"
  bne cmd_not_jiff
  ;
  inx
  lda PROMPT_START, x
  cmp #"I"
  bne cmd_not_jiff
  ;
  inx
  lda PROMPT_START, x
  cmp #"F"
  bne cmd_not_jiff
  ;
  inx
  lda PROMPT_START, x
  cmp #"F"
  bne cmd_not_jiff
  jsr print_jiffies_cmd
  ldy #$01            ; return true
cmd_not_jiff:
  rts

print_time_cmd:
  jsr print_time_since_startup_acia
print_time:
  lda #0
  sta DEC_VALUE + 1               ; default hi byte to 0 for all times
  lda TIME_HOURS
  sta DEC_VALUE
  jsr print_2byte_decimal_acia_zero_pad
  ;
  lda #":"
  jsr print_char_acia
  lda TIME_MINUTES
  sta DEC_VALUE
  jsr print_2byte_decimal_acia_zero_pad
  ;
  lda #":"
  jsr print_char_acia
  lda TIME_SECONDS
  sta DEC_VALUE
  jsr print_2byte_decimal_acia_zero_pad
  rts

print_jiffies_cmd:
  jsr print_jiffies_per_min_acia
print_jiffies:
  lda #0
  sta DEC_VALUE + 1
  lda TIME_JIFFIES
  sta DEC_VALUE
  jsr print_2byte_decimal_acia
  rts

delay_1_sec:
  lda #0
  sta JIFFY_TIMER
delay_1_sec_wait_loop:
  lda JIFFY_TIMER
  cmp #50                                       ; number of jiffies to wait (50 = 1 sec.)
  bcs delay_1_sec_end                           ; > or = as a fail safe
  jmp delay_1_sec_wait_loop
delay_1_sec_end:
  rts