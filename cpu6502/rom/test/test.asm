;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; HIDDEN COMMANDs
;;
;; TEST DEC - test decimal converter
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
CHECK_TEST:
  ldy #$00            ; return false if falls though
  ldx #$00
  lda PROMPT_START, x
  cmp #"T"
  bne cmd_not_test
  ;
  inx
  lda PROMPT_START, x
  cmp #"E"
  bne cmd_not_test
  ;
  inx
  lda PROMPT_START, x
  cmp #"S"
  bne cmd_not_test
  ;
  inx
  lda PROMPT_START, x
  cmp #"T"
  bne cmd_not_test
  ;
  jsr run_tests
  ldy #$01            ; return true
cmd_not_test:
  rts

run_tests:
  jsr test_decimal_output
  jsr test_divide
  jsr test_1_sec_delay
  rts

print_test_sep:
  jsr print_new_line_acia
  lda #"-"
  ldx #10
print_test_sep_loop:
  jsr print_char_acia
  dex
  beq print_test_sep_end
  jmp print_test_sep_loop
print_test_sep_end:
  jsr print_new_line_acia
  rts

test_decimal_output:
  jsr print_test_sep
  lda #$01                          ; 1 (lo byte)
  sta DEC_VALUE
  lda #$00                          ; 1 (hi byte)
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  jsr print_new_line_acia
  ;
  lda #$2F                          ; 47 (lo byte)
  sta DEC_VALUE
  lda #$00                          ; 47 (hi byte)
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  jsr print_new_line_acia
  ;
  lda #$02                          ; 770 (lo byte)
  sta DEC_VALUE
  lda #$03                          ; 770 (hi byte)
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  jsr print_new_line_acia
  ;
  lda #$B8                          ; 1976 (lo byte)
  sta DEC_VALUE
  lda #$07                          ; 1976 (hi byte)
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;
  jsr print_new_line_acia
  lda #$31                          ; 54321 (lo byte)
  sta DEC_VALUE
  lda #$D4                          ; 54321 (hi byte)
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;
  jsr print_new_line_acia
  lda #$05                          ; 5 (lo byte)
  sta DEC_VALUE
  lda #$00                          ; 5 (hi byte)
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia_zero_pad
  ;
  jsr print_new_line_acia
  lda #$0B                          ; 11 (lo byte)
  sta DEC_VALUE
  lda #$00                          ; 11 (hi byte)
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia_zero_pad
  rts

test_divide:
  jsr print_test_sep
  lda #$64                          ; 100 (lo byte)
  sta DIVIDE_VALUE
  lda #$00                          ; 100 (hi byte)
  sta DIVIDE_VALUE + 1
  lda #9                            ; 9 (divisor max is 255 (FF))
  sta DIVISOR
  ;jsr divide
  jsr divide_16bit_by_8bit
  lda DIVIDE_VALUE
  sta DEC_VALUE
  lda #0
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;
  lda #"R"
  jsr print_char_acia
  ;
  lda REMAINDER
  sta DEC_VALUE
  lda #0
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
  jsr print_new_line_acia
  lda #$E8                          ; 1000 (lo byte)
  sta DIVIDE_VALUE
  lda #$03                          ; 1000 (hi byte)
  sta DIVIDE_VALUE + 1
  lda #$0A                          ; 10 (divisor max is 255 (FF))
  sta DIVISOR
  ;jsr divide
  jsr divide_16bit_by_8bit
  lda DIVIDE_VALUE
  sta DEC_VALUE
  lda #0
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;
  lda #"R"
  jsr print_char_acia
  ;
  lda REMAINDER
  sta DEC_VALUE
  lda #0
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
  jsr print_new_line_acia
  lda #$B8                          ; 1976 (lo byte)
  sta DIVIDE_VALUE
  lda #$07                          ; 1976 (hi byte)
  sta DIVIDE_VALUE + 1
  lda #$1B                          ; 27 (divisor max is 255 (FF))
  sta DIVISOR
  ;jsr divide
  jsr divide_16bit_by_8bit
  lda DIVIDE_VALUE
  sta DEC_VALUE
  lda #0
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;
  lda #"R"
  jsr print_char_acia
  ;
  lda REMAINDER
  sta DEC_VALUE
  lda #0
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
  jsr print_new_line_acia
  lda #$FD                          ; 65533 (lo byte)
  sta DIVIDE_VALUE
  lda #$FF                          ; 65533 (hi byte)
  sta DIVIDE_VALUE + 1
  lda #$02                          ; 2 (lo byte)
  sta DIVISOR
  lda #$00                          ; 2 (hi byte)
  sta DIVISOR
  jsr divide_16bit_by_16bit
  lda DIVIDE_VALUE
  sta DEC_VALUE
  lda DIVIDE_VALUE + 1
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  ;
  lda #"R"
  jsr print_char_acia
  ;
  lda REMAINDER
  sta DEC_VALUE
  lda #0
  sta DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  rts

test_1_sec_delay:
  jsr print_test_sep
  jsr print_time
  jsr print_new_line_acia
  jsr print_jiffies
  jsr print_new_line_acia
  lda #"-"
  jsr print_char_acia
  jsr delay_1_sec
  jsr print_new_line_acia
  jsr print_jiffies
  jsr print_new_line_acia
  jsr print_time
  rts