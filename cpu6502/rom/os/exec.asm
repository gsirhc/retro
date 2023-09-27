TEMP_STOR = $50                 ; dont have 4 registers, so store in available zero page

exec_address:
  jsr print_exec_status_via
  lda #$00
  sta LOCAL_MEM_BYTE1
  sta LOCAL_MEM_BYTE2
  ldx PROMPT_CHAR_CNT
  ldy #$00                      ; Read counter to know which byte to add the digit
get_next_addr_digit:
  dex
  lda PROMPT_START, x 
  cmp #"0"
  bcc nan
  cmp #"9"
  bcc convert_number
  beq convert_number               
nan:
  cmp #"A"
  bcc notHex
  cmp #"F"
  bcc convert_hex_jetter
  beq convert_hex_jetter
convert_number:
  sbc #"0"                      ; Subtract ascii "0" to get number
  jmp store_hex
convert_hex_jetter
  sbc #"A"                      ; Subtract ascii "A" to get 0-5 
  adc #$0A                      ; Add Hex A to get 10 - 16
store_hex:
  iny                           ; Inc digit counter (1-based, no better place to put INY)
  cmp #$01
  bne try_hi_second:
  sta LOCAL_MEM_BYTE2           ; Place the last (4th digit) in hi byte (most significant)
  jmp get_next_addr_digit
try_hi_second:
  cmp #$02
  bne try_lo_first
  jsr multiply_a_by_16          ; Mulitple by 16 (HEX 10) to fill hi byte (most significat)
  adc LOCAL_MEM_BYTE2           ; Add current byte 2 to A
  sta LOCAL_MEM_BYTE2
  jmp get_next_addr_digit
try_lo_first:
  cmp #$03
  bne must_be_high_fist
  sta LOCAL_MEM_BYTE1           ; Place 2nd digit (from left) in lo byte (least significant)
  jmp get_next_addr_digit
must_be_high_fist:
  jsr multiply_a_by_16          ; Mulitple by 16 (HEX 10) to fill hi byte (most significat)
  adc LOCAL_MEM_BYTE1           ; Add current byte 1 to A
  sta LOCAL_MEM_BYTE1
  ; Print the address to LCD for debugging and feedback to user
  lda LOCAL_MEM_BYTE1
  jsr print_a_hex_lcd
  lda LOCAL_MEM_BYTE2
  jsr print_a_hex_lcd
  ; JMP to address
  jmp LOCAL_MEM_BYTE1           ; The OS dies here, the program must exit property through BIOS (or press Reset)
                                ; TODO add a Ctrl-C IRG routine for programs that don't exit
notHex:
  jsr print_invalid_addr_acia
  rts

multiply_a_by_16:
  sty TEMP_STOR                 ; save off Y      
  ldy #$10                      ; 16 loops
multiply_a_by_16_loop:
  clc
  adc #$10                      ; add 16
  dey
  bne multiply_a_by_16_loop
  ldy TEMP_STOR                 ; restore Y
  rts
