LD_PROGRAM_PRT = $4E               ; current pointer to read next byte, 2 bytes
HAS_RECV       = $4D               ; boolean true when the first byte is received, 1 byte
WAIT_CNT       = $4B               ; wait loop counter, 2 bytes

LOAD:
shell_rx_main:
  lda #'>'
  jsr CHROUT
  lda #' '
  jsr CHROUT
  ldy #0                           ; Y is the hi byte counter
  sty HAS_RECV                     ; init has received to false
  sty WAIT_CNT                     ; wait counter (2 bytes)
  sty WAIT_CNT + 1
  ; lda #<PROGRAM_START_ADDR         ; lo byte first
  lda #$00
  sta LD_PROGRAM_PRT
  ; lda #>PROGRAM_START_ADDR         ; hi byte next
  lda #$03
  sta LD_PROGRAM_PRT + 1
shell_rx_loop:
  jsr READCHAR                     ; get next byte
  bcc no_byte_rx                   ; no byte rx
  stx HAS_RECV                     ; set the has received flag, X = 1
  sta (LD_PROGRAM_PRT), Y          ; store byte at program pointer (2 bytes)
  stx WAIT_CNT                     ; reset the wait counter, X = 1 from check_acia_rx
  iny                              ; inc Y, the high byte
  bne not_end_y                    ; Y > 0 and has not rolled over to 0 yet
  inc LD_PROGRAM_PRT + 1           ; Y rolled over, inc hi byte
not_end_y:
  jmp continue_reading             ; keep reading
no_byte_rx:
  lda HAS_RECV                     ; check if any bytes received yet 
  cmp #0
  beq shell_rx_loop                ; if HAS_RECV is 0, we wa haven't received anything, loop forever
  inc WAIT_CNT                     ; increment wait count, once we reach FFFF, we exit the loader
  bne shell_rx_loop
  inc WAIT_CNT + 1
  lda WAIT_CNT + 1
  cmp #$1F                         ; Max wait time is determinted by the hi byte (255-64K cycles)
  beq end_rx               
continue_reading:
  jmp shell_rx_loop
end_rx:
  rts