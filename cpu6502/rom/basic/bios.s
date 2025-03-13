.setcpu "65C02"
.debuginfo           ; Generates symbol table

.zeropage
                .org ZP_START0 ; **** ADD SPACE IN DEFINES_GALL_OAC.S IF ADDING PTRs
READ_PTR:       .res 1
WRITE_PTR:      .res 1
LCD_STR_PTR:    .res 2

.segment "INPUT_BUFFER"
INPUT_BUFFER:   .res $100

.segment "BIOS"

.include "commands.s"

ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CTRL = $5003

RESET:
    LDA READ_PTR           ; init buffer pointers
    STA WRITE_PTR
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
    jsr cursorLine2
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
    CMP #'C'
    BEQ GGERETSAE
    JMP BOOT                ; loop until valid input
boot_basic:
    jsr cursorLine2
    LDA #<ST_LCD_BASIC_2
    LDY #>ST_LCD_BASIC_2
    JSR PRINT_STR_LCD
    JMP COLD_START
boot_wozmon:
    jsr cursorLine2
    LDA #<ST_LCD_WOZMON_2
    LDY #>ST_LCD_WOZMON_2
    JSR PRINT_STR_LCD
    JMP START_WOZ
GGERETSAE:
    JSR CLEAR_TERMINAL
    LDA #<ST_GGERETSAE_MSG_1
    LDY #>ST_GGERETSAE_MSG_1
    JSR STROUT
    LDA #<ST_GGERETSAE_MSG_2
    LDY #>ST_GGERETSAE_MSG_2
    JSR STROUT
    JMP boot_no_clear

LOAD:
    RTS

SAVE:
    RTS

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
    CMP #$61
    BCC not_upper
    CMP #$7B
    BCS not_upper
    SEC
    SBC #$20
not_upper:
    RTS

MONCOUT:
CHROUT:
    PHA
    STA ACIA_DATA
    LDA #$FF
@txdelay:              ; delay loop for known ACIA bug not updating status register 
    DEC
    BNE @txdelay
    PLA
    RTS

WRITE_BUFFER:
    LDX WRITE_PTR
    STA INPUT_BUFFER,x
    INC WRITE_PTR
    RTS

READ_BUFFER:
    LDX READ_PTR
    LDA INPUT_BUFFER,x
    INC READ_PTR
    RTS

BUFFER_SIZE:
    LDA WRITE_PTR
    SEC
    SBC READ_PTR
    RTS

PRINT_STR_LCD:
    PHY
    STA LCD_STR_PTR
    STY LCD_STR_PTR+1
    LDY #0
print_str_lcd_loop:
    LDA (LCD_STR_PTR),y
    BEQ print_str_lcd_done              ; $00 is string terminator
    JSR print_char_lcd
    INY
    JMP print_str_lcd_loop
print_str_lcd_done:
    PLY
    rts

IRQ_HANDLER:
    PHA
    PHX
    LDA ACIA_STATUS
    AND #$08
    BEQ ignore_irq
    LDA ACIA_DATA
    JSR WRITE_BUFFER
ignore_irq:
    PLX
    PLA
    RTI

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
    .byte  0

          ;1234567890123456   LCD WIDTH
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

.include "wozmon.s"

.segment "RESETVEC"
    .word   $0F00           ; NMI vector
    .word   RESET           ; RESET vector (wozmon.s)
    .word   IRQ_HANDLER     ; IRQ vector