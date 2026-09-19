#!/usr/bin/env python3
"""Assemble the Pac-Man board hardware self-test ROM (not Namco Pac-Man).

Boots like a Midway cabinet self-test (crosshatch, then color bars), then
holds a help screen whose 8×8 font is original — not a dump of pacman.5e —
explaining that Namco's ROMs are still under copyright and the user has to
load their own set. No Namco graphics or code.
"""
import argparse, json, os, struct, zlib

PROG_SIZE = 0x4000
SCREEN_BYTES = 0x800  # 0x400 videoram + 0x400 colorram
WAIT_FRAMES = 90      # ~1.5 s at 60.6 Hz per test screen

# Upright tile grid (after ROT90): 28 columns × 36 rows.
U_COLS, U_ROWS = 28, 36

# Original 8×8 caps, era-looking but not Namco's 5E bitmaps. Bit 7 = left.
FONT = {
    " ": (0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
    "0": (0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00),
    "1": (0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00),
    "2": (0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00),
    "3": (0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00),
    "4": (0x0C, 0x1C, 0x2C, 0x4C, 0x7E, 0x0C, 0x0C, 0x00),
    "5": (0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00),
    "6": (0x3C, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00),
    "7": (0x7E, 0x06, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00),
    "8": (0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00),
    "9": (0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00),
    "A": (0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00),
    "B": (0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00),
    "C": (0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00),
    "D": (0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00),
    "E": (0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00),
    "F": (0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00),
    "G": (0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00),
    "H": (0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00),
    "I": (0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00),
    "J": (0x3E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00),
    "K": (0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00),
    "L": (0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00),
    "M": (0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00),
    "N": (0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00),
    "O": (0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00),
    "P": (0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00),
    "Q": (0x3C, 0x66, 0x66, 0x66, 0x6A, 0x6C, 0x36, 0x00),
    "R": (0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00),
    "S": (0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00),
    "T": (0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00),
    "U": (0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00),
    "V": (0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00),
    "W": (0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00),
    "X": (0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00),
    "Y": (0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00),
    "Z": (0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00),
    "-": (0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00),
    ".": (0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00),
    ",": (0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30),
    "!": (0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00),
    "?": (0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00),
    "'": (0x18, 0x18, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00),
    "/": (0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00),
    ":": (0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00),
}

# Help-screen layout (upright). Title row is what Machine.HwtestHelpScreen
# asserts; keep that line/row in sync with the GoogleTest.
HELP_TITLE = "PAC-MAN ARCADE"
HELP_TITLE_ROW = 4
HELP_LINES = [
    (4,  HELP_TITLE,          1),
    (7,  "SELF TEST OK",      2),
    (11, "THIS PAGE DOES NOT", 2),
    (13, "INCLUDE NAMCO'S",   2),
    (15, "PAC-MAN ROMS",      2),
    (18, "THEY ARE STILL",    3),
    (20, "UNDER COPYRIGHT",   3),
    (23, "IF YOU OWN A",      2),
    (25, "MIDWAY PACMAN SET", 2),
    (27, "LOAD IT YOURSELF",  1),
    (30, "IT STAYS IN THIS",  2),
    (32, "BROWSER ONLY",      2),
]


class Asm:
    def __init__(self):
        self.mem = bytearray(PROG_SIZE)
        self.pc = 0

    def org(self, addr):
        self.pc = addr

    def db(self, *bs):
        for b in bs:
            self.mem[self.pc] = b & 0xFF
            self.pc += 1

    def word(self, n):
        self.db(n & 0xFF, (n >> 8) & 0xFF)

    def jp(self, addr):
        self.db(0xC3)
        self.word(addr)

    def call(self, addr):
        self.db(0xCD)
        self.word(addr)

    def ld_hl(self, n):
        self.db(0x21)
        self.word(n)

    def ld_de(self, n):
        self.db(0x11)
        self.word(n)

    def ld_bc(self, n):
        self.db(0x01)
        self.word(n)

    def ld_sp(self, n):
        self.db(0x31)
        self.word(n)

    def ld_a(self, n):
        self.db(0x3E, n)

    def ld_nn_a(self, addr):
        self.db(0x32)
        self.word(addr)

    def ld_a_nn(self, addr):
        self.db(0x3A)
        self.word(addr)


def vram_offset(col, row):
    """Native 36×28 tilemap offset — same decode as Video::vram_offset."""
    c = col - 2
    r = row + 2
    if c & 0x20:
        return r + ((c & 0x1F) << 5)
    return c + (r << 5)


def put_upright(video, color, ucol, urow, code, attr):
    """Place a tile at upright (ucol, urow). Native col = upright y, native
    row = 27 - upright x, matching render()'s ROT90 (FLIP_X | SWAP_XY)."""
    if not (0 <= ucol < U_COLS and 0 <= urow < U_ROWS):
        return
    ncol, nrow = urow, 27 - ucol
    offs = vram_offset(ncol, nrow) & 0x3FF
    video[offs] = code & 0xFF
    color[offs] = attr & 0xFF


def center_col(text):
    return max(0, (U_COLS - len(text)) // 2)


def put_text(video, color, ucol, urow, text, attr):
    for i, ch in enumerate(text.upper()):
        put_upright(video, color, ucol + i, urow, ord(ch), attr)


def screen_crosshatch():
    video = bytearray(0x400)
    color = bytearray(0x400)
    for uc in range(U_COLS):
        for ur in range(U_ROWS):
            put_upright(video, color, uc, ur, 1, 2)
    return video + color


def screen_color_bars():
    video = bytearray(0x400)
    color = bytearray(0x400)
    # Six vertical bars (upright x). Tile 2 is solid; attrs 4–9 are the
    # bar colors in lookup_prom.
    bar_w = U_COLS // 6
    for uc in range(U_COLS):
        attr = 4 + min(uc // bar_w, 5)
        for ur in range(U_ROWS):
            put_upright(video, color, uc, ur, 2, attr)
    return video + color


def screen_help():
    video = bytearray(0x400)
    color = bytearray(0x400)
    for urow, text, attr in HELP_LINES:
        put_text(video, color, center_col(text), urow, text, attr)
    return video + color


def assemble_program(screen1, screen2, screen3):
    a = Asm()
    a.org(0x0000)
    a.jp(0x0200)

    # IM 1 vblank: kick the watchdog, bump the frame counter at $4C08.
    a.org(0x0038)
    a.db(0xF5)             # PUSH AF
    a.db(0xAF)             # XOR A
    a.ld_nn_a(0x50C0)      # LD (50C0),A  watchdog
    a.ld_a_nn(0x4C08)      # LD A,(4C08)
    a.db(0x3C)             # INC A
    a.ld_nn_a(0x4C08)      # LD (4C08),A
    a.db(0xF1)             # POP AF
    a.db(0xFB)             # EI
    a.db(0xED, 0x4D)       # RETI

    # copy_screen: HL → $4000/$4400 (0x400 bytes each).
    a.org(0x0100)
    copy_screen = a.pc
    a.ld_de(0x4000)
    a.ld_bc(0x0400)
    a.db(0xED, 0xB0)       # LDIR
    a.ld_de(0x4400)
    a.ld_bc(0x0400)
    a.db(0xED, 0xB0)
    a.db(0xC9)             # RET

    # wait90: until (4C08) has advanced WAIT_FRAMES, echoing IN0 to $4C10.
    wait90 = a.pc
    a.ld_a_nn(0x4C08)
    a.db(0xC6, WAIT_FRAMES)  # ADD A,n
    a.db(0x47)               # LD B,A
    wait_loop = a.pc
    a.ld_a_nn(0x5000)        # IN0
    a.ld_nn_a(0x4C10)
    a.ld_a_nn(0x4C08)
    a.db(0xB8)               # CP B
    rel = wait_loop - (a.pc + 2)
    a.db(0x20, rel & 0xFF)   # JR NZ,wait_loop
    a.db(0xC9)

    # hide_sprites: zero sprite RAM + XY shifters so they don't cover text.
    hide_sprites = a.pc
    a.db(0xAF)               # XOR A
    a.ld_hl(0x4FF0)
    a.db(0x06, 16)           # LD B,16
    loop1 = a.pc
    a.db(0x77)               # LD (HL),A
    a.db(0x23)               # INC HL
    rel = loop1 - (a.pc + 2)
    a.db(0x10, rel & 0xFF)   # DJNZ
    a.ld_hl(0x5060)
    a.db(0x06, 16)
    loop2 = a.pc
    a.db(0x77)
    a.db(0x23)
    rel = loop2 - (a.pc + 2)
    a.db(0x10, rel & 0xFF)
    a.db(0xC9)

    a.org(0x0200)
    a.db(0xF3)               # DI
    a.db(0xED, 0x56)         # IM 1
    a.ld_sp(0x4FE0)

    a.ld_hl(0x4C00)          # signature TST1
    a.db(0x36, ord("T"))
    a.db(0x23)
    a.db(0x36, ord("S"))
    a.db(0x23)
    a.db(0x36, ord("T"))
    a.db(0x23)
    a.db(0x36, ord("1"))

    # Sprite 0 parked mid-screen for the two test patterns (sprite test).
    a.ld_a(0x00)
    a.ld_nn_a(0x4FF0)
    a.ld_a(0x0A)             # color attr 10
    a.ld_nn_a(0x4FF1)
    a.ld_a(0x80)
    a.ld_nn_a(0x5060)
    a.ld_a(0x80)
    a.ld_nn_a(0x5061)

    a.ld_a(0x01)
    a.ld_nn_a(0x5001)        # sound enable
    a.ld_a(0x0F)
    a.ld_nn_a(0x5055)        # voice 0 volume
    a.ld_a(0x01)
    a.ld_nn_a(0x5050)
    a.ld_a(0x08)
    a.ld_nn_a(0x5051)
    a.ld_a(0x00)
    a.ld_nn_a(0x5045)        # waveform 0

    a.ld_a(0x01)
    a.ld_nn_a(0x5000)        # IRQ enable
    a.db(0xFB)               # EI

    a.ld_hl(0x0800)
    a.call(copy_screen)
    a.call(wait90)
    a.ld_hl(0x1000)
    a.call(copy_screen)
    a.call(wait90)
    a.call(hide_sprites)
    a.ld_hl(0x1800)
    a.call(copy_screen)

    forever = a.pc
    a.ld_a_nn(0x5000)
    a.ld_nn_a(0x4C10)
    rel = forever - (a.pc + 2)
    a.db(0x18, rel & 0xFF)

    a.org(0x0800)
    a.mem[0x0800:0x0800 + SCREEN_BYTES] = screen1
    a.org(0x1000)
    a.mem[0x1000:0x1000 + SCREEN_BYTES] = screen2
    a.org(0x1800)
    a.mem[0x1800:0x1800 + SCREEN_BYTES] = screen3
    return a.mem


def plot_tile(rom, code, x, y):
    base = code * 16
    idx = base + (y + 8 if x < 4 else y)
    xb = x & 3
    rom[idx] |= (1 << (7 - xb)) | (1 << (3 - xb))


def tiles():
    rom = bytearray(4096)
    # Tile 1: top+left edges → a crosshatch when the map is filled with it.
    for x in range(8):
        plot_tile(rom, 1, x, 0)
    for y in range(8):
        plot_tile(rom, 1, 0, y)
    # Tile 2: solid (color bars).
    for y in range(8):
        for x in range(8):
            plot_tile(rom, 2, x, y)
    for ch, rows in FONT.items():
        code = ord(ch)
        for y, bits in enumerate(rows):
            # Glyphs are drawn in *upright* (lx right, ly down) and stored
            # already rotated so Video::render's ROT90 stands them up on
            # the cabinet monitor. Inverse of dst(x=223-ny, y=nx):
            #   native tx = ly, native ty = 7 - lx.
            for x in range(8):
                if bits & (0x80 >> x):
                    plot_tile(rom, code, y, 7 - x)
    return rom


def sprites():
    rom = bytearray(4096)
    for cell in range(4):
        base = cell * 16
        for y in range(8):
            rom[base + y] = 0xFF
            rom[base + 8 + y] = 0xFF
    return rom


def color_prom():
    p = bytearray(32)
    p[0] = 0x00   # black
    p[1] = 0x07   # red
    p[2] = 0x38   # green
    p[3] = 0xC0   # blue
    p[4] = 0x3F   # yellow
    p[5] = 0x38 | 0xC0  # cyan
    p[6] = 0x07 | 0x38 | 0xC0  # white
    p[7] = 0x07 | 0xC0  # magenta
    return p


def lookup_prom():
    p = bytearray(256)
    def row(attr, pix0, pix1, pix2, pix3):
        p[(attr << 2) | 0] = pix0
        p[(attr << 2) | 1] = pix1
        p[(attr << 2) | 2] = pix2
        p[(attr << 2) | 3] = pix3
    row(0, 0, 0, 0, 0)
    row(1, 0, 4, 4, 4)   # yellow text
    row(2, 0, 6, 6, 6)   # white text / crosshatch
    row(3, 0, 1, 1, 1)   # red text
    row(4, 1, 1, 1, 1)   # solid red bar
    row(5, 4, 4, 4, 4)   # yellow
    row(6, 2, 2, 2, 2)   # green
    row(7, 5, 5, 5, 5)   # cyan
    row(8, 3, 3, 3, 3)   # blue
    row(9, 6, 6, 6, 6)   # white
    row(10, 0, 5, 4, 6)  # sprite: cyan/yellow/white
    return p


def wave_prom():
    p = bytearray(256)
    for i in range(32):
        p[i] = i >> 1
    return p


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def write_c_header(path, blobs):
    with open(path, "w") as f:
        f.write("// Generated by gen_hwtest.py — do not edit.\n")
        f.write("#pragma once\n#include <cstdint>\n#include <array>\n")
        f.write("namespace pacman { namespace hwtest {\n")
        for name, data in blobs.items():
            f.write(f"inline const std::array<uint8_t, {len(data)}> {name} = {{")
            f.write(",".join(str(b) for b in data))
            f.write("};\n")
        f.write("}} // namespace\n")


def write_png(path, w, h, rgb_rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def marquee_png(path):
    """Landing-page thumbnail: an original approximation of a 1980 upright
    marquee's *layout* (black housing, backlit yellow plexi, the game's
    name, a pellet trail, four color chips). Not a scan or redraw of
    Namco/Midway art — no official logotype, no character sprites, no
    Midway mark. Uses this file's own 8×8 font. 286×128 matches the
    Altair card so `.machine-card .shot`'s 143×64 box doesn't crop it."""
    w, h = 286, 128
    pix = [[(14, 12, 10) for _ in range(w)] for _ in range(h)]

    def put(x, y, rgb):
        if 0 <= x < w and 0 <= y < h:
            pix[y][x] = rgb

    def lerp(a, b, t):
        return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

    # Inner plexi, inset in a black housing with a thin gold lip.
    x0, y0, x1, y1 = 8, 8, w - 8, h - 8
    for y in range(h):
        for x in range(w):
            in_lip = 5 <= x < w - 5 and 5 <= y < h - 5
            in_plexi = x0 <= x < x1 and y0 <= y < y1
            if in_plexi:
                ty = (y - y0) / (y1 - y0)
                tx = abs((x - w / 2) / (w / 2))
                base = lerp((255, 232, 70), (196, 140, 16), ty)
                # Hotter in the middle, like a backlit marquee.
                pix[y][x] = lerp(base, (255, 250, 180), 0.28 * (1 - tx) * (1 - abs(ty - 0.35)))
            elif in_lip:
                pix[y][x] = (196, 160, 32) if (x == 5 or y == 5 or x == w - 6 or y == h - 6) else (40, 32, 16)

    def blit_char(ch, ox, oy, scale, fill, outline):
        rows = FONT.get(ch, FONT[" "])
        for gy, bits in enumerate(rows):
            if gy == 7:
                continue
            for gx in range(8):
                if not (bits & (0x80 >> gx)):
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        px, py = ox + gx * scale + dx, oy + gy * scale + dy
                        for ox2, oy2 in ((-1, 0), (1, 0), (0, -1), (0, 1),
                                         (-1, -1), (1, -1), (-1, 1), (1, 1)):
                            put(px + ox2, py + oy2, outline)
        for gy, bits in enumerate(rows):
            if gy == 7:
                continue
            for gx in range(8):
                if not (bits & (0x80 >> gx)):
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        put(ox + gx * scale + dx, oy + gy * scale + dy, fill)

    text = "PAC-MAN"
    scale, gap = 4, 3
    cw = 8 * scale
    tw = len(text) * cw + (len(text) - 1) * gap
    tx = (w - tw) // 2
    ty = 22
    fill, outline = (176, 16, 16), (32, 8, 0)
    for i, ch in enumerate(text):
        blit_char(ch, tx + i * (cw + gap), ty, scale, fill, outline)

    # Pellet trail (plain circles — not maze art) and four color chips
    # standing in for the attract-screen palette, not ghost sprites.
    pellets = 8
    py = 98
    span = 150
    p0 = (w - span) // 2 - 10
    for i in range(pellets):
        cx = p0 + i * (span // (pellets - 1))
        r = 3 if i % 2 == 0 else 2
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if dx * dx + dy * dy <= r * r:
                    put(cx + dx, py + dy, (255, 248, 220))

    chips = ((255, 32, 32), (255, 176, 208), (32, 224, 224), (255, 176, 32))
    cx0 = p0 + span + 18
    for i, col in enumerate(chips):
        cx = cx0 + i * 14
        for dy in range(-5, 6):
            for dx in range(-5, 6):
                if dx * dx + dy * dy <= 25:
                    put(cx + dx, py + dy, col)

    rows = [bytearray(c for rgb in row for c in rgb) for row in pix]
    write_png(path, w, h, rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--header")
    ap.add_argument("--png")
    args = ap.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    s1, s2, s3 = screen_crosshatch(), screen_color_bars(), screen_help()
    blobs = {
        "program": assemble_program(s1, s2, s3),
        "tiles": tiles(),
        "sprites": sprites(),
        "color_prom": color_prom(),
        "lookup_prom": lookup_prom(),
        "wave_prom": wave_prom(),
    }
    names = {
        "program": "program.bin",
        "tiles": "pacman.5e",
        "sprites": "pacman.5f",
        "color_prom": "82s123.7f",
        "lookup_prom": "82s126.4a",
        "wave_prom": "82s126.1m",
    }
    for k, data in blobs.items():
        with open(os.path.join(args.dir, names[k]), "wb") as f:
            f.write(data)
    with open(os.path.join(args.dir, "82s126.3m"), "wb") as f:
        f.write(blobs["wave_prom"])
    for i, name in enumerate(("pacman.6e", "pacman.6f", "pacman.6h", "pacman.6j")):
        with open(os.path.join(args.dir, name), "wb") as f:
            f.write(blobs["program"][i * 4096:(i + 1) * 4096])
    if args.header:
        os.makedirs(os.path.dirname(os.path.abspath(args.header)) or ".", exist_ok=True)
        write_c_header(args.header, blobs)
    if args.png:
        os.makedirs(os.path.dirname(os.path.abspath(args.png)) or ".", exist_ok=True)
        marquee_png(args.png)
    crcs = {names[k]: crc32(data) for k, data in blobs.items()}
    for i, name in enumerate(("pacman.6e", "pacman.6f", "pacman.6h", "pacman.6j")):
        crcs[name] = crc32(blobs["program"][i * 4096:(i + 1) * 4096])
    with open(os.path.join(args.dir, "crc.json"), "w") as f:
        json.dump({k: f"{v:08x}" for k, v in crcs.items()}, f)
    print("hwtest CRCs:")
    for k, v in crcs.items():
        print(f"  {k}: {v:08x}")


if __name__ == "__main__":
    main()
