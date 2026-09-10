;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;       SERIAL LOAD/SAVE -- moves the numbered program in editor.s's
;       source buffer (SRC_START-SRC_END) across the real RS232/ACIA
;       link. Reached only as shell commands now (editor.s's DISPATCH_CMD
;       calls DO_LOAD/DO_SAVE directly) -- not independent <addr>R
;       targets like the pre-shell LOAD_ENTRY/SAVE_ENTRY were. The browser
;       side (web/) handles naming, local storage, and download/import on
;       the human end; the ROM only ever sees a byte stream over the wire.
;
;       Both directions speak real decimal-ASCII text -- "<number> <text>"
;       per line, CR-separated -- not editor.s's internal 2-byte-binary-
;       prefix buffer format, so a saved program is a genuinely readable
;       text file a person can open, edit, and re-import. DO_LOAD clears
;       the program first (DO_NEW), then splits the incoming byte stream
;       on CR and feeds each completed line through editor.s's own
;       PROCESS_LINE -- the exact same classify-and-store path the
;       interactive shell prompt uses, just fed from the serial idle-
;       timeout reader below instead of the keyboard. That one reuse is
;       what makes LOAD accept both a real numbered save file *and* a
;       plain unnumbered text file someone hand-typed and imported: an
;       unnumbered line auto-numbers, exactly as it would if typed at the
;       shell prompt directly. LOADMODE (editor.s) keeps a line that
;       happens to read "RUN"/"QUIT"/etc from being misread as a command
;       mid-transfer -- see PROCESS_LINE's own header for why.
;
;       LOAD frames itself with an idle timeout: once ~0.33s (256
;       iterations of CHLL's own delay, bios.s) passes with no new byte
;       after the first one arrives, the transfer is considered finished
;       (a trailing partial line with no final CR is still processed). A
;       real serial paste behaves the same way -- there's no length
;       prefix, just "keep going until it stops coming."
;
;       SAVE is the browser-initiated direction, so its framing can be
;       exact rather than timing-based: the whole transfer is bracketed in
;       real ASCII control characters, STX ($02) before and ETX ($03)
;       after -- the same "Start/End of Text" framing bytes serial gear of
;       this era actually used for exactly this purpose, not an invented
;       marker. The browser's capture listens for STX, buffers until ETX,
;       and offers what's between them (not the markers themselves) as the
;       downloadable file.
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

FRAME_STX = $02        ; note: named FRAME_STX, not STX -- ca65 reserves
FRAME_ETX = $03        ; "STX" itself as the store-X-register mnemonic
IDLE_ITERS = 256      ; ~0.33s of silence (256 * CHLL's ~1275-cycle delay)
                       ; before DO_LOAD decides the transfer is done --
                       ; comfortably longer than any inter-byte gap at real
                       ; ACIA baud rates, short enough not to feel hung.

; ==========================================================================
; DO_LOAD -- clears the program (DO_NEW), then reads a byte stream from
; the ACIA, splitting on CR into lines and PROCESS_LINE-ing each one, until
; IDLE_ITERS worth of silence follows the first received byte. A final
; partial line (no trailing CR before the timeout) is processed too.
; ==========================================================================
DO_LOAD:
    jsr DO_NEW
    lda #1
    sta LOADMODE
    stz WAITCNT
    stz HASRECV
    ldy #0                       ; write index into LINE_BUF for the line in progress
dload_loop:
    jsr READCHAR
    bcc dload_nobyte
    cmp #CR
    beq dload_gotcr
    cpy #LINE_BUF_SIZE-1
    bcs dload_continue           ; line too long -- drop extra chars (defensive)
    sta LINE_BUF,y
    iny
dload_continue:
    lda #1
    sta HASRECV
    stz WAITCNT
    jmp dload_loop
dload_gotcr:
    ; READCHAR already echoed the bare CR we just read (every caller gets
    ; that echo, unconditionally) -- but the wire format itself is
    ; deliberately bare-CR-separated (see header), and a lone CR on a real
    ; terminal returns the cursor to column 0 without advancing a row, so
    ; without this the *next* line's echoed text would overwrite this
    ; one's in place instead of appearing on its own line. Same fix
    ; READLINE_ECHO already applies after its own CR for the same reason.
    lda #LF
    jsr CHROUT
    sty LINELEN
    jsr PROCESS_LINE
    ldy #0
    lda #1
    sta HASRECV
    stz WAITCNT
    jmp dload_loop
dload_nobyte:
    lda HASRECV
    beq dload_loop                ; nothing received yet -- wait indefinitely
    jsr CHLL                      ; fixed per-iteration delay, see IDLE_ITERS
    inc WAITCNT
    bne dload_loop
    ; WAITCNT wrapped $FF->$00 -- IDLE_ITERS idle iterations elapsed
    cpy #0
    beq dload_finished             ; no partial line pending
    sty LINELEN
    jsr PROCESS_LINE
dload_finished:
    stz LOADMODE
    rts

; ==========================================================================
; DO_SAVE -- sends the program out the ACIA as decimal-ASCII "<num> <text>"
; lines (CR-separated), bracketed by STX/ETX (see header).
; ==========================================================================
DO_SAVE:
    lda #FRAME_STX
    jsr CHROUT
    lda #<SRC_START
    sta SRCPTR
    lda #>SRC_START
    sta SRCPTR+1
dsave_loop:
    jsr DECODE_ENTRY
    bcc dsave_done
    lda CURNUM
    sta DECVAL
    lda CURNUM+1
    sta DECVAL+1
    jsr PRDEC
    lda #' '
    jsr CHROUT
    lda ENTRYLEN
    sec
    sbc #1
    sta CPYCNT
    ldy #2
dsave_txt:
    cpy CPYCNT
    beq dsave_endline
    lda (SRCPTR),y
    jsr CHROUT
    iny
    bra dsave_txt
dsave_endline:
    lda #CR
    jsr CHROUT
    clc
    lda SRCPTR
    adc ENTRYLEN
    sta SRCPTR
    bcc dsave_loop
    inc SRCPTR+1
    jmp dsave_loop
dsave_done:
    lda #FRAME_ETX
    jsr CHROUT
    lda #<MSG_OK
    ldy #>MSG_OK
    jsr STROUT
    rts
