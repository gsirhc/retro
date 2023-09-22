; recives a file over XMODEM protocol, and loads it into memory
USER_PROGRAM_START = $0400   ; Address for start of user programs
USER_PROGRAM_WRITE_PTR = $08 ; ZP address for writing user program

shell_rx_main:
  ; Set pointers
  lda #<USER_PROGRAM_START   ; Low byte first
  sta USER_PROGRAM_WRITE_PTR
  lda #>USER_PROGRAM_START   ; High byte next
  sta USER_PROGRAM_WRITE_PTR + 1
  ; Delay so that we can set up file send
  ;lda #20                     ; wait ~1 second
  ;jsr shell_rx_sleep_seconds
  ; NAK, ACK once
shell_block_nak:
  lda #$15                   ; NAK gets started
  jsr print_char_lcd
  ;lda SPEAKER                ; Click each time we send a NAK or ACK
  jsr shell_rx_receive_with_timeout  ; Check in loop w/ timeout
  jsr print_a_hex_lcd
  bcc shell_block_nak       ; Not received yet
  cmp #$01                   ; If we do have char, should be SOH
  bne shell_rx_fail         ; Terminate transfer if we don't get SOH
shell_rx_block:
  ; Receive one block
  jsr check_acia_no_irq         ; Block number
  jsr check_acia_no_irq         ; Inverse block number
  ldy #0                     ; Start at char 0
shell_rx_char:
  jsr check_acia_no_irq
  sta (USER_PROGRAM_WRITE_PTR), Y
  iny
  cpy #128
  bne shell_rx_char
  jsr check_acia_no_irq         ; Checksum - TODO verify this and jump to shell_block_nak to repeat if not mathing
  lda #$06                   ; ACK the packet
  jsr print_char_lcd
  ;lda SPEAKER                ; Click each time we send a NAK or ACK
  jsr check_acia_no_irq
  cmp #$04                   ; EOT char, no more blocks
  beq shell_rx_done
  cmp #$01                   ; SOH char, next block on the way
  bne shell_block_nak       ; Anything else fail transfer
  lda USER_PROGRAM_WRITE_PTR ; This next part moves write pointer along by 128 bytes
  cmp #$00
  beq block_half_advance
  lda #$00                   ; If low byte != 0, set to 0 and inc high byte
  sta USER_PROGRAM_WRITE_PTR
  inc USER_PROGRAM_WRITE_PTR + 1
  jmp shell_rx_block
block_half_advance:         ; If low byte = 0, set it to 128
  lda #$80
  sta USER_PROGRAM_WRITE_PTR
  jmp shell_rx_block
shell_rx_done:
  lda #$6                    ; ACK the EOT as well.
  jsr print_char_acia
  ;lda SPEAKER                ; Click each time we send a NAK or ACK
  lda #1                     ; wait a moment (printing does not work otherwise..)
  ;jsr shell_rx_sleep_seconds
  ;jsr shell_rx_print_user_program
  ;jsr print_new_line
  lda #0
  jmp return_os
shell_rx_fail:
  lda #1
  jmp return_os

shell_rx_receive_with_timeout:
  ldy #$ff
y_loop:
  ldx #$ff
x_loop:
  lda ACIA_STATUS              ; check ACIA status in inner loop
  and #$08                     ; mask rx buffer status flag
  bne rx_got_char
  dex
  cpx #0
  bne x_loop
  dey
  cpy #0
  bne y_loop
  clc                          ; no byte received in time
  rts
rx_got_char:
  lda ACIA_DATA                  ; get byte from ACIA data port
  sec                          ; set carry bit
  rts

shell_rx_sleep_seconds: ; sleep for 0-63 seconds (approx)
  pha                   ; save registers
  txa
  pha
  tya
  pha
  asl                   ; multiply A by 4, outer loop is approx 250ms.
  asl
a_loop1:
  cmp #0                ; stop if A is 0 (outer loop)
  beq end
  ldy #$ff              ; start Y at 255 and decrement (middle loop)
y_loop1:
  ldx #$ff
x_loop1:                ; start Y at 255 and decrement (inner loop)
  dex
  cpx #0
  bne x_loop1           ; end inner loop
  dey
  cpy #0
  bne y_loop1           ; end middle loop
  sta VARIABLE_MEM      ; decrement A and repeat
  inc VARIABLE_MEM
  lda VARIABLE_MEM
  jmp a_loop1           ; end outer loop
end:
  pla                   ; restore registers
  tay
  pla
  tax
  pla
  rts