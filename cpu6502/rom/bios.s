.setcpu "65C02"
.debuginfo           ; Generates symbol table

CR = 13
LF = 10

; No .org here -- link.cfg's ZP memory area already pins this segment to
; $0000; an explicit .org $00 turns out to force ca65 into "absolute"
; placement that ld65 then just keeps incrementing through every segment
; declared afterward (SERIAL_BUFFER, BIOS, ...), ignoring their own MEMORY
; start addresses entirely -- verified empirically (a minimal repro placed
; BIOS's RESET at $0102 instead of $8000 with .org present, $8000 without).
                .zeropage
READ_PTR:       .res 1
WRITE_PTR:      .res 1
ADDR_PTR:       .res 2

.segment "SERIAL_BUFFER"
SERIAL_BUFFER:  .res $100

.segment "BIOS"

; Fixed, memorable jump table -- deliberately the very first bytes of the
; BIOS segment (ld65's gall_oac.cfg starts BASROM at $8000, and the old
; BASIC/menu segments it once shared this region with are gone -- see the
; pivot's "Dropped" list -- so BIOS is now the first thing in ROM), so a
; human can type these from memory (from Wozmon, or JSR'd from their own
; assembled program) without needing to know or look up a build-specific
; address every time editor.s's/via.s's own code shifts around. One-
; instruction jump-table entries, same shape as a fixed low-memory cold-
; start vector on period hardware (e.g. the C64's $A000 BASIC cold-start
; entry) -- every real implementation keeps moving freely as its own
; source changes; only these jumps need to stay put. See
; CGOAC6502_REVIEW.md for the full writeup and the Help panel's "OS calls"
; section for user-facing documentation of these as callable routines.
SHELL_ENTRY:
    jmp SHELL_START       ; $8000 -- <addr>R from Wozmon starts the shell
PRINT_CHAR:
    jmp CHROUT            ; $8003 -- A = char -> terminal (ACIA)
PRINT_STR:
    jmp STROUT            ; $8006 -- A/Y = lo/hi of a NUL-terminated string -> terminal
LCD_PUTC:
    jmp print_char_lcd    ; $8009 -- A = char -> LCD at the current cursor position
LCD_PUTS:
    jmp PRINT_STR_LCD      ; $800C -- A/Y = lo/hi of a NUL-terminated string -> LCD
LCD_CLEAR:
    jmp clear_lcd          ; $800F -- clear the LCD and home its cursor
LCD_LINE1:
    jmp cursorLine1_lcd   ; $8012 -- move the LCD cursor to row 1
LCD_LINE2:
    jmp cursorLine2_lcd   ; $8015 -- move the LCD cursor to row 2

.include "vterm.s"
.include "via.s"
.include "editor.s"
.include "load.s"      ; after editor.s -- uses its SRC_START/SRC_END/SRCPTR

ACIA_DATA = $5000
ACIA_STATUS = $5001
ACIA_CMD = $5002
ACIA_CTRL = $5003

RESET:
    jsr CHLL               ; delay for resets?? (doesn't always start properly)
    jsr CHLL
    LDA READ_PTR           ; init buffer pointers
    STA WRITE_PTR
    CLI
    LDA #$1F               ; ACIA: 8-N-1, 19200 baud.
    STA ACIA_CTRL
    LDA #$89               ; ACIA: No parity, no echo, interrupts.
    STA ACIA_CMD
    jsr reset_via           ; also clears the LCD
    JSR CLEAR_TERMINAL
    JMP START_WOZ

MONRDKEY:
CHRIN:                 ; for BASIC-era callers; kept as a plain alias of READCHAR
    jsr READCHAR
    bcc @nochar
    cmp #$08           ; Backspace key (ignore)
    beq @backspace
    sec
    jmp @done
@backspace:
    lda #$5F           ; send underscore for its backspace (don't echo)
    sec
    jmp @done
@nochar:
    clc
@done:
    rts

READCHAR:               ; For all non-monitor callers
    phx
    jsr BUFFER_SIZE
    beq @buffer_empty
    cmp #$B0
    bcs @buffer_mostly_full
    pha
    lda #$09           ; clear RTC/CTS to allow more chars to be sent
    sta ACIA_CMD
    pla
@buffer_mostly_full:
    jsr READ_BUFFER
    jsr CHROUT
    jsr FORCE_UPPER    ; REQUIRE upper-case (matches Wozmon's own convention)
    sec
    jmp @done
@buffer_empty:
    clc
@done:
    plx
    rts

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

; Print a NUL-terminated string to the terminal. A/Y = lo/hi of the string
; pointer, same calling convention as PRINT_STR_LCD (via.s) but out CHROUT.
STROUT:
    sta ADDR_PTR
    sty ADDR_PTR+1
    ldy #0
@loop:
    lda (ADDR_PTR),y
    beq @done
    jsr CHROUT
    iny
    bne @loop
@done:
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

CHLL:
    lda #$FF
@loop:
    dec
    bne @loop
    rts

NMI_HANDLER:
    pha
    phx
    lda ACIA_STATUS
    and #$08
    beq @done
    lda ACIA_DATA
    cmp #$03                        ; Ctrl-C (ASCII ETX) -- real serial
    beq @breakToShell                ; break, not buffered as input (below)
    jsr WRITE_BUFFER
    jsr BUFFER_SIZE
    cmp #$F0                        ; head room for full buffer
    bcc @done
    lda #$01                        ; set RS232 RTS to stop sending
    sta ACIA_CMD
@done:
    plx
    pla
    rti
@breakToShell:
    ; NMI is genuinely non-maskable -- it fires the instant this byte
    ; arrives no matter what the CPU is doing, including a user program
    ; spinning forever with no READCHAR poll of its own to ever notice a
    ; buffered byte. That's what makes Ctrl-C usable as a real break key
    ; here (unlike, say, a BASIC STOP key, which only works because the
    ; interpreter itself polls between statements) -- the standard 6502
    ; SBC monitor pattern for a serial break: the receive-interrupt
    ; handler recognizes the break character and, instead of RTI-ing back
    ; to whatever it interrupted, discards that context and jumps straight
    ; to the monitor. We're never returning to the interrupted code, so
    ; there's nothing to restore -- just reset the stack (abandoning this
    ; handler's own PHA/PHX along with everything the interrupted code had
    ; pushed) to a known-good empty state and go straight to the shell's
    ; prompt. Deliberately doesn't CHROUT anything here first: the
    ; interrupted code could itself be mid-CHROUT (mid-CHLL, waiting out
    ; the real per-character ACIA delay), and writing a fresh byte to
    ; ACIA_DATA before that finishes would corrupt whatever transmission
    ; was already in flight -- the shell's own "*" reprompt is feedback
    ; enough that the break landed. Only meaningful while the ACIA is
    ; jumpered to NMI (J7's default, matching the shipped ROM) -- routing
    ; it to IRQ instead already disables all serial reception today
    ; (IRQ_HANDLER is a bare stub), a pre-existing limit, not new here.
    ldx #$FF
    txs
    jmp SHELL_PROMPT

IRQ_HANDLER:                        ; no default source enables an IRQ (see
    pha                              ; reset_via's IER write) -- this stub
    pla                              ; exists so a user program that enables
    rti                              ; one (VIA CA1/Timer1/etc via J7) has a
                                     ; safe vector to land on.

.include "wozmon.s"

.segment "RESETVEC"
    .word   NMI_HANDLER     ; NMI vector
    .word   RESET           ; RESET vector
    .word   IRQ_HANDLER     ; IRQ vector
