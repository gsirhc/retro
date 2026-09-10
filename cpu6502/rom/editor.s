;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;       RESIDENT COMMAND SHELL + TWO-PASS ASSEMBLER. Reached from Wozmon
;       once via <addr>R (SHELL_ENTRY, a fixed trampoline at $8000 -- see
;       bios.s) -- unlike the old NEW/ENTER/LIST/ASM surface, this is now
;       the *only* new address Wozmon needs to know about. Once inside,
;       the shell owns the terminal and takes a real
;       command line at its own "*" prompt: a line starting with a decimal
;       number stores/replaces/deletes that program line (BASIC-style, so
;       inserting between existing lines needs no renumbering); anything
;       else is tried as a command word (NEW/LIST/EDIT/ASM/RUN/LOAD/SAVE/
;       QUIT); anything that's neither is auto-numbered onto the end of
;       the program (current highest number + 10, same step AUTO used on
;       classic BASICs), so plain unnumbered entry still works exactly
;       like the old sequential ENTER did. QUIT returns to Wozmon's own
;       "\" prompt. RUN's resume convention ("JMP <addr>" for a program
;       that wants to come back rather than trap on BRK) now targets
;       SHELL_PROMPT, not Wozmon -- the shell is "home" for program
;       development. See CGOAC6502_REVIEW.md for the full design writeup.
;
;       Source is plain ASCII text, one instruction per line:
;         LABEL: MNEMONIC OPERAND    ; comment
;       Numbers are hex only, "$"-prefixed (Wozmon's own convention) --
;       no decimal, no <// >> byte-select operators, no ORG/.BYTE/.WORD
;       directives, no indirect addressing, no 65C02 bit-ops (RMBx etc) --
;       all real v1 scope cuts, documented in the project plan, not bugs.
;       Labels are truncated to 6 significant characters. Source must
;       already be uppercase (matches READCHAR's own FORCE_UPPER). Line
;       *numbers* (the shell's own addressing scheme for editing) are
;       decimal -- a separate concept from the hex operand syntax above.
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

.setcpu "65C02"

; ---- the real $0000-$3FFF RAM ceiling (see bus.cpp / CGOAC6502_REVIEW.md)
; split three ways: source text ("high RAM"), a symbol table, and the
; assembled object code ("low RAM"), right after Wozmon's own line buffer.
SRC_START = $3000
SRC_END   = $4000
SYM_START = $2C00
SYM_END   = $3000
OBJ_START = $0400
OBJ_END   = $2C00

; Scratch line buffer: the back half of Wozmon's own WOZMON_BUFFER segment
; ($300-$3FF) -- Wozmon only ever uses the first 128 of its reserved 256
; bytes (wozmon.s: WOZMON_BUFFER: .res 128), and the assembler/shell are
; never resident at the same time Wozmon's own line-read loop is running,
; the same non-concurrency argument this codebase already relies on for
; Wozmon's zero-page vars overlapping BASIC's (now-removed) input buffer.
LINE_BUF      = $380
LINE_BUF_SIZE = 64

SYM_NAME_LEN  = 6
SYM_ENTRY_LEN = 8    ; 6-char space-padded name + 2-byte address

LIST_PAGE = 20       ; lines per LIST page before a --MORE-- pause
AUTO_STEP = 10        ; auto-numbering increment, same convention as BASIC's AUTO

; PRINT_ENTRY's column layout -- label field width (including its trailing
; separator space; a longer label still gets at least one space before the
; mnemonic, just no extra padding) and operand field width (comments align
; to this column when one follows; a longer operand just gets a single
; space before ";" instead of breaking alignment further right).
LABEL_FIELD   = 8
OPERAND_FIELD = 10

; addressing-mode indices -- also OPTAB's per-row column order
AM_IMP  = 0
AM_IMM  = 1
AM_ZP   = 2
AM_ZPX  = 3
AM_ZPY  = 4
AM_ABS  = 5
AM_ABSX = 6
AM_ABSY = 7
AM_REL  = 8
AM_UNSUPPORTED = $02   ; sentinel in OPTAB -- see the table's own header

; ---- zero page (avoiding Wozmon's $24-$2B and load.s's $4B/$4D/$4E-$4F --
; everything else is free now that BASIC isn't part of this ROM) ---------
.segment "ZEROPAGE"
SRCPTR   := $30   ; 2: current line's start in the source buffer
ASMPC    := $32   ; 2: current assembly address
LINELEN  := $36   ; 1: length of text copied into LINE_BUF this line
AMODE    := $37   ; 1: resolved addressing mode (AM_* above)
ASIZE    := $38   ; 1: instruction size in bytes (0 = nothing this line)
OPVAL    := $39   ; 2: resolved operand value (literal or symbol lookup)
OPCODE   := $3B   ; 1: matched opcode byte for this mnemonic/mode
TABPTR   := $3C   ; 2: opcode table scan pointer
SYMPTR   := $3E   ; 2: symbol table scan pointer
HASLABEL := $50   ; 1: operand is a label reference, not a literal
LABOFF   := $51   ; 1: LINE_BUF offset of the operand's label text
LABLEN   := $52   ; 1: length of same
DEFOFF   := $53   ; 1: LINE_BUF offset of a leading "LABEL:" definition
DEFLEN   := $54   ; 1: length of same (0 = no label defined this line)
CURSOR   := $55   ; 1: parse position within LINE_BUF
MNEMOFF  := $56   ; 1: LINE_BUF offset of the mnemonic
ERRFLAG  := $57   ; 1: non-zero once an error has been reported
COPYIDX  := $58   ; 1: scratch read index used while copying into the symtab
SAVEDCUR := $59   ; 1: scratch -- CURSOR checkpoint for the label/mnemonic probe
IDXFLAG  := $5A   ; 1: scratch -- 0 none, 1 ",X", 2 ",Y" (CHECK_INDEX_SUFFIX)
LABPTR   := $5B   ; 2: scratch -- LINE_BUF+LABOFF, built once per SYM_LOOKUP
TEMP16   := $5D   ; 2: scratch (branch-offset arithmetic, bounds checks)
WAITCNT  := $5F   ; 1: load.s's DO_LOAD -- idle-timeout iteration count
HASRECV  := $60   ; 1: load.s's DO_LOAD -- true once the first byte arrives
LOADMODE := $8A   ; 1: PROCESS_LINE -- non-zero while DO_LOAD is ingesting
                   ;    lines, so an unnumbered line always auto-numbers
                   ;    instead of risking a command-word match (a plain-
                   ;    text import whose content happens to read "RUN" or
                   ;    "QUIT" must never hijack the load)

; ---- shell / line-storage engine zero page (new) ------------------------
CURNUM     := $61   ; 2: DECODE_ENTRY's decoded line number for the entry at SRCPTR
ENTRYLEN   := $63   ; 1: DECODE_ENTRY's decoded entry length (2 + text + CR)
LINENUM    := $64   ; 2: target line number for STORE_LINE / SCAN_BUFFER
MATCHPTR   := $66   ; 2: SCAN_BUFFER -- ptr to existing line == LINENUM
MATCHLEN   := $68   ; 1: SCAN_BUFFER -- length of that line (0 = not found)
GTPTR      := $69   ; 2: SCAN_BUFFER -- first line with number > LINENUM
GTFOUND    := $6B   ; 1: SCAN_BUFFER -- whether GTPTR is valid
ENDPTR     := $6C   ; 2: SCAN_BUFFER -- position of the 0,0 end-of-buffer sentinel
LASTNUM    := $6E   ; 2: SCAN_BUFFER -- number of the last real line scanned
INSPTR     := $70   ; 2: STORE_LINE -- resolved insertion point
NEWTOTLEN  := $72   ; 1: STORE_LINE -- byte length of the entry being inserted
MVLEN      := $73   ; 2: MEMMOVE_UP/DOWN -- byte count
MVSRC      := $75   ; 2: MEMMOVE_UP/DOWN -- source pointer
MVDST      := $77   ; 2: MEMMOVE_UP/DOWN -- dest pointer
RESTOFF    := $79   ; 1: LINE_BUF offset of the text passed to STORE_LINE
RESTLEN    := $7A   ; 1: length of same
CPYCNT     := $7B   ; 1: scratch byte-copy counter (STORE_LINE, PRINT_ENTRY)
DECVAL     := $7C   ; 2: DEC_PARSE / PRDEC working value
ARGNUM     := $7E   ; 2: PARSE_ARG -- LIST/EDIT's optional decimal argument
ARGGIVEN   := $80   ; 1: PARSE_ARG -- whether an argument was supplied
TOKOFF     := $81   ; 1: SHELL_PROMPT -- LINE_BUF offset of the command token
TOKLEN     := $82   ; 1: length of same
TOKPTR     := $83   ; 2: LINE_BUF+TOKOFF, built once per dispatch attempt
CMPPTR     := $85   ; 2: TOKEN_EQ -- pointer to the literal being compared
PAGECNT    := $87   ; 1: DO_LIST -- lines printed so far this page
PRDEC_ANY  := $88   ; 1: PRDEC -- whether a nonzero digit has printed yet
PRDEC_DIG  := $89   ; 1: PRDEC -- current digit's subtraction count

.segment "EDITOR"

; ==========================================================================
; SHELL_START / SHELL_PROMPT -- the shell's own command loop. SHELL_START
; prints the startup banner once, then falls into the loop (shell_loop).
; SHELL_PROMPT is the public "resume here" address a running program's own
; JMP should target (also where bios.s's NMI_HANDLER lands on a Ctrl-C
; break) -- mirrors wozmon.s's own START_WOZ (banner) vs GETLINE (bare
; reprompt) split, except a resumed/interrupted program may have left the
; cursor mid-line with no guarantee its own output ended in CR/LF, so
; SHELL_PROMPT prints a fresh one before reprompting; shell_loop, the
; ordinary per-command continuation, skips that, since whatever the shell
; itself just printed already ends in its own CR/LF -- printing another
; would open a spurious blank line before every routine reprompt. Reached
; from Wozmon only through the fixed SHELL_ENTRY trampoline pinned at
; $8000 (bios.s) -- see that file's header comment for why the jump exists
; instead of just landing here directly.
; ==========================================================================
SHELL_START:
    ; Reached via Wozmon's real "AAAA R" run syntax -- typing "8000R" makes
    ; genuine Wozmon echo "8000: <byte>" first (its own real EXAMINE-mode
    ; echo, triggered whenever a hex address is terminated by a non-hex
    ; char -- see wozmon.s's NOTCR/ECHO), with no trailing CR/LF of its own
    ; before the JMP. Without one here, the banner runs right into that
    ; echo on the same row (e.g. "8000: 4C6502 ASSEMBLY CODER").
    lda #CR
    jsr CHROUT
    lda #LF
    jsr CHROUT
    lda #<MSG_SHELL
    ldy #>MSG_SHELL
    jsr STROUT
    bra shell_loop
SHELL_PROMPT:
    lda #CR
    jsr CHROUT
    lda #LF
    jsr CHROUT
shell_loop:
    lda #'>'
    jsr CHROUT
    jsr READLINE_ECHO
    stz LOADMODE
    jsr PROCESS_LINE
    jmp shell_loop

MSG_SHELL:   .byte "6502 ASSEMBLY CODER", CR, LF, 0
MSG_BADLINE: .byte "?LINE", CR, LF, 0

; ==========================================================================
; PROCESS_LINE -- classifies and acts on one already-read line (LINE_BUF/
; LINELEN): blank -> ignored; leading digit -> STORE_LINE at that number
; (may delete, if nothing follows); else, unless LOADMODE is set, tried as
; a command word (DISPATCH_CMD); anything left over auto-numbers onto the
; end of the program. Shared by the interactive shell prompt and DO_LOAD's
; per-line serial ingestion (load.s) -- LOADMODE keeps a plain-text import
; whose content happens to read "RUN"/"QUIT"/etc from hijacking a LOAD in
; progress by skipping command dispatch entirely during it.
; ==========================================================================
PROCESS_LINE:
    stz CURSOR
    jsr SKIP_SPACES
    lda CURSOR
    cmp LINELEN
    bne pl_notblank
    jmp pl_rts
pl_notblank:
    ldy CURSOR
    lda LINE_BUF,y
    cmp #'0'
    bcc pl_notdigit
    cmp #'9'+1
    bcs pl_notdigit
    jsr DEC_PARSE
    lda DECVAL
    sta LINENUM
    lda DECVAL+1
    sta LINENUM+1
    ora LINENUM
    bne pl_numok
    lda #<MSG_BADLINE
    ldy #>MSG_BADLINE
    jsr STROUT
    rts
pl_numok:
    ldy CURSOR
    cpy LINELEN
    bcs pl_notext
    lda LINE_BUF,y
    cmp #' '
    bne pl_notext
    inc CURSOR
pl_notext:
    lda CURSOR
    sta RESTOFF
    lda LINELEN
    sec
    sbc CURSOR
    sta RESTLEN
    jsr STORE_LINE
    rts
pl_notdigit:
    lda LOADMODE
    bne pl_autonum
    lda CURSOR
    sta TOKOFF
    clc
    lda #<LINE_BUF
    adc TOKOFF
    sta TOKPTR
    lda #>LINE_BUF
    adc #0
    sta TOKPTR+1
pl_toklen:
    ldy CURSOR
    cpy LINELEN
    bcs pl_havetok
    lda LINE_BUF,y
    cmp #'A'
    bcc pl_havetok
    cmp #'Z'+1
    bcs pl_havetok
    inc CURSOR
    jmp pl_toklen
pl_havetok:
    lda CURSOR
    sec
    sbc TOKOFF
    sta TOKLEN
    jsr DISPATCH_CMD
    bcs pl_rts
pl_autonum:
    jsr AUTO_NUMBER
    stz RESTOFF
    lda LINELEN
    sta RESTLEN
    jsr STORE_LINE
pl_rts:
    rts

; ==========================================================================
; DISPATCH_CMD -- TOKPTR/TOKLEN name a candidate command word. Tries each
; known word (exact length + byte match, same discipline as MNEM_LOOKUP's
; mnemonic match). Carry SET + dispatched if matched, CLEAR otherwise
; (caller falls back to auto-numbered program-line entry).
; ==========================================================================
DISPATCH_CMD:
    lda #<CMD_NEW
    sta CMPPTR
    lda #>CMD_NEW
    sta CMPPTR+1
    lda #3
    jsr TOKEN_EQ
    bcc dc_list
    jsr DO_NEW
    sec
    rts
dc_list:
    lda #<CMD_LIST
    sta CMPPTR
    lda #>CMD_LIST
    sta CMPPTR+1
    lda #4
    jsr TOKEN_EQ
    bcc dc_edit
    jsr PARSE_ARG
    jsr DO_LIST
    sec
    rts
dc_edit:
    lda #<CMD_EDIT
    sta CMPPTR
    lda #>CMD_EDIT
    sta CMPPTR+1
    lda #4
    jsr TOKEN_EQ
    bcc dc_asm
    jsr PARSE_ARG
    jsr DO_EDIT
    sec
    rts
dc_asm:
    lda #<CMD_ASM
    sta CMPPTR
    lda #>CMD_ASM
    sta CMPPTR+1
    lda #3
    jsr TOKEN_EQ
    bcc dc_run
    jsr DO_ASM
    sec
    rts
dc_run:
    lda #<CMD_RUN
    sta CMPPTR
    lda #>CMD_RUN
    sta CMPPTR+1
    lda #3
    jsr TOKEN_EQ
    bcc dc_load
    jmp OBJ_START            ; never returns -- see editor.s header
dc_load:
    lda #<CMD_LOAD
    sta CMPPTR
    lda #>CMD_LOAD
    sta CMPPTR+1
    lda #4
    jsr TOKEN_EQ
    bcc dc_save
    jsr DO_LOAD
    sec
    rts
dc_save:
    lda #<CMD_SAVE
    sta CMPPTR
    lda #>CMD_SAVE
    sta CMPPTR+1
    lda #4
    jsr TOKEN_EQ
    bcc dc_quit
    jsr DO_SAVE
    sec
    rts
dc_quit:
    lda #<CMD_QUIT
    sta CMPPTR
    lda #>CMD_QUIT
    sta CMPPTR+1
    lda #4
    jsr TOKEN_EQ
    bcc dc_none
    jmp START_WOZ             ; never returns
dc_none:
    clc
    rts

CMD_NEW:  .byte "NEW"
CMD_LIST: .byte "LIST"
CMD_EDIT: .byte "EDIT"
CMD_ASM:  .byte "ASM"
CMD_RUN:  .byte "RUN"
CMD_LOAD: .byte "LOAD"
CMD_SAVE: .byte "SAVE"
CMD_QUIT: .byte "QUIT"

; A = length of the literal at (CMPPTR). Compares against LINE_BUF at
; (TOKPTR), length TOKLEN. Carry SET if an exact match, CLEAR otherwise.
TOKEN_EQ:
    cmp TOKLEN
    bne te_no
    tay
    beq te_yes
te_loop:
    dey
    lda (TOKPTR),y
    cmp (CMPPTR),y
    bne te_no
    tya
    bne te_loop
te_yes:
    sec
    rts
te_no:
    clc
    rts

; ==========================================================================
; PARSE_ARG -- LIST/EDIT's optional trailing decimal argument. CURSOR sits
; right after the command word. Sets ARGGIVEN/ARGNUM.
; ==========================================================================
PARSE_ARG:
    jsr SKIP_SPACES
    stz ARGGIVEN
    lda CURSOR
    cmp LINELEN
    bcs pa_rts
    ldy CURSOR
    lda LINE_BUF,y
    cmp #'0'
    bcc pa_rts
    cmp #'9'+1
    bcs pa_rts
    jsr DEC_PARSE
    lda DECVAL
    sta ARGNUM
    lda DECVAL+1
    sta ARGNUM+1
    lda #1
    sta ARGGIVEN
pa_rts:
    rts

; ==========================================================================
; DO_NEW -- clears the source buffer (2-byte 0,0 sentinel at SRC_START).
; ==========================================================================
DO_NEW:
    stz SRC_START
    stz SRC_START+1
    lda #<MSG_OK
    ldy #>MSG_OK
    jsr STROUT
    rts

; ==========================================================================
; DECODE_ENTRY -- decodes the entry at SRCPTR. Carry CLEAR if it's the
; end-of-buffer sentinel (0,0 -- SRCPTR unchanged). Carry SET otherwise:
; CURNUM = this entry's line number, ENTRYLEN = its total byte length
; (2 + text + CR). Does not advance SRCPTR.
; ==========================================================================
DECODE_ENTRY:
    ldy #0
    lda (SRCPTR),y
    sta CURNUM
    iny
    lda (SRCPTR),y
    sta CURNUM+1
    ora CURNUM
    bne de_real
    clc
    rts
de_real:
    ldy #2
de_findcr:
    lda (SRCPTR),y
    cmp #CR
    beq de_foundcr
    iny
    bne de_findcr
de_foundcr:
    iny
    sty ENTRYLEN
    sec
    rts

; ==========================================================================
; PRINT_ENTRY -- prints the entry decoded at SRCPTR/CURNUM/ENTRYLEN as a
; column-aligned listing line:
;   <decimal num> LABEL:  MNM OPERAND   ; comment
; Reformats on the fly for display -- the *stored* text is untouched free-
; form ASCII (however the user actually typed it), this just re-derives
; the label/mnemonic/operand/comment field boundaries the same way
; PARSE_LINE does (colon ends a label, a mnemonic is always exactly 3
; characters -- MNEM_LOOKUP's own assumption) purely to print them padded.
; Deliberately does none of PARSE_LINE's real validation (MNEM_LOOKUP,
; addressing-mode resolution): an unassembled or even broken line still
; lists sensibly, same as the raw dump this replaces did. Comments
; (introduced by ';', stripped before real assembly by ASM_READLINE) are
; preserved here since this is a display pass, not the real parse.
; Shared by DO_LIST and DO_EDIT.
; ==========================================================================
PRINT_ENTRY:
    lda CURNUM
    sta DECVAL
    lda CURNUM+1
    sta DECVAL+1
    jsr PRDEC
    lda #' '
    jsr CHROUT

    ; Copy the raw entry text (excluding the 2-byte number prefix and the
    ; trailing CR) into LINE_BUF/LINELEN, verbatim -- unlike ASM_READLINE,
    ; the comment stays in. Safe to reuse LINE_BUF/CURSOR/DEFOFF/DEFLEN/
    ; SAVEDCUR: LIST/EDIT never run concurrently with ASM's own use of them.
    lda ENTRYLEN
    sec
    sbc #1
    sta CPYCNT
    ldy #2
    ldx #0
@copyloop:
    cpy CPYCNT
    beq @copydone
    lda (SRCPTR),y
    sta LINE_BUF,x
    iny
    inx
    bra @copyloop
@copydone:
    stx LINELEN

    ; --- label field (mirrors PARSE_LINE's own scan-to-colon) ---
    stz CURSOR
    jsr SKIP_SPACES
    lda CURSOR
    sta SAVEDCUR
    stz DEFLEN
    lda CURSOR
    cmp LINELEN
    bcs @havelabel        ; nothing after the number at all
    ldy CURSOR
    lda LINE_BUF,y
    jsr IS_ALPHA
    bcc @havelabel
@scanlabel:
    ldy CURSOR
    cpy LINELEN
    bcs @notlabel
    lda LINE_BUF,y
    cmp #':'
    beq @foundcolon
    jsr IS_ALNUM
    bcc @notlabel
    inc CURSOR
    bra @scanlabel
@foundcolon:
    lda SAVEDCUR
    sta DEFOFF
    lda CURSOR
    sec
    sbc SAVEDCUR
    inc a
    sta DEFLEN            ; label length including the colon itself
    inc CURSOR
    bra @havelabel
@notlabel:
    lda SAVEDCUR
    sta CURSOR
@havelabel:
    ldx #0
    lda DEFLEN
    beq @labelpad
    ldy DEFOFF
@labelchar:
    cpx DEFLEN
    beq @labelsep
    lda LINE_BUF,y
    jsr CHROUT
    iny
    inx
    bra @labelchar
@labelsep:
    lda #' '               ; guaranteed separator, even past a long label
    jsr CHROUT
    inx
@labelpad:
    cpx #LABEL_FIELD
    bcs @labeldone
    lda #' '
    jsr CHROUT
    inx
    bra @labelpad
@labeldone:

    jsr SKIP_SPACES
    lda CURSOR
    cmp LINELEN
    bcc @havemnem
    jmp pe_crlf            ; label-only (or fully blank) line -- done

    ; --- mnemonic field (fixed 3 chars, same assumption PARSE_LINE makes;
    ; a ';' right here means no mnemonic at all, just a comment) ---
@havemnem:
    ldy CURSOR
    lda LINE_BUF,y
    cmp #';'
    bne @realmnem
    ldx #0
@mnemplaceholder:
    cpx #4
    bcs @nomnemdone
    lda #' '
    jsr CHROUT
    inx
    bra @mnemplaceholder
@nomnemdone:
    stz CPYCNT
    jmp pe_gotcomment
@realmnem:
    lda LINELEN
    sec
    sbc CURSOR
    cmp #3
    bcs @mnemok
    jmp pe_dumprest        ; fewer than 3 chars left and not a comment -- garbage tail
@mnemok:
    ldy CURSOR
    lda LINE_BUF,y
    jsr CHROUT
    iny
    lda LINE_BUF,y
    jsr CHROUT
    iny
    lda LINE_BUF,y
    jsr CHROUT
    lda CURSOR
    clc
    adc #3
    sta CURSOR
    lda #' '
    jsr CHROUT
    jsr SKIP_SPACES

    ; --- operand field, up to ';' or end of line ---
    stz CPYCNT
@operandloop:
    ldy CURSOR
    cpy LINELEN
    bcs @operanddone
    lda LINE_BUF,y
    cmp #';'
    beq @operanddone
    jsr CHROUT
    inc CURSOR
    inc CPYCNT
    bra @operandloop
@operanddone:
    lda CURSOR
    cmp LINELEN
    bcs pe_crlf             ; ran off the end -- no comment, stop (no trailing pad)

    ; --- comment: pad the operand field, then print it ---
pe_gotcomment:
    lda CPYCNT
@padloop:
    cmp #OPERAND_FIELD
    bcs @paddone
    pha
    lda #' '
    jsr CHROUT
    pla
    inc a
    bra @padloop
@paddone:
    lda #';'
    jsr CHROUT
    lda #' '
    jsr CHROUT
    inc CURSOR              ; skip the ';' itself in the source
    jsr SKIP_SPACES          ; drop leading space(s) after ';' for tidy realignment
pe_dumprest:
    ldy CURSOR
    cpy LINELEN
    bcs pe_crlf
    lda LINE_BUF,y
    jsr CHROUT
    inc CURSOR
    jmp pe_dumprest
pe_crlf:
    lda #CR
    jsr CHROUT
    lda #LF
    jsr CHROUT
    rts

; ==========================================================================
; DO_LIST -- ARGGIVEN/ARGNUM (from PARSE_ARG) select the starting line;
; walks and prints every line from there, paging every LIST_PAGE lines.
; ==========================================================================
DO_LIST:
    lda #<SRC_START
    sta SRCPTR
    lda #>SRC_START
    sta SRCPTR+1
    stz PAGECNT
dl_loop:
    jsr DECODE_ENTRY
    bcc dl_done
    lda ARGGIVEN
    beq dl_print
    lda CURNUM+1
    cmp ARGNUM+1
    bcc dl_skip
    bne dl_print
    lda CURNUM
    cmp ARGNUM
    bcc dl_skip
dl_print:
    jsr PRINT_ENTRY
    inc PAGECNT
    lda PAGECNT
    cmp #LIST_PAGE
    bne dl_skip
    stz PAGECNT
    lda #<MSG_MORE
    ldy #>MSG_MORE
    jsr STROUT
dl_waitkey:
    jsr READCHAR
    bcc dl_waitkey
    lda #CR
    jsr CHROUT
    lda #LF
    jsr CHROUT
dl_skip:
    clc
    lda SRCPTR
    adc ENTRYLEN
    sta SRCPTR
    bcc dl_loop
    inc SRCPTR+1
    jmp dl_loop
dl_done:
    rts

MSG_MORE: .byte "--MORE--", 0

; ==========================================================================
; DO_EDIT -- ARGGIVEN/ARGNUM name the line to edit. Prints it, then reads
; one fresh line of raw text and STORE_LINEs it over the same number
; (blank input deletes it -- the general STORE_LINE rule, no special case).
; ==========================================================================
DO_EDIT:
    lda ARGGIVEN
    bne de_have
    lda #<MSG_BADLINE
    ldy #>MSG_BADLINE
    jsr STROUT
    rts
de_have:
    lda ARGNUM
    sta LINENUM
    lda ARGNUM+1
    sta LINENUM+1
    jsr SCAN_BUFFER
    lda MATCHLEN
    bne de_found
    lda #<MSG_BADLINE
    ldy #>MSG_BADLINE
    jsr STROUT
    rts
de_found:
    lda MATCHPTR
    sta SRCPTR
    lda MATCHPTR+1
    sta SRCPTR+1
    jsr DECODE_ENTRY
    jsr PRINT_ENTRY
    lda #'>'
    jsr CHROUT
    lda #' '
    jsr CHROUT
    jsr READLINE_ECHO
    stz RESTOFF
    lda LINELEN
    sta RESTLEN
    jsr STORE_LINE
    rts

; ==========================================================================
; DO_ASM -- assembles the program currently in the source buffer. Prints
; "OK" or "ERR LINE <n>" (n = the real program line number, not a
; sequential count), then returns to the shell prompt.
; ==========================================================================
DO_ASM:
    stz ERRFLAG
    jsr ASM_PASS1
    lda ERRFLAG
    bne da_err
    jsr ASM_PASS2
    lda ERRFLAG
    bne da_err
    lda #<MSG_OK
    ldy #>MSG_OK
    jsr STROUT
    rts
da_err:
    lda #<MSG_ERR
    ldy #>MSG_ERR
    jsr STROUT
    lda CURNUM
    sta DECVAL
    lda CURNUM+1
    sta DECVAL+1
    jsr PRDEC
    lda #CR
    jsr CHROUT
    lda #LF
    jsr CHROUT
    rts

MSG_OK:  .byte "Ok", CR, LF, 0    ; shared completion message -- ASM (clean assemble), NEW, SAVE (load.s)
MSG_ERR: .byte "ERR LINE ", 0

; ==========================================================================
; PASS 1 -- walks every line, records label definitions against the
; address they'd assemble to, without emitting any bytes.
; ==========================================================================
ASM_PASS1:
    jsr CLEAR_SYMTAB
    jsr RESET_SCAN
p1_loop:
    jsr ASM_READLINE
    bcc p1_done
    stz CURSOR
    jsr PARSE_LINE
    lda ERRFLAG
    bne p1_ret
    lda DEFLEN
    beq p1_nodef
    jsr SYM_DEFINE
    lda ERRFLAG
    bne p1_ret
p1_nodef:
    jsr ADVANCE_PC
    jmp p1_loop
p1_done:
p1_ret:
    rts

; ==========================================================================
; PASS 2 -- walks every line again, this time resolving operands (labels
; are now fully known) and writing real bytes into the object region.
; ==========================================================================
ASM_PASS2:
    jsr RESET_SCAN
p2_loop:
    jsr ASM_READLINE
    bcc p2_done
    stz CURSOR
    jsr PARSE_LINE
    lda ERRFLAG
    bne p2_ret
    lda ASIZE
    beq p2_nosize
    jsr EMIT_INSTRUCTION
    lda ERRFLAG
    bne p2_ret
p2_nosize:
    jsr ADVANCE_PC
    jmp p2_loop
p2_done:
p2_ret:
    rts

; Shared by both passes: rewind the source pointer and PC.
RESET_SCAN:
    lda #<SRC_START
    sta SRCPTR
    lda #>SRC_START
    sta SRCPTR+1
    lda #<OBJ_START
    sta ASMPC
    lda #>OBJ_START
    sta ASMPC+1
    rts

ADVANCE_PC:
    lda ASIZE
    beq @rts
    clc
    adc ASMPC
    sta ASMPC
    bcc @rts
    inc ASMPC+1
@rts:
    rts

; Zeroes the whole symbol table so every entry's first byte reads 0
; ("empty") before pass 1 starts defining labels.
CLEAR_SYMTAB:
    lda #<SYM_START
    sta SYMPTR
    lda #>SYM_START
    sta SYMPTR+1
    lda #0
@loop:
    lda SYMPTR+1
    cmp #>SYM_END
    bne @clr
    lda SYMPTR
    cmp #<SYM_END
    bne @clr
    rts
@clr:
    ldy #0
    lda #0
    sta (SYMPTR),y
    inc SYMPTR
    bne @loop
    inc SYMPTR+1
    jmp @loop

; ==========================================================================
; ASM_READLINE -- copies one source line (comment-stripped) into LINE_BUF,
; sets LINELEN, advances SRCPTR past it, and sets CURNUM to the line's
; real (decimal) number for error reporting. Carry SET = a line was read;
; carry CLEAR = source exhausted (the 0,0 sentinel).
; ==========================================================================
ASM_READLINE:
    ldy #0
    lda (SRCPTR),y
    bne ar_notend
    iny
    lda (SRCPTR),y
    bne ar_notend
    clc
    rts
ar_notend:
    ldy #0
    lda (SRCPTR),y
    sta CURNUM
    ldy #1
    lda (SRCPTR),y
    sta CURNUM+1
    clc
    lda SRCPTR
    adc #2
    sta SRCPTR
    bcc ar_noc
    inc SRCPTR+1
ar_noc:
    stz LINELEN
    stz TEMP16          ; reused here as the "in a comment" flag
    ldy #0
@loop:
    lda (SRCPTR),y
    beq @hit_nul
    cmp #CR
    beq @hit_cr
    cmp #';'
    bne @notsemi
    lda #1
    sta TEMP16
@notsemi:
    lda TEMP16
    bne @nocopy
    ldx LINELEN
    cpx #LINE_BUF_SIZE-1
    bcs @nocopy          ; line too long -- truncate defensively
    lda (SRCPTR),y
    sta LINE_BUF,x
    inc LINELEN
@nocopy:
    iny
    bne @loop
    bra @advance          ; Y wrapped (256-byte line) -- bail defensively
@hit_cr:
    iny                    ; consume the CR itself too
    bra @advance
@hit_nul:                  ; leave the NUL unconsumed -- next call sees EOF
@advance:
    tya
    clc
    adc SRCPTR
    sta SRCPTR
    bcc @noc
    inc SRCPTR+1
@noc:
    sec
    rts

; ==========================================================================
; SCAN_BUFFER -- walks the whole source buffer once. Inputs: LINENUM (the
; number being searched for). Outputs: ENDPTR (end-of-buffer sentinel
; position), MATCHPTR/MATCHLEN (an existing line == LINENUM, MATCHLEN=0 if
; none), GTPTR/GTFOUND (the first line with number > LINENUM), LASTNUM
; (the highest line number seen, for AUTO_NUMBER).
; ==========================================================================
SCAN_BUFFER:
    lda #<SRC_START
    sta SRCPTR
    lda #>SRC_START
    sta SRCPTR+1
    stz MATCHLEN
    stz GTFOUND
sb_loop:
    jsr DECODE_ENTRY
    bcc sb_atend
    lda CURNUM
    sta LASTNUM
    lda CURNUM+1
    sta LASTNUM+1
    lda CURNUM
    cmp LINENUM
    bne sb_notequal
    lda CURNUM+1
    cmp LINENUM+1
    bne sb_notequal
    lda SRCPTR
    sta MATCHPTR
    lda SRCPTR+1
    sta MATCHPTR+1
    lda ENTRYLEN
    sta MATCHLEN
sb_notequal:
    lda GTFOUND
    bne sb_advance
    lda CURNUM+1
    cmp LINENUM+1
    bcc sb_advance
    bne sb_isgt
    lda CURNUM
    cmp LINENUM
    bcc sb_advance
    beq sb_advance
sb_isgt:
    lda SRCPTR
    sta GTPTR
    lda SRCPTR+1
    sta GTPTR+1
    lda #1
    sta GTFOUND
sb_advance:
    clc
    lda SRCPTR
    adc ENTRYLEN
    sta SRCPTR
    bcc sb_loop
    inc SRCPTR+1
    jmp sb_loop
sb_atend:
    lda SRCPTR
    sta ENDPTR
    lda SRCPTR+1
    sta ENDPTR+1
    rts

; ==========================================================================
; AUTO_NUMBER -- sets LINENUM = (highest stored line number) + AUTO_STEP,
; or AUTO_STEP if the buffer is empty. Classic BASIC AUTO convention.
; ==========================================================================
AUTO_NUMBER:
    lda #$FF
    sta LINENUM
    sta LINENUM+1
    jsr SCAN_BUFFER
    lda ENDPTR
    cmp #<SRC_START
    bne an_nonempty
    lda ENDPTR+1
    cmp #>SRC_START
    bne an_nonempty
    lda #AUTO_STEP
    sta LINENUM
    stz LINENUM+1
    rts
an_nonempty:
    clc
    lda LASTNUM
    adc #AUTO_STEP
    sta LINENUM
    lda LASTNUM+1
    adc #0
    sta LINENUM+1
    rts

; ==========================================================================
; CHECK_SYNTAX -- lightweight opcode/operand syntax check on the
; about-to-be-stored text at LINE_BUF[RESTOFF..RESTOFF+RESTLEN), reusing
; PARSE_LINE -- the exact same routine ASM_PASS1/2 use for real assembly --
; so "will ASM accept this line" is caught the instant it's typed (or
; retyped via EDIT, or streamed in via LOAD -- every path funnels through
; STORE_LINE), not only after a later ASM/RUN. Deliberately does NOT touch
; the symbol table: labels aren't resolved yet at entry time, and don't
; need to be -- PARSE_LINE's own mnemonic/addressing-mode checks never
; consult it (only SYM_DEFINE/EMIT_INSTRUCTION do, both PASS-only, see
; ASM_PASS1/2 above), matching the user-facing ask of checking the line's
; own shape, not whether every label it mentions happens to exist yet.
; Comments are excluded from the checked range first (same trigger
; ASM_READLINE's own strip uses, '; to end of text') by temporarily
; shortening LINELEN rather than copying to a scratch buffer -- PARSE_LINE
; only ever reads LINE_BUF, never writes it, so this is safe and the real
; LINELEN is restored before returning either way. Skipping this step
; would false-positive on a perfectly ordinary trailing comment: a bare
; ';' fails PARSE_OPERAND's label-operand path ("not a letter"). Carry SET
; on clean syntax; CLEAR (with "?SYNTAX" already printed) on bad.
; ==========================================================================
CHECK_SYNTAX:
    lda LINELEN
    sta TEMP16          ; reused here as a one-byte "real LINELEN" checkpoint
    ldy RESTOFF
cs_scan:
    cpy LINELEN
    bcs cs_dostrip
    lda LINE_BUF,y
    cmp #';'
    beq cs_dostrip
    iny
    bra cs_scan
cs_dostrip:
    sty LINELEN
    lda RESTOFF
    sta CURSOR
    stz ERRFLAG
    jsr PARSE_LINE
    lda TEMP16
    sta LINELEN
    lda ERRFLAG
    beq cs_ok
    lda #<MSG_SYNTAX
    ldy #>MSG_SYNTAX
    jsr STROUT
    clc
    rts
cs_ok:
    sec
    rts

MSG_SYNTAX: .byte "?SYNTAX", CR, LF, 0

; ==========================================================================
; STORE_LINE -- the single mutation primitive. Inputs: LINENUM (target
; number), LINE_BUF[RESTOFF..RESTOFF+RESTLEN) (the text, RESTLEN may be
; 0). Deletes any existing line with this number, then (if RESTLEN != 0)
; inserts the new line at the correct sorted position. Reports FULL if
; there isn't room. A syntactically bad non-empty line (CHECK_SYNTAX)
; is rejected outright -- nothing is deleted or stored, matching classic
; Microsoft BASIC's own immediate "?SYNTAX ERROR" on a bad line, not a
; silent accept caught only later at ASM/RUN time.
; ==========================================================================
STORE_LINE:
    jsr CHECK_SYNTAX
    bcs sl_syntaxok
    jmp sl_bailout        ; sl_bailout is far below -- bcc's range can't reach
sl_syntaxok:
    jsr SCAN_BUFFER
    lda MATCHLEN
    beq sl_nomatch
    lda MATCHPTR
    sta MVDST
    lda MATCHPTR+1
    sta MVDST+1
    clc
    lda MATCHPTR
    adc MATCHLEN
    sta MVSRC
    lda MATCHPTR+1
    adc #0
    sta MVSRC+1
    sec
    lda ENDPTR
    sbc MVSRC
    sta MVLEN
    lda ENDPTR+1
    sbc MVSRC+1
    sta MVLEN+1
    jsr MEMMOVE_DOWN
    sec
    lda ENDPTR
    sbc MATCHLEN
    sta ENDPTR
    lda ENDPTR+1
    sbc #0
    sta ENDPTR+1
    lda MATCHPTR
    sta INSPTR
    lda MATCHPTR+1
    sta INSPTR+1
    jmp sl_haveinsp
sl_nomatch:
    lda GTFOUND
    beq sl_useend
    lda GTPTR
    sta INSPTR
    lda GTPTR+1
    sta INSPTR+1
    jmp sl_haveinsp
sl_useend:
    lda ENDPTR
    sta INSPTR
    lda ENDPTR+1
    sta INSPTR+1
sl_haveinsp:
    lda RESTLEN
    bne sl_doinsert
    ldy #0
    lda #0
    sta (ENDPTR),y
    ldy #1
    lda #0
    sta (ENDPTR),y
    rts
sl_doinsert:
    lda RESTLEN
    clc
    adc #3
    sta NEWTOTLEN
    clc
    lda ENDPTR
    adc NEWTOTLEN
    sta TEMP16
    lda ENDPTR+1
    adc #0
    sta TEMP16+1
    lda TEMP16+1
    cmp #>(SRC_END-2)
    bcc sl_roomok
    bne sl_full
    lda TEMP16
    cmp #<(SRC_END-2)
    bcc sl_roomok
sl_full:
    lda #<MSG_FULL
    ldy #>MSG_FULL
    jsr STROUT
    rts
sl_roomok:
    sec
    lda ENDPTR
    sbc INSPTR
    sta MVLEN
    lda ENDPTR+1
    sbc INSPTR+1
    sta MVLEN+1
    lda INSPTR
    sta MVSRC
    lda INSPTR+1
    sta MVSRC+1
    clc
    lda INSPTR
    adc NEWTOTLEN
    sta MVDST
    lda INSPTR+1
    adc #0
    sta MVDST+1
    jsr MEMMOVE_UP
    ldy #0
    lda LINENUM
    sta (INSPTR),y
    iny
    lda LINENUM+1
    sta (INSPTR),y
    ldy #2
    ldx RESTOFF
    lda RESTLEN
    sta CPYCNT
sl_copytxt:
    lda CPYCNT
    beq sl_donetxt
    lda LINE_BUF,x
    sta (INSPTR),y
    inx
    iny
    dec CPYCNT
    bra sl_copytxt
sl_donetxt:
    lda #CR
    sta (INSPTR),y
    ; TEMP16 still holds ENDPTR(before this insert) + NEWTOTLEN from the
    ; bounds check above -- the real new end of buffer. MVDST is NOT
    ; reusable here: MEMMOVE_UP walks it down to nothing as its own
    ; working copy pointer (and leaves it untouched, misleadingly still
    ; "correct", only in the degenerate append case where MVLEN was 0 and
    ; the whole loop never ran -- exactly the case every earlier test that
    ; only ever appended happened to exercise).
    lda TEMP16
    sta ENDPTR
    lda TEMP16+1
    sta ENDPTR+1
    ldy #0
    lda #0
    sta (ENDPTR),y
    ldy #1
    sta (ENDPTR),y
    rts
sl_bailout:
    rts

MSG_FULL: .byte "FULL", CR, LF, 0

; ==========================================================================
; MEMMOVE_DOWN -- copies MVLEN bytes from MVSRC to MVDST, low address to
; high (safe when MVDST < MVSRC, i.e. closing a gap left by a deletion).
; ==========================================================================
MEMMOVE_DOWN:
    lda MVLEN
    ora MVLEN+1
    beq md_done
md_loop:
    ldy #0
    lda (MVSRC),y
    sta (MVDST),y
    inc MVSRC
    bne md_nosrchi
    inc MVSRC+1
md_nosrchi:
    inc MVDST
    bne md_nodsthi
    inc MVDST+1
md_nodsthi:
    lda MVLEN
    bne md_declo
    dec MVLEN+1
md_declo:
    dec MVLEN
    lda MVLEN
    ora MVLEN+1
    bne md_loop
md_done:
    rts

; ==========================================================================
; MEMMOVE_UP -- copies MVLEN bytes from MVSRC to MVDST, high address to
; low (safe when MVDST > MVSRC, i.e. opening a gap for an insertion).
; ==========================================================================
MEMMOVE_UP:
    lda MVLEN
    ora MVLEN+1
    beq mu_done
    clc
    lda MVSRC
    adc MVLEN
    sta MVSRC
    lda MVSRC+1
    adc MVLEN+1
    sta MVSRC+1
    lda MVSRC
    bne mu_s1
    dec MVSRC+1
mu_s1:
    dec MVSRC
    clc
    lda MVDST
    adc MVLEN
    sta MVDST
    lda MVDST+1
    adc MVLEN+1
    sta MVDST+1
    lda MVDST
    bne mu_d1
    dec MVDST+1
mu_d1:
    dec MVDST
mu_loop:
    ldy #0
    lda (MVSRC),y
    sta (MVDST),y
    lda MVSRC
    bne mu_s2
    dec MVSRC+1
mu_s2:
    dec MVSRC
    lda MVDST
    bne mu_d2
    dec MVDST+1
mu_d2:
    dec MVDST
    lda MVLEN
    bne mu_declo
    dec MVLEN+1
mu_declo:
    dec MVLEN
    lda MVLEN
    ora MVLEN+1
    bne mu_loop
mu_done:
    rts

; ==========================================================================
; DEC_PARSE -- parses up to 4 decimal digits at LINE_BUF+CURSOR (capped at
; 4 so the result can never exceed 9999, well clear of 16-bit overflow),
; advances CURSOR past them, sets DECVAL. Does not set carry/flags for
; "how many digits" -- callers that care check LINELEN/CURSOR themselves.
; ==========================================================================
DEC_PARSE:
    stz DECVAL
    stz DECVAL+1
    ldx #0
dp_loop:
    cpx #4
    beq dp_done
    ldy CURSOR
    cpy LINELEN
    bcs dp_done
    lda LINE_BUF,y
    cmp #'0'
    bcc dp_done
    cmp #'9'+1
    bcs dp_done
    jsr MUL10DEC
    ldy CURSOR
    lda LINE_BUF,y
    sec
    sbc #'0'
    clc
    adc DECVAL
    sta DECVAL
    bcc dp_nocarry
    inc DECVAL+1
dp_nocarry:
    inc CURSOR
    inx
    jmp dp_loop
dp_done:
    rts

; DECVAL = DECVAL * 10 (16-bit, in place). v*10 = v*8 + v*2.
MUL10DEC:
    asl DECVAL
    rol DECVAL+1
    lda DECVAL
    sta TEMP16
    lda DECVAL+1
    sta TEMP16+1
    asl DECVAL
    rol DECVAL+1
    asl DECVAL
    rol DECVAL+1
    clc
    lda DECVAL
    adc TEMP16
    sta DECVAL
    lda DECVAL+1
    adc TEMP16+1
    sta DECVAL+1
    rts

; ==========================================================================
; PRDEC -- prints DECVAL (2 bytes) in decimal, no leading zeros (prints a
; single "0" for the value zero). Destroys DECVAL. Standard 6502
; repeated-subtraction-against-powers-of-ten shape, decimal analogue of
; this file's own PRBYTE/PRHEX (wozmon.s) for hex.
; ==========================================================================
PRDEC:
    ldx #0
    stz PRDEC_ANY
pd_digit:
    stz PRDEC_DIG
pd_sub:
    lda DECVAL
    sec
    sbc POW10_LO,x
    tay
    lda DECVAL+1
    sbc POW10_HI,x
    bcc pd_donedigit
    sta DECVAL+1
    sty DECVAL
    inc PRDEC_DIG
    bra pd_sub
pd_donedigit:
    lda PRDEC_DIG
    bne pd_mustprint
    lda PRDEC_ANY
    bne pd_mustprint
    cpx #4
    bne pd_next
    lda #'0'
    jsr CHROUT
    bra pd_next
pd_mustprint:
    lda #1
    sta PRDEC_ANY
    lda PRDEC_DIG
    ora #'0'
    jsr CHROUT
pd_next:
    inx
    cpx #5
    bne pd_digit
    rts

POW10_LO: .byte <10000, <1000, <100, <10, <1
POW10_HI: .byte >10000, >1000, >100, >10, >1

; ==========================================================================
; READLINE_ECHO -- reads one line of typed input into LINE_BUF (bounded by
; LINE_BUF_SIZE), echoing each character and handling backspace, until CR
; (consumed, not stored). Sets LINELEN. Shared by the shell's main loop
; and EDIT's retype step.
; ==========================================================================
READLINE_ECHO:
    ldy #0
rl_charloop:
    jsr READCHAR
    bcc rl_charloop
    ; Two distinct bytes mean "erase the previous character": $08 (BS),
    ; a real ASR-33/Teletype-style keyboard's own Backspace key, and $7F
    ; (DEL) -- what the Backspace key labeled on a real keyboard actually
    ; sends through xterm.js (this board's own browser terminal, app.js),
    ; a well-established terminal convention (VT100 and descendants) that
    ; has nothing to do with real Apple-1-era hardware. Both are handled
    ; identically once classified (rl_erase, below) except for one thing:
    ; READCHAR already echoed whichever raw byte was read, and only $08
    ; moves a real terminal's cursor on its own -- a bare $7F in the
    ; *output* stream is the classic teletype "rubout, ignore me" no-op
    ; byte, so the DEL path has to supply that missing leading BS itself
    ; before erasing, or the erase would land one column too far right.
    cmp #$7F
    beq rl_del
    cmp #$08
    bne rl_notbs
    cpy #0
    beq rl_charloop        ; nothing typed yet -- no-op
    dey
    jsr rl_erase
    jmp rl_charloop
rl_del:
    cpy #0
    beq rl_charloop        ; nothing typed yet -- no-op
    dey
    lda #$08
    jsr CHROUT
    jsr rl_erase
    jmp rl_charloop
rl_notbs:
    cmp #CR
    beq rl_gotcr
    cpy #LINE_BUF_SIZE-1
    bcs rl_charloop
    sta LINE_BUF,y
    iny
    jmp rl_charloop
rl_gotcr:
    ; READCHAR already echoed the bare CR -- add the LF a real terminal
    ; needs to actually advance a row instead of returning to column 0 in
    ; place (same reasoning as load.s's dload_gotcr). Unconditional: a
    ; blank line (just pressing Enter) needs its own fresh row exactly
    ; like a typed one, same as any command-prompt convention -- there
    ; used to be a special no-LF case here for Y=0 that left a bare Enter
    ; silently overwriting the current line instead of advancing.
    lda #LF
    jsr CHROUT
    sty LINELEN
    rts

; rl_erase -- the destructive half shared by both backspace bytes above:
; overwrite the just-vacated column with a space, then back the cursor up
; over the space too, so the erased character is actually gone rather
; than just stepped over (real teletype-era hardware with no addressable
; erase couldn't do this -- wozmon.s's own BACKSPACE is genuinely
; non-destructive, period-accurate there for the real Apple 1 monitor it
; recreates -- but this shell is original software for this board, not a
; recreation of a specific historical monitor, so it's free to choose
; real destructive behavior instead).
rl_erase:
    lda #' '
    jsr CHROUT
    lda #$08
    jsr CHROUT
    rts

; ==========================================================================
; PARSE_LINE -- parses LINE_BUF/LINELEN (already comment-stripped) into a
; leading label definition (DEFOFF/DEFLEN), a mnemonic (MNEMOFF -> TABPTR),
; and a resolved addressing mode/size/opcode. ASIZE=0 signals a blank or
; label-only line (nothing to assemble). Sets ERRFLAG on any bad syntax.
; ==========================================================================
PARSE_LINE:
    ; CURSOR is the caller's responsibility, not reset here -- CHECK_SYNTAX
    ; (below) needs to start mid-buffer, at RESTOFF, not always at 0 the way
    ; ASM_PASS1/2's own calls (via ASM_READLINE's always-offset-0 LINE_BUF)
    ; do -- see those two call sites' own `stz CURSOR`.
    stz DEFLEN
    stz ASIZE
    jsr SKIP_SPACES
    lda CURSOR
    cmp LINELEN
    bcc @notblank
    rts
@notblank:
    lda CURSOR
    sta SAVEDCUR
    ldy CURSOR
    lda LINE_BUF,y
    jsr IS_ALPHA
    bcc @skiplabel
@scanlabel:
    ldy CURSOR
    cpy LINELEN
    bcs @notlabel
    lda LINE_BUF,y
    cmp #':'
    beq @foundcolon
    jsr IS_ALNUM
    bcc @notlabel
    inc CURSOR
    jmp @scanlabel
@foundcolon:
    lda SAVEDCUR
    sta DEFOFF
    lda CURSOR
    sec
    sbc SAVEDCUR
    sta DEFLEN
    inc CURSOR
    jsr SKIP_SPACES
    lda CURSOR
    cmp LINELEN
    bcc @skiplabel
    rts                    ; label-definition-only line
@notlabel:
    lda SAVEDCUR
    sta CURSOR
@skiplabel:
    lda CURSOR
    sta MNEMOFF
    lda LINELEN
    sec
    sbc CURSOR
    cmp #3
    bcs @have3
    lda #1
    sta ERRFLAG
    rts
@have3:
    lda CURSOR
    clc
    adc #3
    sta CURSOR
    jsr MNEM_LOOKUP
    lda ERRFLAG
    bne @ret
    jsr SKIP_SPACES
    jsr PARSE_OPERAND
    sta AMODE            ; stash PARSE_OPERAND's natural-mode return (A)
                          ; before the ERRFLAG check below clobbers it
    lda ERRFLAG
    bne @ret
    lda AMODE
    jsr RESOLVE_MODE
    lda ERRFLAG
    bne @ret
    jsr RESOLVE_SIZE
@ret:
    rts

SKIP_SPACES:
@loop:
    lda CURSOR
    cmp LINELEN
    bcs @rts
    ldy CURSOR
    lda LINE_BUF,y
    cmp #' '
    bne @rts
    inc CURSOR
    jmp @loop
@rts:
    rts

; A = char. Carry SET if a letter, '_', or '.'; carry CLEAR otherwise.
IS_ALPHA:
    cmp #'A'
    bcc @no
    cmp #'Z'+1
    bcc @yes
    cmp #'_'
    beq @yes
    cmp #'.'
    beq @yes
@no:
    clc
    rts
@yes:
    sec
    rts

; A = char. Carry SET if a letter, digit, '_', or '.'.
IS_ALNUM:
    cmp #'0'
    bcc @tryalpha
    cmp #'9'+1
    bcc @yes
@tryalpha:
    jmp IS_ALPHA
@yes:
    sec
    rts

; ==========================================================================
; Operand parsing -- sets the "natural" addressing mode (A, on return) plus
; OPVAL / HASLABEL / LABOFF / LABLEN. CURSOR must already be positioned at
; the operand (or end-of-line, for no operand).
; ==========================================================================
PARSE_OPERAND:
    stz HASLABEL
    lda CURSOR
    cmp LINELEN
    bcc @haveoperand
    lda #AM_IMP            ; nothing left -- implied/accumulator/none
    rts
@haveoperand:
    ldy CURSOR
    lda LINE_BUF,y
    cmp #'#'
    bne @notimm
    inc CURSOR
    jsr EXPECT_DOLLAR_HEX
    lda #AM_IMM
    rts
@notimm:
    cmp #'$'
    bne @label
    inc CURSOR
    jsr HEX_PARSE
    cpx #2
    beq @zpdigits
    cpx #4
    beq @absdigits
    lda #1
    sta ERRFLAG
    rts
@zpdigits:
    lda #AM_ZP
    jmp @checkidx
@absdigits:
    lda #AM_ABS
@checkidx:
    pha
    jsr CHECK_INDEX_SUFFIX
    pla
    ldx IDXFLAG
    beq @noidx
    cpx #1
    bne @yidx
    clc
    adc #1                  ; AM_ZP->AM_ZPX (2->3) or AM_ABS->AM_ABSX (5->6)
    rts
@yidx:
    clc
    adc #2                  ; AM_ZP->AM_ZPY (2->4) or AM_ABS->AM_ABSY (5->7)
    rts
@noidx:
    rts
@label:
    jsr IS_ALPHA
    bcc @badoperand
    sty LABOFF
    stz LABLEN
@labloop:
    ldy CURSOR
    cpy LINELEN
    bcs @labdone
    lda LINE_BUF,y
    jsr IS_ALNUM
    bcc @labdone
    inc CURSOR
    inc LABLEN
    jmp @labloop
@labdone:
    lda #1
    sta HASLABEL
    jsr CHECK_INDEX_SUFFIX
    ldx IDXFLAG
    beq @lnone
    cpx #1
    bne @lyidx
    lda #AM_ABSX
    rts
@lyidx:
    lda #AM_ABSY
    rts
@lnone:
    lda #AM_ABS             ; labels always assemble absolute -- see header
    rts
@badoperand:
    lda #1
    sta ERRFLAG
    lda #AM_IMP
    rts

; Requires '$' at CURSOR, then 1-2 hex digits (immediate is one byte).
; Advances CURSOR, sets OPVAL. Sets ERRFLAG if malformed.
EXPECT_DOLLAR_HEX:
    ldy CURSOR
    cpy LINELEN
    bcs @bad
    lda LINE_BUF,y
    cmp #'$'
    bne @bad
    inc CURSOR
    jsr HEX_PARSE
    cpx #0
    beq @bad
    rts
@bad:
    lda #1
    sta ERRFLAG
    rts

; Checks for a trailing ",X" / ",Y" at CURSOR; advances CURSOR past it if
; present. Sets IDXFLAG: 0 none, 1 X, 2 Y.
CHECK_INDEX_SUFFIX:
    stz IDXFLAG
    ldy CURSOR
    cpy LINELEN
    bcs @none
    lda LINE_BUF,y
    cmp #','
    bne @none
    iny
    cpy LINELEN
    bcs @bad
    lda LINE_BUF,y
    cmp #'X'
    beq @isx
    cmp #'Y'
    beq @isy
@bad:
    lda #1
    sta ERRFLAG
    rts
@isx:
    lda #1
    sta IDXFLAG
    bra @adv
@isy:
    lda #2
    sta IDXFLAG
@adv:
    iny
    sty CURSOR
@none:
    rts

; Parses up to 4 hex digits starting at LINE_BUF+CURSOR, advances CURSOR
; past them, sets OPVAL. X (returned) = digit count consumed (0 = none).
HEX_PARSE:
    stz OPVAL
    stz OPVAL+1
    ldx #0
@loop:
    cpx #4
    beq @done
    ldy CURSOR
    cpy LINELEN
    bcs @done
    lda LINE_BUF,y
    jsr HEX_DIGIT_VAL
    bcc @done
    pha
    asl OPVAL
    rol OPVAL+1
    asl OPVAL
    rol OPVAL+1
    asl OPVAL
    rol OPVAL+1
    asl OPVAL
    rol OPVAL+1
    pla
    ora OPVAL
    sta OPVAL
    inc CURSOR
    inx
    jmp @loop
@done:
    rts

; A = ascii char. Carry SET + value 0-15 in A if a valid uppercase hex
; digit; carry CLEAR (A unchanged) otherwise.
HEX_DIGIT_VAL:
    cmp #'0'
    bcc @no
    cmp #'9'+1
    bcs @tryaf
    sec
    sbc #'0'
    rts
@tryaf:
    cmp #'A'
    bcc @no
    cmp #'F'+1
    bcs @no
    sec
    sbc #'A'-10
    rts
@no:
    clc
    rts

; ==========================================================================
; MNEM_LOOKUP -- finds the 3-char mnemonic at LINE_BUF+MNEMOFF in OPTAB.
; Sets TABPTR to the matching row, or ERRFLAG if not found.
; ==========================================================================
MNEM_LOOKUP:
    lda #<OPTAB
    sta TABPTR
    lda #>OPTAB
    sta TABPTR+1
@scan:
    ldy #0
    lda (TABPTR),y
    bne @haverow
    lda #1
    sta ERRFLAG
    rts
@haverow:
    ldx MNEMOFF
    lda LINE_BUF,x
    cmp (TABPTR),y
    bne @nomatch
    inx
    iny
    lda LINE_BUF,x
    cmp (TABPTR),y
    bne @nomatch
    inx
    iny
    lda LINE_BUF,x
    cmp (TABPTR),y
    beq @match
@nomatch:
    clc
    lda TABPTR
    adc #12
    sta TABPTR
    bcc @scan
    inc TABPTR+1
    jmp @scan
@match:
    rts

; ==========================================================================
; RESOLVE_MODE -- given the natural mode in A and TABPTR, applies the
; branch override (any mnemonic that supports AM_REL always uses it, if an
; operand was given) and the zp->abs / zpx->absx promotion (a mnemonic
; with no zp/zpx form still accepts an $xx-sized literal, widened to
; absolute). Sets AMODE/OPCODE, or ERRFLAG if truly unsupported.
; ==========================================================================
RESOLVE_MODE:
    sta AMODE
    ldy #3+AM_REL
    lda (TABPTR),y
    cmp #AM_UNSUPPORTED
    beq @notbranch
    lda AMODE
    bne @forcerel
    lda #1
    sta ERRFLAG
    rts
@forcerel:
    lda #AM_REL
    sta AMODE
    jmp @lookup
@notbranch:
    lda AMODE
    clc
    adc #3
    tay
    lda (TABPTR),y
    cmp #AM_UNSUPPORTED
    bne @haveop
    lda AMODE
    cmp #AM_ZP
    bne @tryzpx
    ldy #3+AM_ABS
    lda (TABPTR),y
    cmp #AM_UNSUPPORTED
    beq @unsupported
    lda #AM_ABS
    sta AMODE
    jmp @lookup
@tryzpx:
    cmp #AM_ZPX
    bne @unsupported
    ldy #3+AM_ABSX
    lda (TABPTR),y
    cmp #AM_UNSUPPORTED
    beq @unsupported
    lda #AM_ABSX
    sta AMODE
    jmp @lookup
@unsupported:
    lda #1
    sta ERRFLAG
    rts
@haveop:
    sta OPCODE
    rts
@lookup:
    lda AMODE
    clc
    adc #3
    tay
    lda (TABPTR),y
    sta OPCODE
    rts

; ASIZE from AMODE: imp=1, imm/zp/zpx/zpy/rel=2, abs/absx/absy=3.
RESOLVE_SIZE:
    lda AMODE
    beq @sz1
    cmp #AM_REL
    beq @sz2
    cmp #AM_ABS
    bcs @sz3
@sz2:
    lda #2
    sta ASIZE
    rts
@sz3:
    lda #3
    sta ASIZE
    rts
@sz1:
    lda #1
    sta ASIZE
    rts

; ==========================================================================
; SYM_DEFINE -- appends LINE_BUF[DEFOFF..DEFOFF+DEFLEN) = ASMPC to the
; symbol table (first free slot; name truncated/space-padded to 6 chars).
; ERRFLAG if the table is full.
; ==========================================================================
SYM_DEFINE:
    lda #<SYM_START
    sta SYMPTR
    lda #>SYM_START
    sta SYMPTR+1
@scan:
    lda SYMPTR+1
    cmp #>SYM_END
    bne @checkfree
    lda SYMPTR
    cmp #<SYM_END
    bne @checkfree
    lda #1
    sta ERRFLAG
    rts
@checkfree:
    ldy #0
    lda (SYMPTR),y
    beq @found
    clc
    lda SYMPTR
    adc #SYM_ENTRY_LEN
    sta SYMPTR
    bcc @scan
    inc SYMPTR+1
    jmp @scan
@found:
    lda DEFOFF
    sta COPYIDX
    ldy #0
@copyloop:
    cpy #SYM_NAME_LEN
    bcs @storeaddr
    tya
    cmp DEFLEN
    bcs @pad
    ldx COPYIDX
    lda LINE_BUF,x
    sta (SYMPTR),y
    inc COPYIDX
    iny
    jmp @copyloop
@pad:
    lda #' '
    sta (SYMPTR),y
    iny
    cpy #SYM_NAME_LEN
    bcc @pad
@storeaddr:
    lda ASMPC
    sta (SYMPTR),y
    iny
    lda ASMPC+1
    sta (SYMPTR),y
    rts

; ==========================================================================
; SYM_LOOKUP -- finds LINE_BUF[LABOFF..LABOFF+LABLEN) (truncated/padded to
; 6, same convention as SYM_DEFINE) in the symbol table. Sets OPVAL, or
; ERRFLAG if not defined.
; ==========================================================================
SYM_LOOKUP:
    lda #<LINE_BUF
    clc
    adc LABOFF
    sta LABPTR
    lda #>LINE_BUF
    adc #0
    sta LABPTR+1
    lda #<SYM_START
    sta SYMPTR
    lda #>SYM_START
    sta SYMPTR+1
@scan:
    ldy #0
    lda (SYMPTR),y
    bne @check
    lda #1
    sta ERRFLAG
    rts
@check:
    ldy #0
@cmploop:
    cpy #SYM_NAME_LEN
    beq @matched
    tya
    cmp LABLEN
    bcs @wantspace
    lda (LABPTR),y
    jmp @havechar
@wantspace:
    lda #' '
@havechar:
    cmp (SYMPTR),y
    bne @nextentry
    iny
    jmp @cmploop
@matched:
    lda (SYMPTR),y
    sta OPVAL
    iny
    lda (SYMPTR),y
    sta OPVAL+1
    rts
@nextentry:
    clc
    lda SYMPTR
    adc #SYM_ENTRY_LEN
    sta SYMPTR
    bcc @checkend
    inc SYMPTR+1
@checkend:
    lda SYMPTR+1
    cmp #>SYM_END
    bne @scan
    lda SYMPTR
    cmp #<SYM_END
    bne @scan
    lda #1
    sta ERRFLAG
    rts

; ==========================================================================
; EMIT_INSTRUCTION -- writes OPCODE (and its operand bytes, per AMODE) at
; (ASMPC). Resolves a label operand via SYM_LOOKUP first if HASLABEL. For
; AM_REL, computes and range-checks the signed branch offset.
; ==========================================================================
EMIT_INSTRUCTION:
    ldy #0
    lda OPCODE
    sta (ASMPC),y
    lda AMODE
    beq @done
    cmp #AM_REL
    beq @rel
    lda HASLABEL
    beq @havevalue
    jsr SYM_LOOKUP
    lda ERRFLAG
    bne @ret
@havevalue:
    lda AMODE
    cmp #AM_ABS
    bcs @twobyte
    ldy #1
    lda OPVAL
    sta (ASMPC),y
    jmp @done
@twobyte:
    ldy #1
    lda OPVAL
    sta (ASMPC),y
    ldy #2
    lda OPVAL+1
    sta (ASMPC),y
    jmp @done
@rel:
    lda HASLABEL
    beq @relhavevalue
    jsr SYM_LOOKUP
    lda ERRFLAG
    bne @ret
@relhavevalue:
    lda ASMPC
    clc
    adc #2
    sta TEMP16
    lda ASMPC+1
    adc #0
    sta TEMP16+1
    lda OPVAL
    sec
    sbc TEMP16
    sta TEMP16
    lda OPVAL+1
    sbc TEMP16+1
    beq @rangeok_pos
    cmp #$FF
    bne @outofrange
    lda TEMP16
    bmi @rangeok
    bra @outofrange
@rangeok_pos:
    lda TEMP16
    bpl @rangeok
@outofrange:
    lda #1
    sta ERRFLAG
    jmp @ret
@rangeok:
    ldy #1
    lda TEMP16
    sta (ASMPC),y
@done:
@ret:
    rts

; ==========================================================================
; OPTAB -- 3-byte mnemonic + 9 opcode bytes (one per AM_* mode above, in
; order; AM_UNSUPPORTED = $02 marks a mode this mnemonic doesn't support).
; Values transcribed from web/assembler/assembler.js's OPS table (itself
; transcribed from cpu65c02.cpp's decoder -- see that file's WDC datasheet
; citation), restricted to v1's addressing-mode scope: no indirect forms,
; no 65C02 bit-ops (RMBx/SMBx/BBRx/BBSx) -- see this file's header.
; Table ends with a $00 mnemonic byte (safe: no real mnemonic starts NUL,
; and BRK's own opcode $00 lives in an *opcode* slot, never the 3-byte
; mnemonic prefix a lookup actually tests).
; order:                     imp   imm   zp    zpx   zpy   abs   absx  absy  rel
OPTAB:
    .byte "LDA", $02,  $A9,  $A5,  $B5,  $02,  $AD,  $BD,  $B9,  $02
    .byte "LDX", $02,  $A2,  $A6,  $02,  $B6,  $AE,  $02,  $BE,  $02
    .byte "LDY", $02,  $A0,  $A4,  $B4,  $02,  $AC,  $BC,  $02,  $02
    .byte "STA", $02,  $02,  $85,  $95,  $02,  $8D,  $9D,  $99,  $02
    .byte "STX", $02,  $02,  $86,  $02,  $96,  $8E,  $02,  $02,  $02
    .byte "STY", $02,  $02,  $84,  $94,  $02,  $8C,  $02,  $02,  $02
    .byte "STZ", $02,  $02,  $64,  $74,  $02,  $9C,  $9E,  $02,  $02
    .byte "TAX", $AA,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "TAY", $A8,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "TXA", $8A,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "TYA", $98,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "TXS", $9A,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "TSX", $BA,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PHA", $48,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PLA", $68,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PHP", $08,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PLP", $28,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PHX", $DA,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PLX", $FA,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PHY", $5A,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "PLY", $7A,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "ORA", $02,  $09,  $05,  $15,  $02,  $0D,  $1D,  $19,  $02
    .byte "AND", $02,  $29,  $25,  $35,  $02,  $2D,  $3D,  $39,  $02
    .byte "EOR", $02,  $49,  $45,  $55,  $02,  $4D,  $5D,  $59,  $02
    .byte "ADC", $02,  $69,  $65,  $75,  $02,  $6D,  $7D,  $79,  $02
    .byte "SBC", $02,  $E9,  $E5,  $F5,  $02,  $ED,  $FD,  $F9,  $02
    .byte "CMP", $02,  $C9,  $C5,  $D5,  $02,  $CD,  $DD,  $D9,  $02
    .byte "CPX", $02,  $E0,  $E4,  $02,  $02,  $EC,  $02,  $02,  $02
    .byte "CPY", $02,  $C0,  $C4,  $02,  $02,  $CC,  $02,  $02,  $02
    .byte "INC", $1A,  $02,  $E6,  $F6,  $02,  $EE,  $FE,  $02,  $02
    .byte "DEC", $3A,  $02,  $C6,  $D6,  $02,  $CE,  $DE,  $02,  $02
    .byte "INX", $E8,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "INY", $C8,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "DEX", $CA,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "DEY", $88,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "ASL", $0A,  $02,  $06,  $16,  $02,  $0E,  $1E,  $02,  $02
    .byte "LSR", $4A,  $02,  $46,  $56,  $02,  $4E,  $5E,  $02,  $02
    .byte "ROL", $2A,  $02,  $26,  $36,  $02,  $2E,  $3E,  $02,  $02
    .byte "ROR", $6A,  $02,  $66,  $76,  $02,  $6E,  $7E,  $02,  $02
    .byte "BIT", $02,  $89,  $24,  $34,  $02,  $2C,  $3C,  $02,  $02
    .byte "TRB", $02,  $02,  $14,  $02,  $02,  $1C,  $02,  $02,  $02
    .byte "TSB", $02,  $02,  $04,  $02,  $02,  $0C,  $02,  $02,  $02
    .byte "JMP", $02,  $02,  $02,  $02,  $02,  $4C,  $02,  $02,  $02
    .byte "JSR", $02,  $02,  $02,  $02,  $02,  $20,  $02,  $02,  $02
    .byte "RTS", $60,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "RTI", $40,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "BRK", $00,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "CLC", $18,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "SEC", $38,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "CLI", $58,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "SEI", $78,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "CLV", $B8,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "CLD", $D8,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "SED", $F8,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "NOP", $EA,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "WAI", $CB,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "STP", $DB,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02
    .byte "BPL", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $10
    .byte "BMI", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $30
    .byte "BVC", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $50
    .byte "BVS", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $70
    .byte "BCC", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $90
    .byte "BCS", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $B0
    .byte "BNE", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $D0
    .byte "BEQ", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $F0
    .byte "BRA", $02,  $02,  $02,  $02,  $02,  $02,  $02,  $02,  $80
    .byte $00, $00, $00   ; end marker
