
shell_rx_main:
  ldy #$00
  sty LOCAL_MEM_BYTE1              ; init the wait counter
  sty PROGRAM_SIZE
  sty PROGRAM_SIZE + 1
  lda #<PROGRAM_START_ADDR         ; low byte first
  sta PROGRAM_PRT
  lda #>PROGRAM_START_ADDR         ; high byte next
  sta PROGRAM_PRT + 1
shell_rx_loop:
  jsr check_acia_rx                ; get next byte
  cpx #$01
  bne no_char_rx                   ; no byte rx
  stx LOCAL_MEM_BYTE1              ; reset the wait counter, X = 1 from check_acia_rx
  sta (PROGRAM_PRT), Y             ; store byte at program point (2 bytes)
  iny                              ; inc Y, the high byte
  bne not_end_y                    ; Y > 0 and has not rolled over to 0 yet
  inc PROGRAM_PRT + 1              ; Yes, Y rolled over, inc lo byte
not_end_y:
  jmp continue_reading             ; keep reading
no_char_rx:
  lda LOCAL_MEM_BYTE1              ; load wait counter into A
  cmp #$00
  beq shell_rx_loop                ; if counter is 0, we wa haven't received anything, loop forever
  adc #$01                         ; inc the wait counter
  bcs end_rx                       ; carry set, the counter has hit max (255), exit
  sta LOCAL_MEM_BYTE1              ; store counter for next loop
  jmp shell_rx_main                ; still waiting, keep reading
continue_reading:
  lda PROGRAM_SIZE
  adc #$01                         ; inc program byte count
  bcc shell_rx_loop                
  inc PROGRAM_SIZE + 1             ; if carry set, increment second byte to count above 255
  jmp shell_rx_loop
end_rx:
  jsr print_program_size_acia      ; print the read program bytes to user
  lda PROGRAM_SIZE
  jsr print_a_hex_acia
  lda PROGRAM_SIZE + 1
  jsr print_a_hex_acia
  jmp return_os                    ; exit to OS
  rts