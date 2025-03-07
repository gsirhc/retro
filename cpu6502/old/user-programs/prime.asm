;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; https://www.geeksforgeeks.org/calculate-pi-using-nilkanthas-series/
;;
;; https://en.wikipedia.org/wiki/Primality_test
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

N          = $50   ; 2 bytes
L          = $52   ; 2 bytes
DVAL       = $60   ; 2 bytes
DIV        = $62   ; 2 bytes
REM        = $64   ; 2 bytes

PRINT_CHAR = $fc00
PRINT_DEC  = $fd00
EXIT       = $fe00 

  .org $0300

  lda #2                                 ; start at 2 (0, 1 are not prime)
  sta N                        
  lda #0
  sta N + 1
MAINLOOP:
  lda #2                                 ; loop from 2 to N to check if N is divisible by L
  sta L
  lda #0
  sta L + 1
MODLOOP:
  lda N
  sta DVAL
  lda N + 1
  sta DVAL + 1
  lda L
  sta DIV
  lda L + 1
  sta DIV + 1
  jsr DIVIDE                             ; divide N by L
  lda REM
  cmp #0                                 ; if remainder is 0, number is primt
  bne CHKLP
  lda REM + 1
  cmp #0
  beq NEX_NUM
CHKLP:
  cmp #0
  ldx L                                  ; check if N = L + 1, if yes, try next number 
  ldy L + 1
  inx
  bne CHKEND
  iny
CHKEND:
  cpx N                                  
  bne NEXT_LOOP
  cpy N + 1
  beq ISPRIME
NEXT_LOOP:
  inc L                                  ; inc L (loop counter)
  bne MODLOOP
  inc L+1
  bne MODLOOP                            ; incase L wraps to 0
ISPRIME:
  ldx N
  ldy N + 1
  jsr PRINT_DEC                          ; print the number if its prime
  lda #" "
  jsr PRINT_CHAR
NEX_NUM:
  inc N
  bne MAINLOOP
  inc N + 1
  bne MAINLOOP                           ; if number is 65535 (max 2 bytes), end program
  jmp EXIT

DIVIDE:
  lda #0	                               ;preset remainder to 0
	sta REM
	sta REM+1
	ldx #16	                               ;repeat for each bit: ...
DIVIDE_LOOP:
	asl DVAL	                             ;dividend lb & hb*2, msb -> Carry
	rol DVAL + 1	
	rol REM	                               ;remainder lb & hb * 2 + msb from carry
	rol REM + 1
	lda REM
	sec
	sbc DIV   	                           ;substract divisor to see if it fits in
	tay	                                   ;lb result -> Y, for we may need it later
	lda REM + 1
	sbc DIV+1
	bcc DIVIDE_SKIP               	       ;if carry=0 then divisor didn't fit in yet
	sta REM + 1	                           ;else save substraction result as new remainder,
	sty REM	
	inc DVAL        	                     ;and INCrement result cause divisor fit in 1 times
DIVIDE_SKIP:	
  dex
	bne DIVIDE_LOOP	
	rts