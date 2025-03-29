;; Thanks Ben Eater! https://www.youtube.com/watch?v=v3-a-zqKfgA&t=340s&ab_channel=BenEater
FM2BYTEDECZP:
  lda #'0'
  jsr push_char_a_decimal_str
  lda DEC_VALUE + 1
  bne print_2byte_decimal_acia
  lda DEC_VALUE
  cmp #10
  bcs print_2byte_decimal_acia
  lda #'0'
  jsr print_char_lcd                ; HACK only works with LCD (figure out how to prepend DECIMAL_STR)
print_2byte_decimal_acia:
  lda #0
  sta DECIMAL_STR
print_2byte_decimal_divide:
  lda #0
  sta MOD10_VALUE
  sta MOD10_VALUE + 1
  clc
  ldx #16 
print_2byte_decimal_loop:
  ; Rotate quotient and remainder
  rol DEC_VALUE
  rol DEC_VALUE + 1
  rol MOD10_VALUE
  rol MOD10_VALUE + 1
  ;  a,y = dividend - divisor
  sec
  lda MOD10_VALUE
  sbc #10
  tay                              ; save lo byte
  lda MOD10_VALUE + 1
  sbc #0
  bcc ignore_result                ; branch if dividend < divisor
  sty MOD10_VALUE
  sta MOD10_VALUE + 1
ignore_result:
  dex
  bne print_2byte_decimal_loop
  rol DEC_VALUE                    ; shift in the last bit of the quotient
  rol DEC_VALUE + 1
  lda MOD10_VALUE                  ; the digit to print
  clc
  adc #'0'                         ; convert to ASCII
  jsr push_char_a_decimal_str
  ; if value != 0, keep dividing
  lda DEC_VALUE                    
  ora DEC_VALUE + 1                ; OR lo byte with high byte to determine if done
  bne print_2byte_decimal_divide   ; if A is not 0, keep going
print_2byte_decimal_end:
   rts

;; 5 -> 51234n  n = null
push_char_a_decimal_str:
  pha                                   ; push new char to stack
  ldy #0
push_char_a_decimal_str_loop:
  lda DECIMAL_STR,y                    
  tax                                   ; get next char from string into into X
  pla
  sta DECIMAL_STR,y                     ; put new char to start of string
  iny
  txa                                   
  pha                                   ; push prev char onto the stack 
  bne push_char_a_decimal_str_loop
  pla                                   ; pull of null terminator
  sta DECIMAL_STR,y                     ; add to string
  tya
  rts