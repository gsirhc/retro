
shell_rx_main:
  jsr print_load_status_via
  lda #">"
  jsr print_char_acia
  lda #" "
  jsr print_char_acia
  ldy #$00                         ; Y is hte hi byte counter
  sty LOCAL_MEM_BYTE1              ; wait counter (2 bytes)
  sty LOCAL_MEM_BYTE2
  sty PROGRAM_SIZE                 ; program size counter (2 bytes)   
  sty PROGRAM_SIZE + 1
  lda #<PROGRAM_START_ADDR         ; lo byte first
  sta PROGRAM_PRT
  lda #>PROGRAM_START_ADDR         ; hi byte next
  sta PROGRAM_PRT + 1
shell_rx_loop:
  jsr check_acia_rx                ; get next byte
  cpx #$01
  bne no_byte_rx                   ; no byte rx
  sta (PROGRAM_PRT), Y             ; store byte at program point (2 bytes)
  stx LOCAL_MEM_BYTE1              ; reset the wait counter, X = 1 from check_acia_rx
  iny                              ; inc Y, the high byte
  bne not_end_y                    ; Y > 0 and has not rolled over to 0 yet
  inc PROGRAM_PRT + 1              ; Yes, Y rolled over, inc lo byte
not_end_y:
  jmp continue_reading             ; keep reading
no_byte_rx:
  lda LOCAL_MEM_BYTE1              ; load wait counter into X
  cmp #$00
  beq shell_rx_loop                ; if counter is 0, we wa haven't received anything, loop forever
  inc LOCAL_MEM_BYTE1
  bne cnt_no_rolloever
  inc LOCAL_MEM_BYTE2
  beq end_rx
cnt_no_rolloever;
  jmp shell_rx_loop                
continue_reading:
  inc PROGRAM_SIZE
  bne done_counting                         ; inc program byte count              
  inc PROGRAM_SIZE + 1             ; if carry set, increment second byte to count above 255
done_counting:
  jmp shell_rx_loop
end_rx:
  jsr print_program_size_acia      ; print the read program bytes to user
  lda PROGRAM_SIZE + 1
  jsr print_a_hex_acia
  lda PROGRAM_SIZE
  jsr print_a_hex_acia
  rts