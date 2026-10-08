; EGA video BIOS stand-in, built from the IBM EGA Technical Reference (1984) interface and tables.

        cpu     286
        bits    16
        org     0

ROM_SIZE        equ     16384
ROM_SEG         equ     0xC000

BDA             equ     0x0040
EQUIP           equ     0x10
CRT_MODE        equ     0x49
CRT_COLS        equ     0x4A
CRT_LEN         equ     0x4C
CRT_START       equ     0x4E
CURSOR_POSN     equ     0x50
CURSOR_MODE     equ     0x60
ACTIVE_PAGE     equ     0x62
ADDR_6845       equ     0x63
CRT_MODE_SET    equ     0x65
CRT_PALETTE     equ     0x66
ROWS            equ     0x84
POINTS          equ     0x85
INFO            equ     0x87
INFO_3          equ     0x88
SAVE_PTR        equ     0xA8
PRTSC_STATUS    equ     0x100

; VIDEO_PARMS entry
P_COLS          equ     0
P_ROWS          equ     1
P_POINTS        equ     2
P_LEN           equ     3
P_SEQ           equ     5
P_MISC          equ     9
P_CRTC          equ     10
P_ATTR          equ     35
P_GFX           equ     55
P_SIZE          equ     64

; INT 10h frame after pusha, push ds, push es
F_ES            equ     0
F_DS            equ     2
F_DI            equ     4
F_SI            equ     6
F_BP            equ     8
F_BX            equ     12
F_DX            equ     14
F_CX            equ     16
F_AX            equ     18

; graphics locals
G_KIND          equ     -2              ; 1 CGA-compatible, 2 planar
G_BPC           equ     -4
G_PITCH         equ     -6
G_BASE          equ     -8
G_H             equ     -10
G_COUNT         equ     -12
G_COLOR         equ     -14
G_I             equ     -16
G_Y             equ     -18
G_X             equ     -20
G_GLYPH         equ     -36
G_SIZE          equ     36

; scroll locals
S_N             equ     -2
S_H             equ     -4
S_W             equ     -6
S_X             equ     -8
S_TOP           equ     -10
S_SPR           equ     -12
S_FILL          equ     -14
S_DIR           equ     -16
S_KIND          equ     -18
S_BASE          equ     -20
S_PITCH         equ     -22
S_R             equ     -24
S_I             equ     -26
S_DST           equ     -28
S_SRC           equ     -30
S_SIZE          equ     30

        db      0x55, 0xAA, ROM_SIZE / 512
        jmp     init

banner  db      "EGA BIOS: open stand-in for IBM's ROM", 13, 10, 0

; --- power-on ------------------------------------------------------------------

init:
        pusha
        push    ds
        push    es
        cld
        call    read_switches
        mov     bx, BDA
        mov     ds, bx
        mov     [INFO_3], al
        mov     byte [INFO], 0x60       ; 256KB, EGA active, cursor emulation on
        mov     ah, al
        call    monitor_type
        cmp     al, 2
        jne     .color
        or      byte [INFO], 0x02
.color:
        mov     word [SAVE_PTR], save_tbl
        mov     word [SAVE_PTR + 2], ROM_SEG
        xor     bx, bx
        mov     es, bx
        cli
        mov     bx, [es:0x10 * 4]
        mov     [es:0x42 * 4], bx
        mov     bx, [es:0x10 * 4 + 2]
        mov     [es:0x42 * 4 + 2], bx
        mov     word [es:0x10 * 4], int10
        mov     [es:0x10 * 4 + 2], cs
        mov     word [es:0x1F * 4], font8 + 128 * 8
        mov     [es:0x1F * 4 + 2], cs
        mov     word [es:0x43 * 4], font8
        mov     [es:0x43 * 4 + 2], cs
        sti
        mov     ah, [INFO_3]
        and     ah, 0x0F
        cmp     ah, 6                   ; 0-5: another adapter is primary
        jb      .done
        ; equipment bits 4-5: 01 40x25 color, 10 80x25 color, 11 monochrome
        mov     bl, 0x30
        mov     bh, 7
        cmp     al, 2
        je      .equip
        mov     bl, 0x20
        mov     bh, 3
        cmp     ah, 6
        jne     .equip
        mov     bl, 0x10
        mov     bh, 1
.equip:
        and     byte [EQUIP], 0xCF
        or      [EQUIP], bl
        mov     al, bh
        call    set_mode
        push    cs
        pop     ds
        mov     si, banner
.print:
        lodsb
        test    al, al
        jz      .done
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     .print
.done:
        pop     es
        pop     ds
        popa
        retf

; AL bits 0-3 = switches 1-4, 1 = open. Clock select n senses switch 4-n.
read_switches:
        push    bx
        push    cx
        push    dx
        mov     dx, 0x3C2
        xor     bl, bl
        mov     cx, 4
        mov     ah, 0x01
.next:
        mov     al, ah
        out     dx, al
        in      al, dx
        shl     al, 4                   ; bit 4 into CF
        rcl     bl, 1
        add     ah, 4
        loop    .next
        mov     al, bl
        pop     dx
        pop     cx
        pop     bx
        ret

; AH = switch nibble. AL: 0 color display, 1 enhanced display high resolution, 2 monochrome.
monitor_type:
        mov     al, 2
        cmp     ah, 0x0A
        jae     .ret
        cmp     ah, 0x04
        je      .ret
        cmp     ah, 0x05
        je      .ret
        mov     al, 1
        cmp     ah, 0x09
        je      .ret
        cmp     ah, 0x03
        je      .ret
        xor     al, al
.ret:
        ret

; CF set when the switches select the enhanced display in high resolution (3 or 9).
is_ecd:
        push    ax
        mov     al, [INFO_3]
        and     al, 0x0F
        cmp     al, 0x03
        je      .yes
        cmp     al, 0x09
        je      .yes
        pop     ax
        clc
        ret
.yes:
        pop     ax
        stc
        ret

; --- INT 10h -------------------------------------------------------------------

int10:
        sti
        cld
        pusha
        push    ds
        push    es
        mov     bp, sp
        mov     si, BDA
        mov     ds, si
        cmp     ah, (functions_end - functions) / 2
        jae     .exit
        mov     si, ax
        shr     si, 8
        shl     si, 1
        call    [cs:functions + si]
.exit:
        pop     es
        pop     ds
        popa
        iret

functions:
        dw      set_mode, set_ctype, set_cpos, read_cursor, read_lpen, act_disp_page
        dw      scroll_up, scroll_down, read_ac, write_ac, write_c, set_color
        dw      write_dot, read_dot, write_tty, video_state, set_palette, char_gen
        dw      alt_select, write_string
functions_end:

read_lpen:
        mov     byte [bp + F_AX + 1], 0
        ret

video_state:
        mov     al, [INFO]
        and     al, 0x80
        or      al, [CRT_MODE]
        mov     ah, [CRT_COLS]
        mov     [bp + F_AX], ax
        mov     al, [ACTIVE_PAGE]
        mov     [bp + F_BX + 1], al
        ret

; --- mode set ------------------------------------------------------------------

; AL = mode, bit 7 keeps the regen buffer.
set_mode:
        pusha
        push    ds
        push    es
        mov     bx, BDA
        mov     ds, bx
        mov     ah, al
        and     al, 0x7F
        cmp     al, 0x10
        ja      .out
        cmp     al, 0x08
        jb      .ok
        cmp     al, 0x0D
        jb      .out                    ; 8-0Ch are reserved
.ok:
        and     ah, 0x80
        and     byte [INFO], 0x7F
        or      [INFO], ah
        mov     [CRT_MODE], al
        call    parm_ptr
        call    program_regs
        mov     al, [es:si + P_COLS]
        xor     ah, ah
        mov     [CRT_COLS], ax
        mov     al, [es:si + P_ROWS]
        mov     [ROWS], al
        mov     al, [es:si + P_POINTS]
        mov     [POINTS], ax
        mov     ax, [es:si + P_LEN]
        mov     [CRT_LEN], ax
        mov     cx, [es:si + P_CRTC + 0x0A]
        xchg    cl, ch
        mov     [CURSOR_MODE], cx
        xor     ax, ax
        mov     [CRT_START], ax
        mov     [ACTIVE_PAGE], al
        mov     di, CURSOR_POSN
        mov     cx, 8
.cur:
        mov     [di], ax
        add     di, 2
        loop    .cur
        mov     bl, [CRT_MODE]
        xor     bh, bh
        mov     al, 0x29
        cmp     bl, 7
        ja      .set65
        mov     al, [cs:mode_set_65 + bx]
.set65:
        mov     [CRT_MODE_SET], al
        mov     al, 0x30
        cmp     bl, 6
        jne     .pal
        mov     al, 0x3F
.pal:
        mov     [CRT_PALETTE], al
        test    byte [INFO], 0x80
        jnz     .noclear
        call    clear_regen
.noclear:
        call    is_text
        jnc     .graphics
        call    load_mode_font
        jmp     .cursor
.graphics:
        xor     ax, ax
        mov     es, ax
        mov     ax, font8
        cmp     byte [POINTS], 14
        jne     .f43
        mov     ax, font14
.f43:
        cli
        mov     [es:0x43 * 4], ax
        mov     [es:0x43 * 4 + 2], cs
        sti
.cursor:
        mov     cx, [CURSOR_MODE]
        call    set_ctype
.out:
        pop     es
        pop     ds
        popa
        ret

mode_set_65     db      0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29

; ES:SI = current mode's VIDEO_PARMS entry, found through SAVE_PTR.
parm_ptr:
        push    ax
        push    bx
        push    dx
        mov     bl, [CRT_MODE]
        xor     bh, bh
        cmp     bl, 3
        ja      .big
        call    is_ecd
        jnc     .index
        add     bl, 0x13                ; 350-line text on the enhanced display
        jmp     .index
.big:
        cmp     bl, 0x0F
        jb      .index
        add     bl, 2                   ; more than 64KB: the second F and 10 entries
.index:
        les     si, [SAVE_PTR]
        les     si, [es:si]
        mov     ax, P_SIZE
        mul     bx
        add     si, ax
        pop     dx
        pop     bx
        pop     ax
        ret

; Loads sequencer, misc output, CRTC, attribute and graphics registers from ES:SI.
program_regs:
        push    si
        mov     dx, 0x3C4
        mov     ax, 0x0100              ; synchronous reset
        out     dx, ax
        mov     cx, 4
        mov     bx, P_SEQ
        mov     ah, 1
.seq:
        mov     al, ah
        out     dx, al
        inc     dx
        mov     al, [es:si + bx]
        out     dx, al
        dec     dx
        inc     bx
        inc     ah
        loop    .seq
        mov     al, [es:si + P_MISC]
        mov     dx, 0x3C2
        out     dx, al
        mov     dx, 0x3C4
        mov     ax, 0x0300
        out     dx, ax
        mov     dx, 0x3D4
        test    byte [es:si + P_MISC], 1
        jnz     .crtc_base
        mov     dl, 0xB4
.crtc_base:
        mov     [ADDR_6845], dx
        xor     ah, ah
        mov     bx, P_CRTC
        mov     cx, 25
.crtc:
        mov     al, ah
        out     dx, al
        inc     dx
        mov     al, [es:si + bx]
        out     dx, al
        dec     dx
        inc     bx
        inc     ah
        loop    .crtc
        add     dx, 6
        in      al, dx                  ; attribute flip-flop to index
        mov     dx, 0x3C0
        xor     ah, ah
        mov     bx, P_ATTR
        mov     cx, 20
.attr:
        mov     al, ah
        out     dx, al
        mov     al, [es:si + bx]
        out     dx, al
        inc     bx
        inc     ah
        loop    .attr
        mov     al, 0x20
        out     dx, al
        mov     dx, 0x3CC               ; Graphics 1 and 2 Position
        xor     al, al
        out     dx, al
        mov     dx, 0x3CA
        mov     al, 1
        out     dx, al
        mov     dx, 0x3CE
        xor     ah, ah
        mov     bx, P_GFX
        mov     cx, 9
.gfx:
        mov     al, ah
        out     dx, al
        inc     dx
        mov     al, [es:si + bx]
        out     dx, al
        dec     dx
        inc     bx
        inc     ah
        loop    .gfx
        pop     si
        ret

; CF set in an alphanumeric mode.
is_text:
        cmp     byte [CRT_MODE], 3
        jbe     .yes
        cmp     byte [CRT_MODE], 7
        je      .yes
        clc
        ret
.yes:
        stc
        ret

; ES = regen segment of the current mode.
regen_seg:
        push    ax
        mov     ax, 0xB800
        cmp     byte [CRT_MODE], 7
        jne     .color
        mov     ax, 0xB000
.color:
        cmp     byte [CRT_MODE], 0x0D
        jb      .set
        mov     ax, 0xA000
.set:
        mov     es, ax
        pop     ax
        ret

clear_regen:
        push    es
        call    regen_seg
        xor     di, di
        mov     cx, 0x4000
        xor     ax, ax
        call    is_text
        jnc     .gfx
        mov     ax, 0x0720
.gfx:
        cmp     byte [CRT_MODE], 0x0D
        jb      .fill
        mov     cx, 0x8000
.fill:
        rep     stosw
        pop     es
        ret

; --- fonts ---------------------------------------------------------------------

; Maps plane 2 at A000 for a font load.
font_access_on:
        push    ax
        push    dx
        mov     dx, 0x3C4
        mov     ax, 0x0100
        out     dx, ax
        mov     ax, 0x0402
        out     dx, ax
        mov     ax, 0x0604
        out     dx, ax
        mov     ax, 0x0300
        out     dx, ax
        mov     dx, 0x3CE
        mov     ax, 0x0204
        out     dx, ax
        mov     ax, 0x0005
        out     dx, ax
        mov     ax, 0x0406
        out     dx, ax
        pop     dx
        pop     ax
        ret

; Restores SR02, SR04 and GR04-06 from the mode's table. DS = BDA.
font_access_off:
        pusha
        push    es
        call    parm_ptr
        mov     dx, 0x3C4
        mov     ax, 0x0100
        out     dx, ax
        mov     al, 2
        mov     ah, [es:si + P_SEQ + 1]
        out     dx, ax
        mov     al, 4
        mov     ah, [es:si + P_SEQ + 3]
        out     dx, ax
        mov     ax, 0x0300
        out     dx, ax
        mov     dx, 0x3CE
        mov     bx, 4
.gfx:
        mov     al, bl
        mov     ah, [es:si + P_GFX + bx]
        out     dx, ax
        inc     bx
        cmp     bx, 7
        jb      .gfx
        pop     es
        popa
        ret

; DS:SI glyphs, CX count, DX first character, BL block, BH bytes per glyph.
load_font:
        pusha
        push    es
        call    font_access_on
        mov     ax, 0xA000
        mov     es, ax
        xor     ah, ah
        mov     al, bl
        and     al, 3
        shl     ax, 14                  ; 16KB a block
        mov     di, dx
        shl     di, 5
        add     di, ax
        jcxz    .done
.glyph:
        push    cx
        push    di
        mov     cl, bh
        xor     ch, ch
        rep     movsb
        mov     cl, 32
        sub     cl, bh
        xor     al, al
        rep     stosb
        pop     di
        pop     cx
        add     di, 32
        loop    .glyph
.done:
        push    ds
        mov     ax, BDA
        mov     ds, ax
        call    font_access_off
        pop     ds
        pop     es
        popa
        ret

; Loads the ROM font matching POINTS into block 0.
load_mode_font:
        pusha
        push    ds
        mov     bh, [POINTS]
        mov     si, font8
        cmp     bh, 8
        je      .load
        mov     bh, 14
        mov     si, font14
.load:
        push    cs
        pop     ds
        mov     cx, 256
        xor     dx, dx
        xor     bl, bl
        call    load_font
        pop     ds
        popa
        ret

; --- cursor --------------------------------------------------------------------

; CH start, CL end, adjusted by IBM's cursor emulation (EGA TR, AH=1 and CALC_CURSOR).
set_ctype:
        mov     [CURSOR_MODE], cx
        mov     al, ch
        and     al, 0x60
        cmp     al, 0x20
        jne     .emulate
        mov     cx, 0x1E00              ; off: start below any cell
        jmp     .out
.emulate:
        test    byte [INFO], 0x01
        jnz     .out
        cmp     byte [CRT_MODE], 3
        ja      .calc
        call    is_ecd
        jnc     .calc
        cmp     ch, 4                   ; CGA lines past 4 move to the bottom of a 14-line cell
        jbe     .end
        add     ch, 5
.end:
        cmp     cl, 4
        jbe     .calc
        add     cl, 5
.calc:
        inc     cl                      ; the EGA's end register is one past the last line
        test    ch, ch
        jz      .wrap_ok
        cmp     cl, [POINTS]
        jb      .wrap_ok
        xor     cl, cl
.wrap_ok:
        mov     al, cl
        sub     al, ch
        cmp     al, 0x10
        jne     .out
        inc     cl
.out:
        mov     dx, [ADDR_6845]
        mov     al, 0x0A
        mov     ah, ch
        out     dx, ax
        mov     al, 0x0B
        mov     ah, cl
        out     dx, ax
        ret

; DX = row/column for page BH.
set_cpos:
        push    bx
        mov     bl, bh
        and     bx, 7
        shl     bx, 1
        mov     [CURSOR_POSN + bx], dx
        shr     bx, 1
        cmp     bl, [ACTIVE_PAGE]
        jne     .done
        call    cursor_to_crtc
.done:
        pop     bx
        ret

; Programs CRTC 0E/0F from the active page's cursor.
cursor_to_crtc:
        pusha
        mov     bl, [ACTIVE_PAGE]
        xor     bh, bh
        shl     bx, 1
        mov     dx, [CURSOR_POSN + bx]
        mov     al, dh
        mul     byte [CRT_COLS]
        xor     dh, dh
        add     ax, dx
        mov     bx, [CRT_START]
        shr     bx, 1
        add     bx, ax
        mov     dx, [ADDR_6845]
        mov     al, 0x0E
        mov     ah, bh
        out     dx, ax
        mov     al, 0x0F
        mov     ah, bl
        out     dx, ax
        popa
        ret

read_cursor:
        call    cursor_of_page
        mov     [bp + F_DX], dx
        mov     ax, [CURSOR_MODE]
        mov     [bp + F_CX], ax
        ret

; DX = page BH's cursor.
cursor_of_page:
        push    bx
        mov     bl, bh
        and     bx, 7
        shl     bx, 1
        mov     dx, [CURSOR_POSN + bx]
        pop     bx
        ret

act_disp_page:
        and     al, 7
        mov     [ACTIVE_PAGE], al
        xor     ah, ah
        mul     word [CRT_LEN]
        mov     [CRT_START], ax
        mov     bx, ax
        call    parm_ptr
        test    byte [es:si + P_CRTC + 0x17], 0x40
        jnz     .bytes
        shr     bx, 1
.bytes:
        mov     dx, [ADDR_6845]
        mov     al, 0x0C
        mov     ah, bh
        out     dx, ax
        mov     al, 0x0D
        mov     ah, bl
        out     dx, ax
        call    cursor_to_crtc
        ret

; --- scrolling -----------------------------------------------------------------

scroll_up:
        xor     bl, bl
        jmp     scroll_core
scroll_down:
        mov     bl, 1

; AL lines (0 blanks the window), BH fill, BL 0 up 1 down, CH/CL top left, DH/DL bottom right.
scroll_core:
        enter   S_SIZE, 0
        pusha
        push    ds
        push    es
        cmp     ch, dh
        ja      .out
        cmp     cl, dl
        ja      .out
        mov     ah, [ROWS]
        cmp     dh, ah
        jbe     .rows_ok
        mov     dh, ah
.rows_ok:
        mov     ah, [CRT_COLS]
        dec     ah
        cmp     dl, ah
        jbe     .cols_ok
        mov     dl, ah
.cols_ok:
        xor     ah, ah
        mov     [bp + S_N], ax
        mov     al, bl
        mov     [bp + S_DIR], ax
        mov     al, bh
        mov     [bp + S_FILL], ax
        mov     al, ch
        mov     [bp + S_TOP], ax
        mov     al, dh
        sub     al, ch
        inc     al
        mov     [bp + S_H], ax
        mov     al, dl
        sub     al, cl
        inc     al
        mov     [bp + S_W], ax
        mov     al, cl
        mov     [bp + S_X], ax
        mov     ax, [bp + S_N]
        cmp     ax, [bp + S_H]
        jbe     .n_ok
        xor     ax, ax
.n_ok:
        test    ax, ax
        jnz     .n_set
        mov     ax, [bp + S_H]
.n_set:
        mov     [bp + S_N], ax
        mov     ax, [CRT_START]
        mov     [bp + S_BASE], ax
        mov     al, [CRT_COLS]
        xor     ah, ah
        mov     [bp + S_PITCH], ax
        mov     word [bp + S_KIND], 0
        mov     word [bp + S_SPR], 1
        mov     bx, 2                   ; bytes per character cell
        call    is_text
        jc      .text
        mov     word [bp + S_KIND], 1
        mov     word [bp + S_SPR], 8
        cmp     byte [CRT_MODE], 0x0D
        jae     .planar
        cmp     byte [CRT_MODE], 6
        jb      .geom
        mov     bx, 1
        jmp     .geom
.planar:
        mov     word [bp + S_KIND], 2
        mov     al, [POINTS]
        mov     [bp + S_SPR], ax
        mov     bx, 1
        call    parm_ptr
        mov     ah, [es:si + P_SEQ + 1]
        mov     [bp + S_SRC], ah        ; the table's map mask, restored at the end
        mov     dx, 0x3C4
        mov     ax, 0x0F02
        out     dx, ax
        jmp     .geom
.text:
        shl     word [bp + S_PITCH], 1
.geom:
        mov     ax, [bp + S_W]
        mul     bx
        mov     [bp + S_W], ax
        mov     ax, [bp + S_X]
        mul     bx
        mov     [bp + S_X], ax
        mov     al, [bp + S_SRC]
        mov     [bp + S_FILL + 1], al
        call    regen_seg
        push    es
        pop     ds
        mov     word [bp + S_R], 0
.row:
        mov     cx, [bp + S_R]
        cmp     cx, [bp + S_H]
        jae     .done
        mov     ax, cx
        cmp     word [bp + S_DIR], 0
        je      .dst
        mov     ax, [bp + S_H]
        dec     ax
        sub     ax, cx
.dst:
        add     ax, [bp + S_TOP]
        mov     [bp + S_DST], ax
        mov     dx, [bp + S_H]
        sub     dx, [bp + S_N]
        cmp     cx, dx
        jae     .blank
        mov     dx, [bp + S_N]
        cmp     word [bp + S_DIR], 0
        je      .src
        neg     dx
.src:
        add     ax, dx
        mov     [bp + S_SRC], ax
        call    scroll_copy_row
        jmp     .next
.blank:
        call    scroll_blank_row
.next:
        inc     word [bp + S_R]
        jmp     .row
.done:
        cmp     word [bp + S_KIND], 2
        jne     .out
        mov     dx, 0x3CE
        mov     ax, 0x0005
        out     dx, ax
        mov     ax, 0xFF08
        out     dx, ax
        mov     dx, 0x3C4
        mov     al, 2
        mov     ah, [bp + S_FILL + 1]
        out     dx, ax
.out:
        pop     es
        pop     ds
        popa
        leave
        ret

; AX = character row -> AX = its scanline S_I.
scroll_scan:
        mul     word [bp + S_SPR]
        add     ax, [bp + S_I]
        ret

; AX = scanline (or text row) -> DI at the window's left edge.
scroll_addr:
        push    dx
        cmp     word [bp + S_KIND], 1
        jne     .linear
        mov     dx, ax
        shr     ax, 1
        imul    ax, ax, 80
        test    dl, 1
        jz      .x
        add     ax, 0x2000
        jmp     .x
.linear:
        mul     word [bp + S_PITCH]
        add     ax, [bp + S_BASE]
.x:
        add     ax, [bp + S_X]
        mov     di, ax
        pop     dx
        ret

scroll_copy_row:
        cmp     word [bp + S_KIND], 2
        jne     .go
        mov     dx, 0x3CE
        mov     ax, 0x0105              ; write mode 1 moves all four planes through the latches
        out     dx, ax
.go:
        mov     word [bp + S_I], 0
.line:
        mov     ax, [bp + S_I]
        cmp     ax, [bp + S_SPR]
        jae     .ret
        mov     ax, [bp + S_SRC]
        call    scroll_scan
        call    scroll_addr
        mov     si, di
        mov     ax, [bp + S_DST]
        call    scroll_scan
        call    scroll_addr
        mov     cx, [bp + S_W]
        rep     movsb
        inc     word [bp + S_I]
        jmp     .line
.ret:
        ret

scroll_blank_row:
        cmp     word [bp + S_KIND], 2
        jne     .go
        mov     dx, 0x3CE
        mov     ax, 0x0205
        out     dx, ax
        mov     ax, 0xFF08
        out     dx, ax
.go:
        mov     word [bp + S_I], 0
.line:
        mov     ax, [bp + S_I]
        cmp     ax, [bp + S_SPR]
        jae     .ret
        mov     ax, [bp + S_DST]
        call    scroll_scan
        call    scroll_addr
        mov     cx, [bp + S_W]
        mov     al, [bp + S_FILL]
        cmp     word [bp + S_KIND], 0
        jne     .bytes
        mov     ah, al
        mov     al, ' '
        shr     cx, 1
        rep     stosw
        jmp     .next
.bytes:
        rep     stosb
.next:
        inc     word [bp + S_I]
        jmp     .line
.ret:
        ret

; --- characters ----------------------------------------------------------------

; DI = offset of row/column DX on page BH in the text regen.
text_cell:
        push    ax
        push    dx
        mov     al, dh
        mul     byte [CRT_COLS]
        xor     dh, dh
        add     ax, dx
        shl     ax, 1
        mov     di, ax
        mov     al, bh
        and     al, 7
        xor     ah, ah
        mul     word [CRT_LEN]
        add     di, ax
        pop     dx
        pop     ax
        ret

read_ac:
        call    is_text
        jnc     .gfx
        call    regen_seg
        call    cursor_of_page
        call    text_cell
        mov     ax, [es:di]
        mov     [bp + F_AX], ax
        ret
.gfx:
        call    gfx_read_char
        xor     ah, ah
        mov     [bp + F_AX], ax
        ret

write_ac:
        call    is_text
        jnc     gfx_write_chars
        call    regen_seg
        call    cursor_of_page
        call    text_cell
        mov     ah, bl
        rep     stosw
        ret

write_c:
        call    is_text
        jnc     gfx_write_chars
        call    regen_seg
        call    cursor_of_page
        call    text_cell
        jcxz    .done
.next:
        stosb
        inc     di
        loop    .next
.done:
        ret

; BH page. Fills the G_ locals for the current graphics mode.
gfx_setup:
        push    ax
        push    dx
        mov     word [bp + G_KIND], 1
        mov     word [bp + G_BPC], 1
        mov     word [bp + G_H], 8
        mov     word [bp + G_BASE], 0
        mov     word [bp + G_PITCH], 80
        mov     al, [CRT_MODE]
        cmp     al, 0x0D
        jae     .planar
        cmp     al, 6
        je      .done
        mov     word [bp + G_BPC], 2
        jmp     .done
.planar:
        mov     word [bp + G_KIND], 2
        mov     al, [POINTS]
        xor     ah, ah
        mov     [bp + G_H], ax
        mov     al, [CRT_COLS]
        mov     [bp + G_PITCH], ax
        mov     al, bh
        and     al, 7
        mul     word [CRT_LEN]
        mov     [bp + G_BASE], ax
.done:
        pop     dx
        pop     ax
        ret

; AX = scanline, CX = byte column -> DI.
gfx_addr:
        push    ax
        push    dx
        cmp     word [bp + G_KIND], 1
        jne     .planar
        mov     dx, ax
        shr     ax, 1
        imul    ax, ax, 80
        test    dl, 1
        jz      .x
        add     ax, 0x2000
        jmp     .x
.planar:
        mul     word [bp + G_PITCH]
        add     ax, [bp + G_BASE]
.x:
        add     ax, cx
        mov     di, ax
        pop     dx
        pop     ax
        ret

; DX = cursor row/column -> G_Y scanline and G_X byte column.
gfx_cell:
        push    ax
        mov     al, dh
        mul     byte [bp + G_H]
        mov     [bp + G_Y], ax
        mov     al, dl
        mul     byte [bp + G_BPC]
        mov     [bp + G_X], ax
        pop     ax
        ret

; AL char -> DS:SI glyph. CGA modes take 80h-FFh from INT 1Fh, everything else comes from INT 43h.
font_ptr:
        push    ax
        push    bx
        xor     ah, ah
        mov     bx, 0x43 * 4
        cmp     word [bp + G_KIND], 1
        jne     .table
        cmp     al, 0x80
        jb      .table
        sub     al, 0x80
        mov     bx, 0x1F * 4
.table:
        mul     byte [bp + G_H]
        xor     si, si
        mov     ds, si
        lds     si, [bx]
        add     si, ax
        pop     bx
        pop     ax
        ret

; AL char, BL colour (bit 7 XORs), BH page, CX count, at that page's cursor.
gfx_write_chars:
        test    cx, cx
        jz      .none
        pusha
        push    es
        enter   G_SIZE, 0
        mov     [bp + G_COUNT], cx
        mov     [bp + G_COLOR], bl
        call    gfx_setup
        push    ds
        call    font_ptr
        push    ss
        pop     es
        lea     di, [bp + G_GLYPH]
        mov     cx, [bp + G_H]
        rep     movsb
        pop     ds
        call    cursor_of_page
        call    gfx_cell
        call    regen_seg
        cmp     word [bp + G_KIND], 2
        jne     .char
        mov     dx, 0x3CE
        mov     ax, 0x0205
        out     dx, ax
        test    byte [bp + G_COLOR], 0x80
        jz      .char
        mov     ax, 0x1803
        out     dx, ax
.char:
        mov     word [bp + G_I], 0
.line:
        mov     si, [bp + G_I]
        cmp     si, [bp + G_H]
        jae     .next_char
        mov     ax, [bp + G_Y]
        add     ax, si
        mov     cx, [bp + G_X]
        call    gfx_addr
        mov     al, [bp + G_GLYPH + si]
        call    put_glyph_line
        inc     word [bp + G_I]
        jmp     .line
.next_char:
        mov     ax, [bp + G_BPC]
        add     [bp + G_X], ax
        dec     word [bp + G_COUNT]
        jnz     .char
        cmp     word [bp + G_KIND], 2
        jne     .out
        mov     dx, 0x3CE
        mov     ax, 0x0003
        out     dx, ax
        mov     ax, 0x0005
        out     dx, ax
        mov     ax, 0xFF08
        out     dx, ax
.out:
        leave
        pop     es
        popa
.none:
        ret

; AL = glyph row bits, written at ES:DI in G_COLOR.
put_glyph_line:
        pusha
        mov     ah, [bp + G_COLOR]
        cmp     word [bp + G_KIND], 2
        je      .planar
        cmp     word [bp + G_BPC], 2
        je      .four
        test    ah, 1
        jnz     .m6
        xor     al, al
.m6:
        test    ah, 0x80
        jz      .m6set
        xor     [es:di], al
        jmp     .ret
.m6set:
        mov     [es:di], al
        jmp     .ret
.four:
        mov     dl, al
        mov     dh, ah
        and     ah, 3
        xor     bx, bx
        mov     cx, 8
.bit:
        shl     bx, 2
        shl     dl, 1
        jnc     .zero
        or      bl, ah
.zero:
        loop    .bit
        test    dh, 0x80
        jz      .set4
        xor     [es:di], bh
        xor     [es:di + 1], bl
        jmp     .ret
.set4:
        mov     [es:di], bh
        mov     [es:di + 1], bl
        jmp     .ret
.planar:
        mov     bl, al
        mov     dx, 0x3CE
        mov     ah, bl
        mov     al, 8
        out     dx, ax                  ; bit mask: foreground pels
        mov     al, [es:di]
        mov     al, [bp + G_COLOR]
        mov     [es:di], al
        test    byte [bp + G_COLOR], 0x80
        jnz     .ret
        mov     ah, bl
        not     ah
        mov     al, 8
        out     dx, ax                  ; background pels to colour 0
        mov     al, [es:di]
        mov     byte [es:di], 0
.ret:
        popa
        ret

; BH page -> AL character at that page's cursor, 0 when no glyph matches.
gfx_read_char:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        enter   G_SIZE, 0
        call    gfx_setup
        call    cursor_of_page
        call    gfx_cell
        call    regen_seg
        cmp     word [bp + G_KIND], 2
        jne     .read
        mov     dx, 0x3CE
        mov     ax, 0x0805              ; read mode 1 against colour 0
        out     dx, ax
        mov     ax, 0x0002
        out     dx, ax
        mov     ax, 0x0F07
        out     dx, ax
.read:
        mov     word [bp + G_I], 0
.line:
        mov     si, [bp + G_I]
        cmp     si, [bp + G_H]
        jae     .match
        mov     ax, [bp + G_Y]
        add     ax, si
        mov     cx, [bp + G_X]
        call    gfx_addr
        cmp     word [bp + G_KIND], 2
        jne     .cga
        mov     al, [es:di]
        not     al
        jmp     .store
.cga:
        mov     al, [es:di]
        cmp     word [bp + G_BPC], 2
        jne     .store
        mov     bh, al
        mov     bl, [es:di + 1]
        xor     al, al
        mov     cx, 8
.pel:
        shl     al, 1
        test    bh, 0xC0
        jz      .bg
        or      al, 1
.bg:
        shl     bx, 2
        loop    .pel
.store:
        mov     [bp + G_GLYPH + si], al
        inc     word [bp + G_I]
        jmp     .line
.match:
        cmp     word [bp + G_KIND], 2
        jne     .scan
        mov     dx, 0x3CE
        mov     ax, 0x0005
        out     dx, ax
.scan:
        push    ss
        pop     es
        mov     word [bp + G_COUNT], 0
.try:
        mov     al, [bp + G_COUNT]
        call    font_ptr
        lea     di, [bp + G_GLYPH]
        mov     cx, [bp + G_H]
        repe    cmpsb
        je      .found
        inc     word [bp + G_COUNT]
        cmp     word [bp + G_COUNT], 256
        jb      .try
        xor     al, al
        jmp     .out
.found:
        mov     al, [bp + G_COUNT]
.out:
        leave
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; --- dots ----------------------------------------------------------------------

; AL colour (bit 7 XORs), BH page, CX column, DX row.
write_dot:
        call    is_text
        jc      .ret
        enter   G_SIZE, 0
        mov     [bp + G_COLOR], al
        call    gfx_setup
        call    regen_seg
        mov     si, cx
        mov     ax, dx
        cmp     word [bp + G_KIND], 2
        jne     .cga
        shr     cx, 3
        call    gfx_addr
        mov     cx, si
        and     cl, 7
        mov     ah, 0x80
        shr     ah, cl
        mov     dx, 0x3CE
        mov     al, 8
        out     dx, ax
        mov     ax, 0x0205
        out     dx, ax
        test    byte [bp + G_COLOR], 0x80
        jz      .plane_write
        mov     ax, 0x1803
        out     dx, ax
.plane_write:
        mov     al, [es:di]
        mov     al, [bp + G_COLOR]
        mov     [es:di], al
        mov     ax, 0x0003
        out     dx, ax
        mov     ax, 0x0005
        out     dx, ax
        mov     ax, 0xFF08
        out     dx, ax
        jmp     .out
.cga:
        cmp     word [bp + G_BPC], 2
        jne     .mode6
        shr     cx, 2
        call    gfx_addr
        mov     cx, si
        and     cl, 3
        xor     cl, 3
        shl     cl, 1
        mov     ah, 3
        mov     al, [bp + G_COLOR]
        and     al, 3
        jmp     .apply
.mode6:
        shr     cx, 3
        call    gfx_addr
        mov     cx, si
        and     cl, 7
        xor     cl, 7
        mov     ah, 1
        mov     al, [bp + G_COLOR]
        and     al, 1
.apply:
        shl     ah, cl
        shl     al, cl
        test    byte [bp + G_COLOR], 0x80
        jz      .set
        xor     [es:di], al
        jmp     .out
.set:
        not     ah
        and     [es:di], ah
        or      [es:di], al
.out:
        leave
.ret:
        ret

; BH page, CX column, DX row -> AL.
read_dot:
        call    is_text
        jc      .ret
        enter   G_SIZE, 0
        call    gfx_setup
        call    regen_seg
        mov     si, cx
        mov     ax, dx
        cmp     word [bp + G_KIND], 2
        jne     .cga
        shr     cx, 3
        call    gfx_addr
        mov     cx, si
        and     cl, 7
        xor     cl, 7
        xor     bl, bl
        mov     dx, 0x3CE
        mov     ah, 3
.plane:
        mov     al, 4
        out     dx, ax                  ; read map select
        mov     al, [es:di]
        shr     al, cl
        and     al, 1
        shl     bl, 1
        or      bl, al
        dec     ah
        jns     .plane
        mov     ax, 0x0004
        out     dx, ax
        mov     al, bl
        jmp     .out
.cga:
        cmp     word [bp + G_BPC], 2
        jne     .mode6
        shr     cx, 2
        call    gfx_addr
        mov     cx, si
        and     cl, 3
        xor     cl, 3
        shl     cl, 1
        mov     al, [es:di]
        shr     al, cl
        and     al, 3
        jmp     .out
.mode6:
        shr     cx, 3
        call    gfx_addr
        mov     cx, si
        and     cl, 7
        xor     cl, 7
        mov     al, [es:di]
        shr     al, cl
        and     al, 1
.out:
        leave
        mov     [bp + F_AX], al
.ret:
        ret

; --- teletype and strings ------------------------------------------------------

write_tty:
        mov     bh, [ACTIVE_PAGE]
        xor     ah, ah

; AL char, BL attribute or colour, BH page, AH 1 also writes BL as the text attribute.
tty_core:
        pusha
        push    es
        call    cursor_of_page
        cmp     al, 13
        je      .cr
        cmp     al, 10
        je      .lf
        cmp     al, 8
        je      .bs
        cmp     al, 7
        je      .bell
        call    is_text
        jnc     .gfx
        call    regen_seg
        call    text_cell
        mov     [es:di], al
        test    ah, ah
        jz      .advance
        mov     [es:di + 1], bl
        jmp     .advance
.gfx:
        mov     cx, 1
        call    gfx_write_chars
.advance:
        inc     dl
        cmp     dl, [CRT_COLS]
        jb      .set
        xor     dl, dl
.lf:
        cmp     dh, [ROWS]
        jb      .down
        push    bx
        push    dx
        call    is_text
        jnc     .gfx_blank
        call    regen_seg
        call    text_cell
        mov     bh, [es:di + 1]         ; the new line takes the attribute under the cursor
        jmp     .scroll
.gfx_blank:
        xor     bh, bh
.scroll:
        mov     al, 1
        xor     cx, cx
        mov     dh, [ROWS]
        mov     dl, [CRT_COLS]
        dec     dl
        xor     bl, bl
        call    scroll_core
        pop     dx
        pop     bx
        jmp     .set
.down:
        inc     dh
        jmp     .set
.cr:
        xor     dl, dl
        jmp     .set
.bs:
        test    dl, dl
        jz      .set
        dec     dl
        jmp     .set
.bell:
        call    beep
        jmp     .out
.set:
        call    set_cpos
.out:
        pop     es
        popa
        ret

; About 896 Hz for half a second through PIT channel 2.
beep:
        pusha
        mov     al, 0xB6
        out     0x43, al
        mov     ax, 1331
        out     0x42, al
        mov     al, ah
        out     0x42, al
        in      al, 0x61
        mov     bl, al
        or      al, 3
        out     0x61, al
        mov     cx, 33000               ; port 61h bit 4 flips every 15 us refresh
        in      al, 0x61
        and     al, 0x10
        mov     ah, al
.wait:
        in      al, 0x61
        and     al, 0x10
        cmp     al, ah
        je      .wait
        mov     ah, al
        loop    .wait
        mov     al, bl
        out     0x61, al
        popa
        ret

; AL mode, BH page, BL attribute, CX count, DX position, ES:BP string.
write_string:
        cmp     al, 3
        ja      .ret
        jcxz    .ret
        mov     si, [bp + F_BP]
        call    cursor_of_page
        push    dx
        mov     dx, [bp + F_DX]
        call    set_cpos
.next:
        mov     al, [es:si]
        inc     si
        test    byte [bp + F_AX], 2
        jz      .attr
        mov     bl, [es:si]
        inc     si
.attr:
        mov     ah, 1
        call    tty_core
        loop    .next
        pop     dx
        test    byte [bp + F_AX], 1
        jnz     .ret
        call    set_cpos
.ret:
        ret

; --- palette -------------------------------------------------------------------

; AH = attribute index, AL = value. Leaves the palette address source off.
pal_set:
        push    ax
        push    dx
        push    ax
        mov     dx, [ADDR_6845]
        add     dx, 6
        in      al, dx
        mov     dx, 0x3C0
        pop     ax
        xchg    al, ah
        out     dx, al
        xchg    al, ah
        out     dx, al
        pop     dx
        pop     ax
        ret

pal_on:
        push    ax
        push    dx
        mov     dx, [ADDR_6845]
        add     dx, 6
        in      al, dx
        mov     dx, 0x3C0
        mov     al, 0x20
        out     dx, al
        pop     dx
        pop     ax
        ret

; AH=0Bh: BH 0 sets border (and background in graphics), BH 1 picks the CGA palette.
set_color:
        cmp     word [ADDR_6845], 0x3B4
        je      .ret
        test    bh, bh
        jnz     .palette
        mov     al, [CRT_PALETTE]
        and     al, 0xE0
        mov     ah, bl
        and     ah, 0x1F
        or      al, ah
        mov     [CRT_PALETTE], al
        mov     al, bl                  ; intensity moves from bit 3 to bit 4
        and     al, 0x07
        test    bl, 0x08
        jz      .bg
        or      al, 0x10
.bg:
        call    is_text
        jc      .border
        mov     ah, 0
        call    pal_set
.border:
        cmp     byte [CRT_MODE], 3
        ja      .overscan
        call    is_ecd                  ; no border on the 350-line display
        jc      .keep
.overscan:
        mov     ah, 0x11
        call    pal_set
.keep:
        mov     bl, [CRT_PALETTE]
        shr     bl, 5
.palette:
        call    is_text
        jc      .done
        mov     al, [CRT_PALETTE]
        and     al, 0xDF
        and     bl, 1
        jz      .id
        or      al, 0x20
.id:
        mov     [CRT_PALETTE], al
        and     al, 0x10
        or      al, 2
        or      al, bl
        mov     ah, 1
.colors:
        call    pal_set
        add     al, 2
        inc     ah
        cmp     ah, 4
        jb      .colors
.done:
        call    pal_on
.ret:
        ret

; AH=10h
set_palette:
        cmp     al, 0
        jne     .overscan
        mov     ah, bl
        and     ah, 0x1F
        mov     al, bh
        call    pal_set
        jmp     .on
.overscan:
        cmp     al, 1
        jne     .all
        mov     ah, 0x11
        mov     al, bh
        call    pal_set
        jmp     .on
.all:
        cmp     al, 2
        jne     .blink
        mov     si, dx
        xor     ah, ah
.reg:
        mov     al, [es:si]
        call    pal_set
        inc     si
        inc     ah
        cmp     ah, 0x10
        jb      .reg
        mov     al, [es:si]
        mov     ah, 0x11
        call    pal_set
        jmp     .on
.blink:
        cmp     al, 3
        jne     .ret
        call    parm_ptr                ; Mode Control is write-only, so start from the table
        mov     al, [es:si + P_ATTR + 0x10]
        and     al, 0xF7
        test    bl, 1
        jz      .ar10
        or      al, 0x08
.ar10:
        mov     ah, 0x10
        call    pal_set
.on:
        call    pal_on
.ret:
        ret

; --- character generator -------------------------------------------------------

char_gen:
        cmp     al, 0x03
        jb      .load
        je      .block
        cmp     al, 0x10
        jb      .ret
        cmp     al, 0x13
        jb      .load
        cmp     al, 0x20
        jb      .ret
        cmp     al, 0x24
        jb      .graphics
        cmp     al, 0x30
        je      .info
.ret:
        ret

.block:
        mov     dx, 0x3C4
        mov     ax, 0x0100
        out     dx, ax
        mov     al, 3
        mov     ah, bl
        out     dx, ax
        mov     ax, 0x0300
        out     dx, ax
        ret

.load:
        mov     ah, al
        and     ah, 0x0F
        push    ds
        test    ah, ah
        jnz     .rom
        mov     si, [bp + F_BP]
        push    es
        pop     ds
        jmp     .go
.rom:
        mov     cx, 256
        xor     dx, dx
        push    cs
        pop     ds
        mov     si, font14
        mov     bh, 14
        cmp     ah, 1
        je      .go
        mov     si, font8
        mov     bh, 8
.go:
        call    load_font
        pop     ds
        push    bx
        call    parm_ptr                ; the TR's "initiates a mode set", keeping the regen buffer
        call    program_regs
        pop     bx
        cmp     byte [bp + F_AX], 0x10
        jae     .recalc
        mov     cx, [CURSOR_MODE]
        jmp     set_ctype

; AL=1xh: POINTS, ROWS, CRT_LEN and CRTC 09-0B and 12 (14 in mode 7) follow the glyph height.
.recalc:
        mov     cl, bh
        test    cl, cl
        jz      .ret
        xor     ch, ch
        mov     [POINTS], cx
        mov     ax, 200
        cmp     byte [es:si + P_POINTS], 14
        jne     .lines
        mov     ax, 350
.lines:
        div     cl
        dec     al
        mov     [ROWS], al
        inc     al
        mul     byte [CRT_COLS]
        shl     ax, 1
        mov     [CRT_LEN], ax
        mov     dx, [ADDR_6845]
        mov     ah, cl
        dec     ah
        mov     al, 0x09
        out     dx, ax
        dec     ah
        mov     al, 0x0A
        out     dx, ax
        mov     ax, 0x000B
        out     dx, ax
        mov     al, [ROWS]
        inc     al
        mul     cl
        dec     ax
        mov     ah, al
        mov     al, 0x12
        out     dx, ax
        cmp     byte [CRT_MODE], 7
        jne     .done
        mov     al, 0x14
        mov     ah, cl
        out     dx, ax
.done:
        ret

; AL=20h-23h: graphics character tables.
.graphics:
        push    ds
        xor     si, si
        mov     ds, si
        cmp     al, 0x20
        jne     .int43
        mov     si, [bp + F_BP]
        cli
        mov     [0x1F * 4], si
        mov     [0x1F * 4 + 2], es
        sti
        pop     ds
        ret
.int43:
        mov     di, [bp + F_BP]
        mov     si, es
        cmp     al, 0x21
        je      .set43
        mov     si, cs
        mov     di, font14
        mov     cx, 14
        cmp     al, 0x22
        je      .set43
        mov     di, font8
        mov     cx, 8
.set43:
        cli
        mov     [0x43 * 4], di
        mov     [0x43 * 4 + 2], si
        sti
        pop     ds
        mov     [POINTS], cx
        mov     al, dl
        test    bl, bl
        jz      .rows
        cmp     bl, 3
        jbe     .rowtbl
        mov     bl, 2
.rowtbl:
        xor     bh, bh
        mov     al, [cs:rows_by_spec + bx]
.rows:
        dec     al
        mov     [ROWS], al
        ret

; AL=30h: CX points, DL rows, ES:BP the table BH names.
.info:
        mov     ax, [POINTS]
        mov     [bp + F_CX], ax
        mov     al, [ROWS]
        mov     [bp + F_DX], al
        cmp     bh, 1
        ja      .rom_table
        push    ds
        xor     si, si
        mov     ds, si
        mov     si, 0x1F * 4
        test    bh, bh
        jz      .vector
        mov     si, 0x43 * 4
.vector:
        les     ax, [si]
        pop     ds
        jmp     .ret_ptr
.rom_table:
        cmp     bh, 5
        ja      .info_ret
        mov     bl, bh
        sub     bl, 2
        xor     bh, bh
        shl     bx, 1
        mov     ax, [cs:rom_tables + bx]
        push    cs
        pop     es
.ret_ptr:
        mov     [bp + F_BP], ax
        mov     [bp + F_ES], es
.info_ret:
        ret

rows_by_spec    db      0, 14, 25, 43
rom_tables      dw      font14, font8, font8 + 128 * 8, alt9x14

; --- alternate select ----------------------------------------------------------

alt_select:
        cmp     bl, 0x10
        je      .info
        cmp     bl, 0x20
        jne     .ret
        push    ds
        xor     ax, ax
        mov     ds, ax
        cli
        mov     word [0x05 * 4], print_screen
        mov     [0x05 * 4 + 2], cs
        sti
        pop     ds
        ret
.info:
        mov     bh, [INFO]
        and     bh, 0x02
        shr     bh, 1
        mov     bl, [INFO]
        and     bl, 0x60
        shr     bl, 5
        mov     [bp + F_BX], bx
        mov     cl, [INFO_3]
        mov     ch, cl
        and     cl, 0x0F
        shr     ch, 4
        mov     [bp + F_CX], cx
.ret:
        ret

; INT 5 for any row count: prints ROWS+1 lines of the active page through INT 17h.
print_screen:
        sti
        pusha
        push    ds
        mov     ax, BDA
        mov     ds, ax
        cmp     byte [PRTSC_STATUS], 1
        je      .exit
        mov     byte [PRTSC_STATUS], 1
        mov     ah, 0x0F
        int     0x10
        mov     cl, ah
        mov     ch, [ROWS]
        inc     ch
        mov     ah, 0x03
        int     0x10
        push    dx
        call    .crlf
        jnz     .error
        xor     dx, dx
.row:
        mov     ah, 0x02
        int     0x10
        mov     ah, 0x08
        int     0x10
        test    al, al
        jnz     .print
        mov     al, ' '
.print:
        push    dx
        xor     dx, dx
        xor     ah, ah
        int     0x17
        pop     dx
        test    ah, 0x29
        jnz     .error
        inc     dl
        cmp     dl, cl
        jb      .row
        xor     dl, dl
        call    .crlf
        jnz     .error
        inc     dh
        cmp     dh, ch
        jb      .row
        mov     byte [PRTSC_STATUS], 0
        jmp     .restore
.error:
        mov     byte [PRTSC_STATUS], 0xFF
.restore:
        pop     dx
        mov     ah, 0x02
        int     0x10
.exit:
        pop     ds
        popa
        iret
.crlf:
        push    dx
        xor     dx, dx
        mov     ax, 0x000D
        int     0x17
        test    ah, 0x29
        jnz     .crlf_out
        mov     ax, 0x000A
        int     0x17
        test    ah, 0x29
.crlf_out:
        pop     dx
        ret

; --- tables --------------------------------------------------------------------

save_tbl:
        dw      video_parms, ROM_SEG
        times   12 dw 0

; VIDEO_PARMS, IBM EGA TR: modes 0-7, 8-C, D, E, F and 10 (64KB), F and 10 (more), 0-3 on the enhanced display
video_parms:
        db 0x28,0x18,0x08
        dw 0x0800
        db 0x0B,0x03,0x00,0x03, 0x23
        db 0x37,0x27,0x2D,0x37,0x31,0x15,0x04,0x11,0x00,0x07,0x06,0x07,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x14,0x08,0xE0,0xF0,0xA3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x08,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
        db 0x28,0x18,0x08
        dw 0x0800
        db 0x0B,0x03,0x00,0x03, 0x23
        db 0x37,0x27,0x2D,0x37,0x31,0x15,0x04,0x11,0x00,0x07,0x06,0x07,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x14,0x08,0xE0,0xF0,0xA3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x08,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
        db 0x50,0x18,0x08
        dw 0x1000
        db 0x01,0x03,0x00,0x03, 0x23
        db 0x70,0x4F,0x5C,0x2F,0x5F,0x07,0x04,0x11,0x00,0x07,0x06,0x07,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x28,0x08,0xE0,0xF0,0xA3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x08,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
        db 0x50,0x18,0x08
        dw 0x1000
        db 0x01,0x03,0x00,0x03, 0x23
        db 0x70,0x4F,0x5C,0x2F,0x5F,0x07,0x04,0x11,0x00,0x07,0x06,0x07,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x28,0x08,0xE0,0xF0,0xA3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x08,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
        db 0x28,0x18,0x08
        dw 0x4000
        db 0x0B,0x03,0x00,0x02, 0x23
        db 0x37,0x27,0x2D,0x37,0x30,0x14,0x04,0x11,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x14,0x00,0xE0,0xF0,0xA2,0xFF
        db 0x00,0x13,0x15,0x17,0x02,0x04,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x03,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x30,0x0F,0x00,0xFF
        db 0x28,0x18,0x08
        dw 0x4000
        db 0x0B,0x03,0x00,0x02, 0x23
        db 0x37,0x27,0x2D,0x37,0x30,0x14,0x04,0x11,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x14,0x00,0xE0,0xF0,0xA2,0xFF
        db 0x00,0x13,0x15,0x17,0x02,0x04,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x03,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x30,0x0F,0x00,0xFF
        db 0x50,0x18,0x08
        dw 0x4000
        db 0x01,0x01,0x00,0x06, 0x23
        db 0x70,0x4F,0x59,0x2D,0x5E,0x06,0x04,0x11,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xE0,0x23,0xC7,0x28,0x00,0xDF,0xEF,0xC2,0xFF
        db 0x00,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x01,0x00,0x01,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x0D,0x00,0xFF
        db 0x50,0x18,0x0E
        dw 0x1000
        db 0x00,0x03,0x00,0x03, 0xA6
        db 0x60,0x4F,0x56,0x3A,0x51,0x60,0x70,0x1F,0x00,0x0D,0x0B,0x0C,0x00,0x00,0x00,0x00,0x5E,0x2E,0x5D,0x28,0x0D,0x5E,0x6E,0xA3,0xFF
        db 0x00,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x10,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x0E,0x00,0x0F,0x08
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0A,0x00,0xFF
%rep 3
        db 0x28,0x18,0x08
        dw 0x4000
        db 0x00,0x00,0x00,0x03, 0x23
        db 0x37,0x27,0x2D,0x37,0x31,0x15,0x04,0x11,0x00,0x07,0x06,0x07,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x14,0x08,0xE0,0xF0,0xA3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x08,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
%endrep
        db 0x50,0x18,0x08
        dw 0x1000
        db 0x01,0x04,0x00,0x07, 0x23
        db 0x70,0x4F,0x5C,0x2F,0x5F,0x07,0x04,0x11,0x00,0x07,0x06,0x07,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x28,0x08,0xE0,0xF0,0xA3,0xFF
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0xFF
        db 0x50,0x18,0x0E
        dw 0x1000
        db 0x00,0x04,0x00,0x07, 0xA6
        db 0x60,0x4F,0x56,0x3A,0x51,0x60,0x70,0x1F,0x00,0x0D,0x0B,0x0C,0x00,0x00,0x00,0x00,0x5E,0x2E,0x5D,0x28,0x0D,0x5E,0x6E,0xA3,0xFF
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0E,0x00,0x0F,0x08
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0xFF
        db 0x28,0x18,0x08
        dw 0x2000
        db 0x0B,0x0F,0x00,0x06, 0x23
        db 0x37,0x27,0x2D,0x37,0x30,0x14,0x04,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xE1,0x24,0xC7,0x14,0x00,0xE0,0xF0,0xE3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF
        db 0x50,0x18,0x08
        dw 0x4000
        db 0x01,0x0F,0x00,0x06, 0x23
        db 0x70,0x4F,0x59,0x2D,0x5E,0x06,0x04,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xE0,0x23,0xC7,0x28,0x00,0xDF,0xEF,0xE3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF
        db 0x50,0x18,0x0E
        dw 0x8000
        db 0x05,0x0F,0x00,0x00, 0xA2
        db 0x60,0x4F,0x56,0x1A,0x50,0xE0,0x70,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5E,0x2E,0x5D,0x14,0x00,0x5E,0x6E,0x8B,0xFF
        db 0x00,0x08,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x18,0x00,0x00,0x0B,0x00,0x05,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x07,0x0F,0xFF
        db 0x50,0x18,0x0E
        dw 0x8000
        db 0x05,0x0F,0x00,0x00, 0xA7
        db 0x5B,0x4F,0x53,0x17,0x50,0xBA,0x6C,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5E,0x2B,0x5D,0x14,0x0F,0x5F,0x0A,0x8B,0xFF
        db 0x00,0x01,0x00,0x00,0x04,0x07,0x00,0x00,0x00,0x01,0x00,0x00,0x04,0x07,0x00,0x00,0x01,0x00,0x05,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x07,0x0F,0xFF
        db 0x50,0x18,0x0E
        dw 0x8000
        db 0x01,0x0F,0x00,0x06, 0xA2
        db 0x60,0x4F,0x56,0x3A,0x50,0x60,0x70,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5E,0x2E,0x5D,0x28,0x00,0x5E,0x6E,0xE3,0xFF
        db 0x00,0x08,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x18,0x00,0x00,0x0B,0x00,0x05,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF
        db 0x50,0x18,0x0E
        dw 0x8000
        db 0x01,0x0F,0x00,0x06, 0xA7
        db 0x5B,0x4F,0x53,0x37,0x52,0x00,0x6C,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5E,0x2B,0x5D,0x28,0x0F,0x5F,0x0A,0xE3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x01,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF
%rep 2
        db 0x28,0x18,0x0E
        dw 0x0800
        db 0x0B,0x03,0x00,0x03, 0xA7
        db 0x2D,0x27,0x2B,0x2D,0x28,0x6D,0x6C,0x1F,0x00,0x0D,0x06,0x07,0x00,0x00,0x00,0x00,0x5E,0x2B,0x5D,0x14,0x0F,0x5E,0x0A,0xA3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x08,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
%endrep
%rep 2
        db 0x50,0x18,0x0E
        dw 0x1000
        db 0x01,0x03,0x00,0x03, 0xA7
        db 0x5B,0x4F,0x53,0x37,0x51,0x5B,0x6C,0x1F,0x00,0x0D,0x06,0x07,0x00,0x00,0x00,0x00,0x5E,0x2B,0x5D,0x28,0x0F,0x5E,0x0A,0xA3,0xFF
        db 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x08,0x00,0x0F,0x00
        db 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
%endrep
video_parms_end:
%if video_parms_end - video_parms != 23 * P_SIZE
%error "VIDEO_PARMS must hold 23 entries"
%endif

font8:
%include "font8.inc"
font14:
%include "font14.inc"
alt9x14:
        db      0

        times   ROM_SIZE - 1 - ($ - $$) db 0xFF
        db      0
