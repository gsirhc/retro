;; Apple-1 Pi
;;
;; Egan Ford (egan@sense.net) with a lot of optimizations and pointers from
;; Anton Treuenfels (atreuenfels@earthlink.net)
;;
;; Sept, 8 2012
;;
;; Apple-1 Pi can compute 1000 decimal digits of Pi.
;;
;; All Apple-1 Pi computation is base 256 and then converted to base 10 for
;; display (technically base 100, but they both display the same).  Arrays
;; of bytes are used to represent multiprecision numbers.  In the comments
;; below "array" refers to a big endian multiprecision number.
;;


;; better docs for bintobcd, etc...
;; review 6800 optimizations, e.g. clear after print
;;   try to get digit from A?
;; use echo, not cout


;; Apple-1 variables/vectors

;cout	=	$FFEF		; character out sub
;prbyte	=	$FFDC		; print a hex byte
;warm	=	$FF1F		; back to monitor

;; my global variables/vectors

ptr	=	$80		; $0-$1 16 bit generic pointer
ptr_mp	=	$82		; $2-$3 16 bit generic pointer
a32	=	$84		; $4-$7 32 bit number
carry_mp=	$84		; $4-$4  8 bit multiprecision carry
				;          32/16 div
dividend=	$84		; $4-$7 32 bit
remainder=	$84		; $4-$5 16 bit
quotient=	$86		; $6-$7 16 bit
divisor	=	$88		; $8-$9 16 bit
temp	=	$87		; $7-$7  8 bit for mult100 code
mtmp	=	$88		; $8-$8  8 bit for mult100 code

				;          backup regs
yreg	=	$8A		; $A-$A  8 bit
xreg	=	$8B		; $B-$B  8 bit

RUNAD = $2e0

bin_len	=	418		; ceil(1001 / log(256)) + 1 (+ 1 if odd) = 418
dec_len	=	1000		; 1000 decimal digits


;; start of global macros
.include	"pimacros.m"
;; end of macros

;; start of real code
.CODE
.proc main

begin:
	jsr	crout		; print CR
	jsr	crout		; print CR

	sprint	"1000 DIGITS OF PI = "
cnton 1
	jsr	pi
cntoff 1
	mprint	mp_a		; print it (note mp_a hosed)
	jsr	crout		; print CR
	;jmp	warm
        jmp getch
        rts
.endproc

ICBAL = $344
ICCOM = $342
ICBLL = $348
PUT = $0B
GET = $07
CIOV = $E456

; http://www.easy68k.com/paulrsm/6502/MON.TXT
prbyte:
        pha
        lsr
        lsr
        lsr
        lsr
        jsr prhexz
        pla
        and #$0F
prhexz:
        ora #$B0
        cmp #$BA
        bcc cout
        adc #$06
cout:
        eor #$80
        sta buffer
        txa
        pha
        tya
        pha
        lda #<buffer
        sta ICBAL
        lda #>buffer
        sta ICBAL+1
        lda #PUT
        sta ICCOM
        lda #1
        sta ICBLL
        lda #0
        sta ICBLL+1
        ldx #0
        jsr CIOV
        pla
        tay
        pla
        tax
        rts
getch:
        lda #<buffer
        sta ICBAL
        lda #>buffer
        sta ICBAL+1
        lda #GET
        sta ICCOM
        lda #1
        sta ICBLL
        lda #0
        sta ICBLL+1
        ldx #0
        jsr CIOV
        rts 
buffer:
        .res 1


;; end of main code, start of non-zp allocations, consider move to zp

arraylen:
	.byte	<bin_len, >bin_len		; length of base 256 array
arrayend:
	.byte	<(bin_len-1), >(bin_len-1)	; length of base 256 array
declen:	.byte	<dec_len, >dec_len		; decimal length of digits
mp_a:	.byte	<mp_a_array, >mp_a_array	; mp pointer
mp_b:	.byte	<mp_b_array, >mp_b_array	; mp pointer
mp_x:	.byte	<mp_x_array, >mp_x_array	; mp pointer
mp_y:	.byte	<mp_y_array, >mp_y_array	; mp pointer



;; start of subs

; Description: Compute pi using the Gregory expansion of Machin's arctan
;              formula and save in array (mp_a) (hardcoded--bad).
;
;   pi = 4 * (4 *      atan(1/5)           -           atan(1/239)          )
;
;
;   __      /   / 1     1      1       \       / 1       1        1        \\
;   || = 4 | 4 |  - - ---- + ---- - ... |  -  | --- - ------ + ------ - ... ||
;          |   |  5      3      5       |     | 239        3        5       ||
;           \   \     3x5    5x5       /       \      3x239    5x239       //
;
;
; Input:       None
;
; Output:      (mp_a) = pi = 4 * (4 * atan(1/5) - atan(1/239))
;
; Clobbered:   A, X, Y
;
; Calls:       matan/atan_mp, masl/asl_mp, msub/sub_mp

pi:	matan	mp_a,#5		; a = atan(1/5)
	masl	mp_a		; a = a * 4
	masl	mp_a
	matan	mp_b,#239	; b = atan(1/239)
	msub	mp_a,mp_b	; a = a - b
	masl	mp_a		; a = a * 4
	masl	mp_a
	rts


; Description: Compute arctan(1/N) using the Gregory expansion of Machin's
;              arctan formula and save in array (ptr_a).
;
;
;                            / 1     1      1       \
;              arctan(N) =  |  - - ---- + ---- - ... |
;                           |  N      3      5       |
;                            \     3xN    5xN       /
;
;
; Input:       Y/A (hi/lo) pointer to array
;              X = N (8 bit)
;
; Output:      Y/A (hi/lo) pointer to array = arctan(1/N)
;
; Clobbered:   A, X, Y
;
; Calls:       mset/set_mp, mdiv/div_mp, madd/add_mp, msub/sub_mp,
;              mdiv16/div16_mp, mcopy/copy_mp
;
; Globals:     mp_x (16-bit), mp_y (16-bit)
;
; C Algorithm:
;
; void atanbig(bignum A, unsigned short x)
; {
;     bignum X, Y;
;     unsigned short n = 1;
; 
;     setbig(X, 1, 0);
;     divbig(X, x);
;     copybig(A, X);
;     x *= x;
;     while (1) {
;         n += 2;
;         divbig(X, x);
;         copybig(Y, X);
;         if (!divbig(Y, n))      // dividend = 0
;             break;
;         if (n & 2)
;             subbig(A, Y);
;         else
;             addbig(A, Y);
;     }
; }
;
; Locals (not really):

;regx	=	$C		;  $C- $C  8 bit
;x2	=	$D		;  $D- $D  8 bit, x^2 if x < 16
;n	=	$E		;  $E- $F 16 bit
;ptr_a	=	$10		; $10-$11 16 bit

regx:	.byte	$0
x2:	.byte	$0		; x^2 if x < 16
n:	.byte	$0, $0
ptr_a:	.byte	$0, $0
squares:
	.byte	0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100
	.byte	121, 144, 169, 196, 225

atan_mp:
	sta	ptr_a		; store ptr lo from A
	sty	ptr_a+1		; store ptr hi from Y
	stx	regx		; save x

	lda	#0		; n = 1
	sta	n
	sta	n+1
	inc	n		; n = 1 little endian

	mset	mp_x,#1		; mp_x = 1
	mdiv	mp_x,regx	; mp_x /= regx
	mcopy	ptr_a,mp_x	; ptr_a = mp_x

	ldx	regx		; square x if x < 16
	stx	x2
	cpx	#16
	bcs	atan_mp_loop
	lda	squares,x	; load from squares table
	sta	x2		;   and save

atan_mp_loop:			; main loop
				; n = n + 2
	inc	n		; n now even
	bne	:+		; n not zero
	inc	n+1		; n rolled to 0, inc n (hi)
:	inc	n		; n to be odd, save to inc (cannot roll from
				;   even to odd)
	ldx	regx		; x = x / regx ^ 2
	cpx	#16
	bcc	:+		; x already x*x, one div required
	mdiv	mp_x,x2		; x >= 16, then do two div
:	mdiv	mp_x,x2		;   (faster than 16 bit div)

	mcopy	mp_y,mp_x	; y = x

	ldx	n+1		; if n < 256 then div else div16
	bne	:+		; >= 256 use 16 bit div
	mdiv	mp_y,n		; < 256 use 8 bit div
	bcs	atan_mp_end	; dividend = 0, done
	bcc	:++		; still working on it
:	mdiv16	mp_y,n		; div16
	bcs	atan_mp_end	; dividend = 0, done

:	lda	n		; add or sub
	and	#2
	beq	:+		; add it
	msub	ptr_a,mp_y	; a = a - y 
	jmp	atan_mp_loop
:	madd	ptr_a,mp_y	; a = a + y 
	jmp	atan_mp_loop
atan_mp_end:
	rts


; Description: Multiprecision add:  a = a + b
;
; Input:       Y/A (hi/lo) pointer to array (a)
;              ptr_mp set by macro to point to array (b)
;
; Output:      a = a + b
;
; Clobbered:   A, X, Y
;
; Globals:     arraylen (16-bit), arrayend (16-bit), zp ptr (16-bit),
;              zp ptr_mp (16-bit)

add_mp:
	sta	ptr		; store ptr lo from A
	tya
	clc
	adc	arrayend+1	; add number of pages since we have to go
	sta	ptr+1		;   backwards for add/sub/asl
	lda	ptr_mp+1
	clc
	adc	arrayend+1	; add number of pages since we have to go
	sta	ptr_mp+1	;   backwards for add/sub/asl
	ldx	arrayend+1	; full pages
	ldy	arrayend	; partial
	clc			; clear carry for many adds
	;tya
	;and	#%10		; Duff's Device.   We know that arraylen is
	;beq	@1		;   even (arrayend odd), so just need to
	;bne	@3		;   check for 2nd bit (odd or even # of pairs).
	bcc	@1		; arraylen hard coded, no need to guess
:	dex
	dec	ptr+1		; previous page of 256
	dec	ptr_mp+1	; previous page of 256
:	dey
@3:	lda	(ptr),y		; a	; do bytes 3,4,...,255
	adc	(ptr_mp),y	; + b
	sta	(ptr),y		; a = a + b
	dey
@2:	lda	(ptr),y		; a	; do bytes 2,4,...,254
	adc	(ptr_mp),y	; + b
	sta	(ptr),y		; a = a + b
	dey
@1:	lda	(ptr),y		; a	; do bytes 1,5,...,253
	adc	(ptr_mp),y	; + b
	sta	(ptr),y		; a = a + b
	dey
@0:	lda	(ptr),y		; a	; do bytes 0,4,...,252
	adc	(ptr_mp),y	; + b
	sta	(ptr),y		; a = a + b
	tya
	bne	:-
	txa
	bne	:--
	rts


; Description: Multiprecision subtract:  a = a - b
;
; Input:       Y/A (hi/lo) pointer to array (a)
;              ptr_mp set by macro to point to array (b)
;
; Output:      a = a - b
;
; Clobbered:   A, X, Y
;
; Globals:     arraylen (16-bit), arrayend (16-bit), zp ptr (16-bit),
;              zp ptr_mp (16-bit)

sub_mp:
	sta	ptr		; store ptr lo from A
	tya
	clc
	adc	arrayend+1	; add number of pages since we have to go
	sta	ptr+1		;   backwards for add/sub/asl
	lda	ptr_mp+1
	clc
	adc	arrayend+1	; add number of pages since we have to go
	sta	ptr_mp+1	;   backwards for add/sub/asl
	ldx	arrayend+1	; full pages
	ldy	arrayend	; partial
	sec			; sec carry for many subs
	;tya
	;and	#%10		; Duff's Device.   We know that arraylen is
	;beq	@1		;   even (arrayend odd), so just need to
	;bne	@3		;   check for 2nd bit (odd or even # of pairs).
	bcs	@1		; arraylen hard coded, no need to guess
:	dex
	dec	ptr+1		; previous page of 256
	dec	ptr_mp+1	; previous page of 256
:	dey
@3:	lda	(ptr),y		; a	; do bytes 3,7,...,255
	sbc	(ptr_mp),y	; - b
	sta	(ptr),y		; a = a - b
	dey
@2:	lda	(ptr),y		; a	; do bytes 2,6,...,254
	sbc	(ptr_mp),y	; - b
	sta	(ptr),y		; a = a - b
	dey
@1:	lda	(ptr),y		; a	; do bytes 1,5,...,253
	sbc	(ptr_mp),y	; - b
	sta	(ptr),y		; a = a - b
	dey
@0:	lda	(ptr),y		; a	; do bytes 0,4,...,252
	sbc	(ptr_mp),y	; - b
	sta	(ptr),y		; a = a - b
	tya
	bne	:-
	txa
	bne	:--
	rts


; Description: Multiprecision shift left:  a = a * 2
;
; Input:       Y/A (hi/lo) pointer to array (a)
;
; Output:      a = a * 2
;
; Clobbered:   A, X, Y
;
; Globals:     arraylen (16-bit), arrayend (16-bit), zp ptr (16-bit),

asl_mp:
	sta	ptr		; store ptr lo from A
	tya
	clc
	adc	arrayend+1	; add number of pages since we have to go
	sta	ptr+1		;   backwards for add/sub/asl
	ldx	arrayend+1	; full pages
	ldy	arrayend	; partial
	clc			; clear carry for many rols
	;tya
	;and	#%10		; Duff's Device.   We know that arraylen is
	;beq	@1		;   even (arrayend odd), so just need to
	;bne	@3		;   check for 2nd bit (odd or even # of pairs).
	bcc	@1		; arraylen hard coded, no need to guess
:	dex
	dec	ptr+1		; previous page of 256
:	dey
@3:	lda	(ptr),y		; do bytes 3,7,...,255
	rol
	sta	(ptr),y
	dey
@2:	lda	(ptr),y		; do bytes 2,6,...,254
	rol
	sta	(ptr),y
	dey
@1:	lda	(ptr),y		; do bytes 1,5,...,253
	rol
	sta	(ptr),y
	dey
@0:	lda	(ptr),y		; do bytes 0,4,...,252
	rol
	sta	(ptr),y
	tya
	bne	:-
	txa
	bne	:--
	rts


; Description: Multiprecision transfer:  a = b
;
; Input:       Y/A (hi/lo) pointer to array (b)
;              ptr_mp set by macro to point to array (a)
;
; Output:      a = b
;
; Clobbered:   A, X, Y
;
; Globals:     arraylen (16-bit), zp ptr (16-bit), zp ptr_mp (16-bit)

copy_mp:
	sta	ptr		; store ptr lo from A
	sty	ptr+1		; store ptr hi from Y
	ldx	arraylen+1	; full pages
	ldy	arraylen	; partial pages
	beq	@0		;   if not process full pages only
	inx			; bump up x +1 for the partial page
	clc			; adjust to point below source start
	tya			; - ex: ptr = $xx00, arraylen = 1
	adc	ptr		; we want ptr = $(xx-1)01, y = $ff (255)
	sta	ptr
	bcs	:+
	dec	ptr+1
:	clc			; adjust to point below destination start
	tya			; - ex: ptr_mp = $xxff, arraylen = 2
	adc	ptr_mp		; we want ptr_mp = $xx01, y = $fe (254)
	sta	ptr_mp
	bcs	:+
	dec	ptr_mp+1
:	tya			; adjust y index value, y = -y
	eor	#$ff		; one's complement
	tay
	iny			; two's complement
	;and	#%10		; Duff's Device.   We know that arraylen is
	;beq	@2		;   even, so just need to check for 2nd bit
				;   (odd or even # of pairs).
				;   Fall through to @0.
				; NOTE: A = Y - 1, checking A, not Y
				; if A & %11 = 1, then Y%3 = 2
				; if A & %11 = 3, then Y%3 = 0
	jmp	@2		; arraylen hard coded, no need to guess
@0:	lda	(ptr),y		; move bytes 0,4,...,252
	sta	(ptr_mp),y
	iny
@1:	lda	(ptr),y		; move bytes 1,5,...,253
	sta	(ptr_mp),y
	iny
@2:	lda	(ptr),y		; move bytes 2,6,...,254
	sta	(ptr_mp),y
	iny
@3:	lda	(ptr),y		; move bytes 3,7,...,255
	sta	(ptr_mp),y
	iny			; page done ?
	bne	@0		; b: no
	inc	ptr+1		; next page
	inc	ptr_mp+1
	dex			; full page left ?
	bne	@0		; b:yes
	rts 


; Description: Skip leading zeros (used by div_mp and div16_mp)
;
; Input:       Y/A (hi/lo) pointer to array (b)
;
; Output:      None
;
; Clobbered:   A, X, Y
;
; Globals:     arraylen (16-bit), zp ptr (16-bit)

skipzeros:
	sta	ptr		; store ptr lo from A
	sty	ptr+1		; store ptr hi from Y
	ldx	arraylen+1	; full pages
	ldy	arraylen	; partial pages ?
	beq	@0		; b:no - process full pages only
	inx			; bump up x +1 for the partial page
	clc			; adjust to point below source start
	tya			; - ex: ptr = $xx00, arraylen = 1
	adc	ptr		; we want ptr = $(xx-1)01, y = $ff (255)
	sta	ptr
	bcs	:+
	dec	ptr+1
:	tya			; adjust y index value, y = -y
	eor	#$ff		; one's complement
	tay
	iny			; two's complement
	;and	#%10		; Duff's Device.   We know that arraylen is
	;beq	@2		;   even, so just need to check for 2nd bit
				;   (odd or even # of pairs).
				;   Fall through to @0.
				; NOTE: A = Y - 1, checking A, not Y
				; if A & %11 = 1, then Y%3 = 2
				; if A & %11 = 3, then Y%3 = 0
	jmp	@2		; arraylen hard coded, no need to guess
@0:	lda	(ptr),y		; check	0,4,...,252
	bne	@5
	iny
@1:	lda	(ptr),y		; check 1,5,...,253
	bne	@4
	iny
@2:	lda	(ptr),y		; check 2,6,...,254
	bne	@5
	iny
@3:	lda	(ptr),y		; check 3,7,...,255
	bne	@4
	iny
	bne	@0
	inc	ptr+1		; next page
	dex			; is there another page ?
	bne	@0		; b:yes
	sec			; flag: all cells are zero
	rts
@4:	dey			; make Y even
@5:	clc			; flag: (ptr),y -> first non-zero cell
	rts


; Description: 16-bit/8-bit divide based on:
;              http://6502org.wikidot.com/software-math-intdiv		
;
; Input:       divisor (8-bit), dividend (16-bit) hi: X, lo: dividend
;              
; Output:      divisor (8-bit) unchanged,
;              X: remainder (8-bit),
;              dividend (8-bit): quotient
;
; Clobbered:   A, X

.macro	div8			
	txa			; remainder
	asl	dividend
	.repeat 8		; unroll 8 times for speed
	rol
	bcs	:+
	cmp	divisor
	bcc	:++
:	sbc	divisor
	sec
:	rol	dividend
	.endrepeat
	tax			; remainder
.endmacro


; Description: Divide mp array by 8-bit number.
;
; Input:       Y(hi)/A(lo) pointer to mp
;              X = dividend/denominator
;
; Output:      (ptr) = (ptr)/x
;
; Clobbered:   A, X, Y
;
; Globals:     zp ptr (16-bit), arraylen (16-bit), dividend (8-bit),
;              zp xreg (8-bit), 
;
; Calls:       skipzeros, div8(macro)
;
; C Algorithm:
;
; short divbig(number, x)
; bignum number;
; unsigned short x;
; {
;     dword result;
;     short j = 0;
;     unsigned short rest = 0;
; 
;     while (number[j] == 0 && j < MAXSIZE)
;         j++;
;     if (j == MAXSIZE)
;         return (0);
;     while (j < MAXSIZE) {
;         result.w.lo = number[j];
;         result.w.hi = rest;
;         number[j] = result.L / x;
;         rest = result.L % x;
;         j++;
;     }
;     return (1);
; }

div_mp:
	stx	divisor		; save divisor
	jsr	skipzeros	; skip leading zeros for speed
	bcc	:+		; carry clear?  continue
	rts			;   else all zeros, return to caller
:	stx	xreg		; need x reg for speed (carry)
	ldx	#0
				; we know y is even and arraylen is also even
				; so unroll twice
@0:	lda	(ptr),y		; div byte 0,4,...,252
	sta	dividend
	div8
	lda	dividend
	sta	(ptr),y
	iny
@1:	lda	(ptr),y		; div byte 1,5,...,253
	sta	dividend
	div8
	lda	dividend
	sta	(ptr),y
	iny
	beq	:+
	jmp	@0		; unroll too big need to jmp it
:	dec	xreg		; full page left ?
	beq	:+
	inc	ptr+1		; next page
	jmp	@0		; unroll too big need to jmp it
:	clc			; non zero result
	rts


; Description: 32-bit/16-bit divide based on:
;              Apple II firmware (Steve Wozniak) and
;              http://www.txbobsc.com/aal/1983/aal8303.html#a5, and
;              optimized by Anton Treuenfels.
;
; Input:       divisor (16-bit big endian), dividend (32-bit big endian)
;              
; Output:      divisor (16-bit big endian) unchanged,
;              dividend (32-bit big endian) hi: remainder
;              dividend (32-bit big endian) lo: quotient
;
; Clobbered:   A, X, Y

.macro	div16
	ldx	dividend+1	; remainder
	ldy	dividend+0	; remainder
	.repeat 16		; unroll 16 times for speed
	asl	dividend+3	; 5
	rol	dividend+2	; 10
	txa			; 12
	rol			; 14
	tax			; 16
	tya			; 18
	rol			; 20
	tay			; 22
	cpx	divisor+1	; 25
	sbc	divisor+0	; 28
	bcc	:+		; 30/31 (no subtraction)
	tay			; 32
	txa			; 34
	sbc	divisor+1	; 37
	tax			; 39
	inc	dividend+3	; 44 (subtraction)
:
	.endrepeat
	stx	dividend+1	; remainder
	sty	dividend+0	; remainder
.endmacro


; Description: Divide mp array by 16-bit number.
;
; Input:       Y(hi)/A(lo) pointer to mp
;              divisor (16-bit) set by macro
;
; Output:      (ptr) = (ptr)/divisor
;
; Clobbered:   A, X, Y
;
; Globals:     zp ptr (16-bit), arraylen (16-bit), dividend (32-bit),
;              zp xreg (8-bit), divisor (16-bit)
;
; Calls:       skipzeros, div16(macro), div16_long(macro)
;
; C Algorithm: see div_mp

.macro	div16_long
	lda	(ptr),y		; load/store LSB of dividend
	sta	dividend+2
	sty	yreg		; save y for updating array
	iny
	lda	(ptr),y		; load/store LSB-1 of dividend
	sta	dividend+3
	div16			; need x and y reg
	ldy	yreg		; restore y
	lda	quotient+0	; load/store quotient MSB to array
	sta	(ptr),y
	iny
	lda	quotient+1	; load/store quotient LSB to array
	sta	(ptr),y
	iny
.endmacro

div16_mp:
	jsr	skipzeros	; skip leading zeros for speed
	bcc	:+		; carry clear?  continue
	rts			;   else all zeros, return to caller
:	lda	#0		; clear remainder/carry
	sta	dividend+0	; MSB (big endian)
	sta	dividend+1	; MSB-1
	stx	xreg		; macro div16 needs x reg
@0:	div16_long
	beq	:+		; done?
	jmp	@0		; unroll too big need to jmp it
:	dec	xreg
	beq	:+
	inc	ptr+1		; next page
	jmp	@0		; unroll too big need to jmp it
:	clc			; non zero result
	rts


; Description: print mp base 10/100
;
; Input:       Y(hi)/A(lo) pointer to array
;
; Output:      mp base 10/100 out to screen
;
; Clobbered:   A, X, Y, input array
;
; Globals:     zero page: ptr (16-bit) , ptr_mp (16-bit), carry_mp (8-bit),
;              arraylen (16-bit)
; Locals:

dlength:
	.byte	$0, $0
ptr_save:
	.byte	$0

print_mp:
	sta	ptr		; store ptr lo from A
	sty	ptr+1		; store ptr hi from Y
	tya
	clc
	adc	arrayend+1	; add number of pages since we have to go
	sta	ptr_save	;   backwards for multiply

	lda	declen+1	; converting to base 100, so dlength=dlength/2
	lsr
	sta	dlength+1
	lda	declen
	ror
	sta	dlength
				; print left of decimal
	ldy	#0		; get first digit in array (big endian)
	lda	(ptr),y
	jsr	bintobcd	; now convert from hex to dec	
	cmp	#10		; if less than 10 skip leading zero
	bcc	:+
	jsr	prbyte		; print it, only A gets hosed, safe to call
	jmp	period
:	clc			; no leading zero code
	adc	#$B0		;   convert single digit to ascii
	jsr	cout		;   print it
period:	lda	#'.'+$80	; print decimal point (fixed point math)
	jsr	cout		;   print it

print_mp1:			; loop through rest of decimal digits
	ldy	#0		; array[0] = 0
	lda	#0
	sta	(ptr),y
	sta	carry_mp	; clear multiprecision carry

	lda	ptr		; ptr_mp to get hosed, must be restored
	sta	ptr_mp
	lda	ptr_save	; add number of pages since we have to go
	sta	ptr_mp+1	;   backwards for multiply (right -> left)

	; multi array x 100
	; loop from LSB to MSB
	; 16-bit product = array[i] * 100 + carry_mp;
	; array[i] = product lo
	; carry_mp = product hi
	;
	; C Algorithm:
	;
	;    while (j >= 0) {
        ;	result.L = (long) number[j] * 100 + carry;
        ;	number[j] = result.w.lo;
        ;	carry = result.w.hi;
        ;	j--;
    	;    }

	.macro mult100		; 19.3
	.local	a1
	.local	a2
	.local	a3
				; 100 = 1100100, so a = ((x*2 + x)*8 + x) * 4

	lda	#0		;hi
	sta	mtmp
	lda	(ptr_mp),y	;lo
	sta	temp
	asl
	rol	mtmp
	adc	temp		;adc	(ptr_mp),y	;lo
	bcc	a1
	inc	mtmp
a1:	asl
	rol	mtmp
	asl
	rol	mtmp
	asl
	rol	mtmp
	adc	temp		;adc	(ptr_mp),y	;lo
	bcc	a2
	inc	mtmp
a2:	asl
	rol	mtmp
	asl
	rol	mtmp
	adc	carry_mp
	bcc	a3
	inc	mtmp
a3:	sta	(ptr_mp),y
	lda	mtmp
	sta	carry_mp
	.endmacro

	ldx	arrayend+1	; full pages
	ldy	arrayend	; partial
				; Duff's Device.   We know that arraylen =
				;   bin_len = 418, i.e. even, so start with m1
	jmp	m1
:	dex
	dec	ptr_mp+1	; previous page of 256
:	dey
m1:	mult100
	dey
m0:	mult100

	tya			; other options?
	bne	:-
	txa
	bne	:--

	; print array[0] (MSB)
	;ldy	#0		; get first digit in array
	lda	(ptr),y
	jsr	bintobcd
	jsr	prbyte		; print it, only A gets hosed, safe to call

				; dlength = dlength - 1
	lda	dlength		; check if dlength = 0
	bne	:+		;   if so, it will roll to $FF
	dec	dlength+1	;     so dec dlength+1
:	dec	dlength		; dec dlength
	bne	:+		; check dlength and dlength+1 for zeros
	lda	dlength+1	; 
	bne	:+
	rts			; dlength = 0, all done
:	jmp	print_mp1	; 


; Description: set array initial value
;
; Input:       Y(hi)/A(lo) pointer to mp
;              X = value left of decimal
;
; Output:      (ptr),0 = X
;              (ptr),1 - (ptr),length-1 = 0
;
; Clobbered:   A, X, Y
;
; Globals:     arraylen (16-bit), zp length (16-bit), zp ptr (16-bit)

set_mp:
	sta	ptr		; store ptr lo from A
	sty	ptr+1		; store ptr hi from Y
	ldy	#0		; left of decimal
	txa
	sta	(ptr),y		; store initial value
	inc	ptr		; bump up pointer
	bne	:+		;  to next digit
	inc	ptr+1		;  rest of array will be 0
: 	ldx	arrayend+1	; full pages (hack, arrayend = arraylen - 1)
	ldy	arrayend	; partial pages
	beq	@0		;   if not process full pages only
	inx			; bump up x +1 for the partial page
	clc			; adjust to point below source start
	tya			; - ex: ptr = $xx00, arraylen = 1
	adc	ptr		; we want ptr = $(xx-1)01, y = $ff (255)
	sta	ptr
	bcs	:+
	dec	ptr+1
:	tya			; adjust y index value, y = -y
	eor	#$ff		; one's complement
	tay
	iny			; two's complement
	lda	#0		; A must be set to #0 to clear array 
@0:	sta	(ptr),y		; move even byte (0, 2, ..., 254)
	iny
	bne	@0		; b: no
	inc	ptr+1		; next page
	dex			; full page left ?
	bne	@0
	rts 


; Description: Print a string.
;
; Input:       Y(hi)/A(lo) pointer to string
;
; Output:      String to screen
;
; Clobbered:   A, Y, X, ptr
;
; Globals:     zp ptr (16-bit)

print:	stay	ptr		; load Y/A (hi/lo) in ptr
	ldy	#0		; start with first char
	lda	(ptr),y		; load initial char
:	ora	#$80		; we do not want flashing or inverse
				;  (its an Apple II thing)
	jsr     cout		; call apple II char out
	iny			; y++
	lda	(ptr),y		; get next char
	bne	:-		; not NULL? then print it
	rts			; all done, move alone


crout:	lda	#$8D
	jmp	cout


; Description: BIN/HEX to BCD
;
; Input:       A (BIN)
;
; Output:      A (BCD)
;
; Clobbered:   A, X

bintobcd:
	pha
	sec
	ldx	#$FF
:	inx
	sbc	#10
	bpl	:-
	pla
	bcc	:++
:	adc	#6
:	dex
	bpl	:--
	rts


; data

mp_a_array:
	;.org	*+bin_len
        .res bin_len
mp_b_array:
	;.org	*+bin_len
        .res bin_len
mp_x_array:
	;.org	*+bin_len
        .res bin_len
mp_y_array:
	;.org	*+bin_len
        .res bin_len

end:
