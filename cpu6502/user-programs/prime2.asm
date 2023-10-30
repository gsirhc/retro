;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; https://www.geeksforgeeks.org/calculate-pi-using-nilkanthas-series/
;;
;; https://en.wikipedia.org/wiki/Primality_test
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

N            = $50   ; 2 bytes
L            = $52   ; 2 bytes
DVAL         = $60   ; 2 bytes
DIV          = $62   ; 2 bytes
REM          = $64   ; 2 bytes
TMP          = $66   ; 2 btyes
ROOT         = $68   ; 2 bytes

PRINT_CHAR   = $fc00
PRINT_DEC    = $fd00
EXIT         = $fe00 

  .org $0300

  lda #"2"
  jsr PRINT_CHAR
  lda #" "
  jsr PRINT_CHAR
  lda #"3"
  jsr PRINT_CHAR
  lda #" "
  jsr PRINT_CHAR

  lda #5                                 ; start at 5 (0-4 are handled)
  sta N                        
  lda #0
  sta N + 1

MAINLOOP:
  jsr CHECKDIV23  
  cpy #1
  beq NEXTNUM
  jsr DIVSQRT            
NEXTNUM:
  inc N
  bne MAINLOOP
  inc N + 1
  bne MAINLOOP
  jmp EXIT

CHECKDIV23:
  ldy #0                      ; return false
  ldx #1
CHECKDIV23LP
  inx
  cpx #4                      ; only check 2 and 3
  beq CHECKDIV23END
  lda N
  sta DVAL
  lda N + 1
  sta DVAL + 1
  lda #0
  sta DIV + 1
  lda #2
  sta DIV
  txa
  pha
  tya
  pha
  jsr DIVIDE
  pla
  tay
  pla
  tax
  lda REM + 1
  bne CHECKDIV23LP
  lda REM
  bne CHECKDIV23LP
  cpx #2
  bne not2
  ;jsr PRINTDIV2
  jmp ret1
not2:
  ;jsr PRINTDIV3
ret1:
  ldy #1                                 ; return true
CHECKDIV23END:
  rts

DIVSQRT:
  lda N
  sta DVAL
  lda N + 1
  sta DVAL + 1
  jsr SQROOT
  inc ROOT
  bne ROOTSTARTLP
  inc ROOT + 1                           ; loop sq root + 1
ROOTSTARTLP:
  lda #5                                 ; loop from 5
  sta L
  lda #0
  sta L + 1
ROOTLOOP:
  ; check N % L
  lda N
  sta DVAL
  lda N + 1
  lda DVAL + 1
  lda L
  sta DIV
  lda L + 1
  sta DIV + 1
  jsr DIVIDE  
  lda REM
  cmp #0
  bne CHKNMP2
  lda REM + 1
  cmp #0
  beq DIVSQRTNOTPRIME
CHKNMP2:
  ; check N % L+2
  lda N
  sta DVAL
  lda N + 1
  sta DVAL + 1
  lda L
  sta DIV
  lda L + 1
  sta DIV + 1
  lda DIV
  clc
  adc #2
  sta DIV
  bcc NP2
  inc DIV + 1
NP2:
  jsr DIVIDE  
  lda REM
  cmp #0
  bne CHECKROOTDONE
  lda REM + 1
  cmp #0
  beq DIVSQRTNOTPRIME
CHECKROOTDONE:
  ; Just need to check the lo byte because it'll never exceed 255 (sqrt(2^16 - 1) = 255)
  lda L
  cmp N
  bcc NEXTLP                           ; L >= N
  beq NEXTLP 
ISPRIME:
  ; is prime
  ;jsr PRINTISPRIME
  jmp DIVSQRTEND  
NEXTLP:
  clc
  lda L
  adc #6                               ; add 6 (see algorythm)    
  sta L
  bcc ROOTLOOP
  inc L + 1
  jmp ROOTLOOP
DIVSQRTNOTPRIME:
  jsr PRINTDIVL
DIVSQRTEND:
  rts

DIVIDE:
  lda #0	                               ; preset remainder to 0
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

;; http://6502org.wikidot.com/software-math-sqrt
SQROOT:
  lda #0
	sta ROOT
  sta REM
  ldx #8
SQROOTL1: 
  sec
  lda DVAL + 1
  sbc #$40
  tay
  lda REM
  sbc ROOT
  bcc SQROOTL2
  sty DVAL + 1
  sta REM
SQROOTL2: 
  rol ROOT
  asl DVAL
  rol DVAL + 1
  rol REM
  asl DVAL
  rol DVAL + 1
  rol REM
  dex
  bne SQROOTL1
  rts

PRINTISPRIME:
  ldx N
  ldy N + 1
  jsr PRINT_DEC                          
  ;lda #" "
  ;jsr PRINT_CHAR
  rts

PRINTDIV2:
  jsr PRINTISPRIME
  lda #"-"
  jsr PRINT_CHAR
  lda #"2"
  jsr PRINT_CHAR
  lda #" "
  jsr PRINT_CHAR
  rts

PRINTDIV3:
  jsr PRINTISPRIME
  lda #"-"
  jsr PRINT_CHAR
  lda #"3"
  jsr PRINT_CHAR
  lda #" "
  jsr PRINT_CHAR
  rts

PRINTDIVL:
  jsr PRINTISPRIME
  lda #"-"
  jsr PRINT_CHAR
  jsr PRINTL
  lda #" "
  jsr PRINT_CHAR
  rts

PRINTL:
  ldx L
  ldy L + 1
  jsr PRINT_DEC                          
  rts

PRINTSQROOT:
  ldx ROOT
  ldy #0
  jsr PRINT_DEC                          
  lda #" "
  jsr PRINT_CHAR
  rts

PRINTDIVISIONRES:
  ldx DVAL
  ldy DVAL + 1
  jsr PRINT_DEC                          
  lda #"/"
  jsr PRINT_CHAR
  ldx DIV
  ldy DIV + 1
  jsr PRINT_DEC                          
  lda #"R"
  jsr PRINT_CHAR
  ldx REM
  ldy REM + 1
  jsr PRINT_DEC                          
  lda #" "
  jsr PRINT_CHAR
  rts