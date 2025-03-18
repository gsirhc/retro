.setcpu "65C02"
.debuginfo           ; Generates symbol table

                .zeropage
                .org ZP_START0 ; **** ADD SPACE IN DEFINES_GALL_OAC.S IF ADDING PTRs
JIFFIES:        .res 1
UP_SECONDS:     .res 1
UP_MINUTES:     .res 1
UP_HOURS:       .res 1
READ_PTR:       .res 1
WRITE_PTR:      .res 1
ADDR_PTR:       .res 2
DEC_VALUE:      .res 2
MOD10_VALUE:    .res 2
DECIMAL_STR:    .res 6

.segment "SERIAL_BUFFER"
SERIAL_BUFFER:  .res $100

.segment "BIOS"

.include "commands.s"
.include "format.s"

ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CTRL = $5003

RESET:
    LDA READ_PTR           ; init buffer pointers
    STA WRITE_PTR
    LDA #0
    STA JIFFIES
    STA UP_MINUTES
    STA UP_SECONDS
    STA UP_HOURS
    CLI
    LDA #$1F               ; ACIA: 8-N-1, 19200 baud.
    STA ACIA_CTRL
    LDA #$89               ; ACIA: No parity, no echo, interrupts.
    STA ACIA_CMD
    LDA #$1B               ; ACIA: Begin with escape.
    ;JMP COLD_START         ; start BASIC
    JSR reset_via_irq

BOOT:
    JSR CLEAR_TERMINAL
    JSR clear_lcd
boot_no_clear:
    LDA #<ST_BOOT_MENU
    LDY #>ST_BOOT_MENU
    JSR STROUT
    LDA #<ST_LCD_IDENT_1
    LDY #>ST_LCD_IDENT_1
    JSR PRINT_STR_LCD
    jsr cursorLine2_lcd
    LDA #<ST_LCD_BOOT_2
    LDY #>ST_LCD_BOOT_2
    JSR PRINT_STR_LCD
boot_loop:
    JSR CHRIN
    BCC boot_loop
    CMP #'1'
    BEQ boot_basic
    CMP #'2'
    BEQ boot_wozmon
    CMP #'3'
    BEQ boot_clock
    CMP #'C'
    BEQ ggeretsae
    JMP BOOT                ; loop until valid input
boot_basic:
    JSR clear_lcd
    JMP COLD_START
boot_wozmon:
    jsr cursorLine2_lcd
    LDA #<ST_LCD_WOZMON_2
    LDY #>ST_LCD_WOZMON_2
    JSR PRINT_STR_LCD
    JMP START_WOZ
boot_clock:
    jsr CLEAR_TERMINAL
    lda #<ST_CLOCK_RUNNING
    ldy #>ST_CLOCK_RUNNING
    jsr STROUT
    jsr cursorLine2_lcd
    lda #<ST_LCD_LINE_CLR
    ldy #>ST_LCD_LINE_CLR
    JSR PRINT_STR_LCD
boot_clock_loop:
    jsr cursorLine2_lcd
    lda #0
    sta DEC_VALUE + 1               ; default hi byte to 0 for all times
    lda UP_HOURS
    sta DEC_VALUE
    jsr FM2BYTEDECZP
    jsr PRINT_NBR_LCD
    lda #':'
    jsr print_char_lcd
    lda UP_MINUTES
    sta DEC_VALUE
    jsr FM2BYTEDECZP
    jsr PRINT_NBR_LCD
    lda #':'
    jsr print_char_lcd
    lda UP_SECONDS
    sta DEC_VALUE
    jsr FM2BYTEDECZP
    jsr PRINT_NBR_LCD
    jsr CHRIN
    bcc boot_clock_loop
    jsr CHLL
    jsr CHLL
    jsr CHLL
    jmp BOOT
ggeretsae:
    jsr CLEAR_TERMINAL
    lda #<ST_GGERETSAE_MSG_1
    ldy #>ST_GGERETSAE_MSG_1
    jsr STROUT
    lda #<ST_GGERETSAE_MSG_2
    ldy #>ST_GGERETSAE_MSG_2
    jsr STROUT
    jmp boot_no_clear

LOAD:
    rts

SAVE:
    rts

MONRDKEY:
CHRIN:
    PHX
    JSR BUFFER_SIZE
    BEQ buffer_empty
    JSR READ_BUFFER
    CMP #$08           ; Backspace key (ignore)
    BEQ backspace
    JSR FORCE_UPPER    ; REQUIRE upper-case for basic (and everything else)
    JSR CHROUT
    SEC
    PLX
    RTS
backspace:
    LDA #$08           ; Backspace for the terminal
    JSR CHROUT
    LDA #$5F           ; send underscore to basic for its backspace (don't echo)
    SEC
    PLX
    RTS
buffer_empty:
    CLC
    PLX
    RTS

FORCE_UPPER:
    cmp #$61
    bcc @skip
    cmp #$7B
    bcs @skip
    sec
    sbc #$20
@skip:
    rts

MONCOUT:
CHROUT:
    pha
    sta ACIA_DATA
    jsr CHLL            ; delay loop for known ACIA bug not updating status register
    pla
    rts

WRITE_BUFFER:
    LDX WRITE_PTR
    STA SERIAL_BUFFER,x
    INC WRITE_PTR
    RTS

READ_BUFFER:
    LDX READ_PTR
    LDA SERIAL_BUFFER,x
    INC READ_PTR
    RTS

BUFFER_SIZE:
    LDA WRITE_PTR
    SEC
    SBC READ_PTR
    RTS

PRINT_STR_LCD:
    PHY
    STA ADDR_PTR
    STY ADDR_PTR+1
    ldy #0
@loop:
    LDA (ADDR_PTR),y
    BEQ @done                 ; $00 is string terminator
    JSR print_char_lcd
    INY
    JMP @loop
@done:
    PLY
    rts

PRINT_NBR_LCD:
    phy
    ldy #0
@loop:
    LDA DECIMAL_STR,y
    BEQ @done                 ; $00 is string terminator
    JSR print_char_lcd
    INY
    JMP @loop
@done:
    PLY
    rts

CHLL:
    lda #$FF
@loop:               
    dec
    bne @loop
    rts

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; VT100 commands, see http://www.braun-home.net/michael/info/misc/VT100_commands.htm
;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

CLEAR_TERMINAL:
  PHA
  JSR PRINT_LEADER
  LDA #'2'
  JSR CHROUT
  LDA #'J'              
  JSR CHROUT
  JSR PRINT_LEADER
  LDA #'f'              ; reset cursor to "home" (top-left of screen)
  JSR CHROUT
  PLA
  RTS

PRINT_LEADER:
  LDA #$1B              ; ESC
  JSR CHROUT
  LDA #'['
  JSR CHROUT
  RTS

ST_BOOT_MENU:
    .byte "BOOT TO:"
    .byte  CR,LF
    .byte  "1. BASIC",CR,LF
    .byte  "2. WOZMON",CR,LF
    .byte  "3. CLOCK",CR,LF
    .byte  0
ST_CLOCK_RUNNING:
    .byte "RUNNING CLOCK.  ANY KEY TO EXIT TO BOOT MENU",0

          ;1234567890123456   LCD WIDTH
ST_LCD_LINE_CLR:
    .byte "                ",0
ST_LCD_IDENT_1:
    .byte "OAC 6502 CPU CG ",0
ST_LCD_BOOT_2:
    .byte "SELECT BOOT     ",0
ST_LCD_BASIC_2:
    .byte "     BASIC      ",0
ST_LCD_WOZMON_2:
    .byte "    WOZMON      ",0
ST_GGERETSAE_MSG_1:
          ;123456789012345678901234567890123456789012345678901234  64 bytes
          ;123456789012345678901234567890123456789012345678901234  128 bytes
          ;123456789012345678901234567890123456789012345678901234  192 bytes
          ;123456789012345678901234567890123456789012345678901234  256 bytes (absolute max)
    .byte "HELLO THERE, THIS IS AN OAC 6502 HOME COMPUTER BUILT BY:", CR, LF, CR, LF
    .byte "                    CHRIS GALL",CR, LF
    .byte "                2023-NEVER FINISHED", CR, LF
    .byte "  1-MHZ W65C02 CPU, 16K RAM, 32K ROM, RS232 TERMINAL", CR, LF, CR, LF
    .byte 0
ST_GGERETSAE_MSG_2: 
    .byte "...PEACE, LOVE, A GOOD BEER AND AN OLD COMPUTER = HAPPY", CR, LF
    .byte "4F 41 43 3D 4F 4C 44 20 41 53 53 20 43 4F 4D 50 55 54 45 52", CR, LF, CR, LF
    .byte  0

;.incbin "../programs/oregon_trail.bas"

IRQ_HANDLER:
    PHA     
    PHX                        
    LDA ACIA_STATUS                 
    AND #$08                        
    BEQ @via
    LDA ACIA_DATA
    JSR WRITE_BUFFER
    JMP @done
@via:                            
    bit T1CL                        ; read timer lo byte to clear timer interrupt
    inc JIFFIES                     
    lda JIFFIES
    cmp #$3C                        ; 59 (zero-based) = 60hz
    bne @done
    ldx #0                          ; reset value for all time counters
    stx JIFFIES
    inc UP_SECONDS
    lda UP_SECONDS
    cmp #$3C
    bne @done
    stx UP_SECONDS
    inc UP_MINUTES
    lda UP_MINUTES
    cmp #$3C
    bne @done
    stx UP_MINUTES
    inc UP_HOURS
    lda UP_HOURS
    cmp #$18
    bne @done
    stx UP_HOURS                    ; rollover the entire clock (who runs for 24 hours??) 
    stx UP_MINUTES
    stx UP_SECONDS
@done:
    PLX
    PLA
    RTI

.include "wozmon.s"

.segment "RESETVEC"
    .word   $0F00           ; NMI vector
    .word   RESET           ; RESET vector (wozmon.s)
    .word   IRQ_HANDLER     ; IRQ vector