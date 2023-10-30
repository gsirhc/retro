;  A = entry value
print_a_hex_lcd:
  pha
  sed        ;2  @2
  tax        ;2  @4
  and #$0F   ;2  @6
  cmp #9+1   ;2  @8
  adc #$30   ;2  @10
  tay        ;2  @12
  txa        ;2  @14
  lsr        ;2  @16
  lsr        ;2  @18
  lsr        ;2  @20
  lsr        ;2  @22
  cmp #9+1   ;2  @24
  adc #$30   ;2  @26
  cld        ;2  @28
  ;  A = MSN ASCII char
  ;  Y = LSN ASCII char
  jsr print_char_lcd
  tya
  jsr print_char_lcd
  pla
  rts

;; X = Lo Byte, Y = Hi Byte
print_2byte_x_y_decimal_acia:
  stx DEC_VALUE
  sty DEC_VALUE + 1
  jsr print_2byte_decimal_acia
  rts

;; Thanks Ben Eater! https://www.youtube.com/watch?v=v3-a-zqKfgA&t=340s&ab_channel=BenEater
print_2byte_decimal_acia_zero_pad:
  lda DEC_VALUE + 1
  bne print_2byte_decimal_acia
  lda DEC_VALUE
  cmp #10
  bcs print_2byte_decimal_acia
  lda #"0"
  jsr print_char_acia
print_2byte_decimal_acia:
  lda #0
  sta DECIMAL_STR
print_2byte_decimal_divide:
  lda #0
  sta MOD10_VALUE
  sta MOD10_VALUE + 1
  clc
  ldx #16 
print_2byte_decimal_loop;
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
  adc #"0"                         ; convert to ASCII
  jsr push_char_a_decimal_str
  ; if value != 0, keep dividing
  lda DEC_VALUE                    
  ora DEC_VALUE + 1                ; OR lo byte with high byte to determine if done
  bne print_2byte_decimal_divide   ; if A is not 0, keep going
  ldx #0
print_2byte_decimal_str
  lda DECIMAL_STR,x
  beq print_2byte_decimal_end
  jsr print_char_acia
  inx
  jmp print_2byte_decimal_str
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

;; http://6502org.wikidot.com/software-math-intdiv
divide_16bit_by_8bit:
  lda DIVIDE_VALUE + 1
  ldx #8
  asl DIVIDE_VALUE
L1: 
  rol
  bcs L2
  cmp DIVISOR
  bcc L3
L2: 
  sbc DIVISOR
  sec
L3: 
  rol DIVIDE_VALUE
  dex
  bne L1
  sta REMAINDER
  rts

divide_16bit_by_16bit:
  lda #0	                               ;preset remainder to 0
	sta REMAINDER
	sta REMAINDER+1
	ldx #16	                               ;repeat for each bit: ...
divide_16bit_by_16bit_loop:
	asl DIVIDE_VALUE	                     ;dividend lb & hb*2, msb -> Carry
	rol DIVIDE_VALUE + 1	
	rol REMAINDER	                         ;remainder lb & hb * 2 + msb from carry
	rol REMAINDER + 1
	lda REMAINDER
	sec
	sbc DIVISOR	                           ;substract divisor to see if it fits in
	tay	                                   ;lb result -> Y, for we may need it later
	lda REMAINDER + 1
	sbc DIVISOR+1
	bcc divide_16bit_by_16bit_skip	       ;if carry=0 then divisor didn't fit in yet
	sta REMAINDER+1	                       ;else save substraction result as new remainder,
	sty REMAINDER	
	inc DIVIDE_VALUE	                     ;and INCrement result cause divisor fit in 1 times
divide_16bit_by_16bit_skip:	
  dex
	bne divide_16bit_by_16bit_loop	
	rts