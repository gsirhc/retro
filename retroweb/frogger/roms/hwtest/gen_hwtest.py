#!/usr/bin/env python3
"""Assemble the Frogger board hardware self-test ROM (not Konami Frogger).

Boots like a cabinet self-test (crosshatch, then color bars), then holds a
help screen whose 8×8 font is original — not a dump of frogger.606 —
explaining that Konami/Sega's ROMs are still under copyright and the user
has to load their own set. No Konami graphics or code.
"""
import argparse, json, os, struct, zlib

PROG_SIZE = 0x4000
SOUND_SIZE = 0x1800
GFX_SIZE = 0x1000
WAIT_FRAMES = 90

# Upright tile grid after ROT90: 28 columns × 32 rows (224×256).
U_COLS, U_ROWS = 28, 32

# Original 8×8 caps, era-looking but not Konami's. Bit 7 = left.
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

HELP_TITLE = "FROGGER ARCADE"
HELP_TITLE_ROW = 4
HELP_LINES = [
    (4,  HELP_TITLE,           2),
    (7,  "SELF TEST OK",       0),
    (10, "THIS PAGE DOES NOT", 0),
    (12, "INCLUDE KONAMI'S",   0),
    (14, "FROGGER ROMS",       0),
    (17, "THEY ARE STILL",     4),
    (19, "UNDER COPYRIGHT",    4),
    (22, "IF YOU OWN A",       0),
    (24, "KONAMI FROGGER SET", 0),
    (26, "LOAD IT YOURSELF",   2),
    (29, "IT STAYS IN THIS",   0),
    (31, "BROWSER ONLY",       0),
]


class Asm:
    def __init__(self, size):
        self.mem = bytearray(size)
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


def swap_d0d1(v):
    return (v & 0xFC) | ((v & 1) << 1) | ((v & 2) >> 1)


def put_upright(video, colors, ucol, urow, code, attr):
    """Place a tile at upright (ucol, urow). Native col = upright y, native
    visible row = 27 - upright x; VRAM row adds the Galaxian visarea's +2.
    Matching Video::render's ROT90 (FLIP_X | SWAP_XY)."""
    if not (0 <= ucol < U_COLS and 0 <= urow < U_ROWS):
        return
    ncol, nrow = urow, 29 - ucol
    offs = (nrow * 32 + ncol) & 0x3FF
    video[offs] = code & 0xFF
    colors[ncol] = attr & 0xFF


def center_col(text):
    return max(0, (U_COLS - len(text)) // 2)


def put_text(video, colors, ucol, urow, text, attr):
    for i, ch in enumerate(text.upper()):
        put_upright(video, colors, ucol + i, urow, ord(ch), attr)


def screen_crosshatch():
    video = bytearray(0x400)
    colors = bytearray(32)
    for uc in range(U_COLS):
        for ur in range(U_ROWS):
            put_upright(video, colors, uc, ur, 1, 0)
    return video, colors


def screen_color_bars():
    video = bytearray(0x400)
    colors = bytearray(32)
    bar_w = max(1, U_ROWS // 6)
    # Bars run along upright x (native columns = urow), six colours.
    bar_attrs = (1, 2, 4, 5, 6, 7)
    for ur in range(U_ROWS):
        attr = bar_attrs[min(ur // bar_w, 5)]
        for uc in range(U_COLS):
            put_upright(video, colors, uc, ur, 2, attr)
    return video, colors


def screen_help():
    video = bytearray(0x400)
    colors = bytearray(32)
    for urow, text, attr in HELP_LINES:
        put_text(video, colors, center_col(text), urow, text, attr)
    return video, colors


def pack_screen(video, colors):
    return bytes(video) + bytes(colors)


def assemble_main(s1, s2, s3):
    a = Asm(PROG_SIZE)
    a.org(0x0000)
    a.jp(0x0200)

    # NMI: kick the watchdog, bump the frame counter at $8008.
    a.org(0x0066)
    a.db(0xF5)             # PUSH AF
    a.ld_a_nn(0x8800)      # LD A,(8800)  watchdog
    a.ld_a_nn(0x8008)
    a.db(0x3C)             # INC A
    a.ld_nn_a(0x8008)
    a.db(0xF1)             # POP AF
    a.db(0xED, 0x45)       # RETN

    # copy_screen: HL → $A800 (0x400) then DE → objram colours ($B001 step 2).
    a.org(0x0100)
    copy_screen = a.pc
    a.ld_de(0xA800)
    a.ld_bc(0x0400)
    a.db(0xED, 0xB0)       # LDIR
    a.ld_de(0xB001)
    a.db(0x06, 32)
    col_loop = a.pc
    a.db(0x7E)             # LD A,(HL)
    a.db(0x12)             # LD (DE),A
    a.db(0x23)             # INC HL
    a.db(0x13)             # INC DE
    a.db(0x13)             # INC DE
    a.db(0x10, (col_loop - (a.pc + 2)) & 0xFF)
    a.db(0xC9)

    wait90 = a.pc
    a.ld_a_nn(0x8008)
    a.db(0xC6, WAIT_FRAMES)
    a.db(0x47)             # LD B,A
    wait_loop = a.pc
    a.ld_a_nn(0xE000)      # IN0
    a.ld_nn_a(0x8010)
    a.ld_a_nn(0x8800)      # kick
    a.ld_a_nn(0x8008)
    a.db(0xB8)             # CP B
    a.db(0x20, (wait_loop - (a.pc + 2)) & 0xFF)
    a.db(0xC9)

    hide_sprites = a.pc
    a.db(0xAF)
    a.ld_hl(0xB040)
    a.db(0x06, 32)
    hs = a.pc
    a.db(0x77)
    a.db(0x23)
    a.db(0x10, (hs - (a.pc + 2)) & 0xFF)
    a.db(0xC9)

    a.org(0x0200)
    a.db(0xF3)             # DI
    a.ld_sp(0x87F0)
    a.ld_a(0x9B)           # PPI0 all inputs
    a.ld_nn_a(0xE006)
    a.ld_a(0x80)           # PPI1 all outputs
    a.ld_nn_a(0xD006)
    a.ld_a(ord("T"))
    a.ld_nn_a(0x8000)
    a.ld_a(ord("S"))
    a.ld_nn_a(0x8001)
    a.ld_a(ord("T"))
    a.ld_nn_a(0x8002)
    a.ld_a(ord("1"))
    a.ld_nn_a(0x8003)
    a.db(0xAF)
    a.ld_nn_a(0x8008)
    a.ld_a(1)
    a.ld_nn_a(0xB808)      # NMI enable

    a.call(hide_sprites)
    a.ld_hl(0x1000)
    a.call(copy_screen)
    a.call(wait90)
    a.ld_hl(0x1440)
    a.call(copy_screen)
    a.call(wait90)
    a.ld_hl(0x1880)
    a.call(copy_screen)
    a.call(hide_sprites)

    # Pulse a sound command so the second Z80 programs the AY.
    a.ld_a(1)
    a.ld_nn_a(0xD000)
    a.ld_a(0x08)
    a.ld_nn_a(0xD002)
    a.db(0xAF)
    a.ld_nn_a(0xD002)

    hang = a.pc
    a.ld_a_nn(0xE000)
    a.ld_nn_a(0x8010)
    a.ld_a_nn(0x8800)
    a.jp(hang)

    # Screen blobs after the code.
    a.org(0x1000)
    a.mem[0x1000:0x1000 + 0x420] = s1
    a.org(0x1440)
    a.mem[0x1440:0x1440 + 0x420] = s2
    a.org(0x1880)
    a.mem[0x1880:0x1880 + 0x420] = s3
    return bytes(a.mem)


def assemble_sound():
    a = Asm(SOUND_SIZE)
    a.org(0x0000)
    a.jp(0x0100)

    a.org(0x0038)
    a.db(0xF5)
    a.ld_a(14)
    a.db(0xD3, 0x80)       # OUT (80),A  AY address (bit 7)
    a.db(0xDB, 0x40)       # IN A,(40)   command (bit 6 = data)
    a.db(0xA7)             # AND A
    jr_z = a.pc
    a.db(0x28, 0)          # JR Z, patched
    a.ld_a(0)
    a.db(0xD3, 0x80)
    a.ld_a(0xFE)
    a.db(0xD3, 0x40)
    a.ld_a(1)
    a.db(0xD3, 0x80)
    a.ld_a(1)
    a.db(0xD3, 0x40)
    a.ld_a(7)
    a.db(0xD3, 0x80)
    a.ld_a(0x38)
    a.db(0xD3, 0x40)
    a.ld_a(8)
    a.db(0xD3, 0x80)
    a.ld_a(0x0F)
    a.db(0xD3, 0x40)
    skip = a.pc
    a.mem[jr_z + 1] = (skip - (jr_z + 2)) & 0xFF
    a.db(0xF1)
    a.db(0xFB)
    a.db(0xED, 0x4D)       # RETI

    a.org(0x0100)
    a.db(0xED, 0x56)       # IM 1
    a.db(0xFB)             # EI
    loop = a.pc
    a.db(0x76)             # HALT
    a.jp(loop)
    return bytes(a.mem)


def plot_tile(rom, code, x, y):
    rom[code * 8 + (y & 7)] |= 1 << (7 - (x & 7))


def gfx_rom():
    rom = bytearray(GFX_SIZE)
    for x in range(8):
        plot_tile(rom, 1, x, 0)
    for y in range(8):
        plot_tile(rom, 1, 0, y)
    for y in range(8):
        for x in range(8):
            plot_tile(rom, 2, x, y)
            rom[0x800 + 2 * 8 + y] |= 1 << (7 - x)  # plane 1 too → pix 3
    for ch, rows in FONT.items():
        code = ord(ch)
        if code * 8 + 8 > 0x800:
            continue
        for ly, bits in enumerate(rows):
            for lx in range(8):
                if bits & (0x80 >> lx):
                    plot_tile(rom, code, ly, 7 - lx)
    return rom


def color_prom():
    # Frogger PROM: bits 0–2 R, 3–5 G, 6–7 B (blue bit 0 unconnected).
    p = bytearray(32)
    def rgb(r, g, b):
        return (r & 7) | ((g & 7) << 3) | ((b & 3) << 6)
    # group g, pix 0 black; pix 1–3 the group colour.
    colours = [
        rgb(7, 7, 3),  # 0 white
        rgb(7, 7, 0),  # 1 yellow
        rgb(7, 0, 0),  # 2 red
        rgb(0, 7, 0),  # 3 green
        rgb(0, 0, 3),  # 4 blue
        rgb(0, 7, 3),  # 5 cyan
        rgb(7, 0, 3),  # 6 magenta
        rgb(7, 5, 0),  # 7 orange
    ]
    for g, c in enumerate(colours):
        p[g * 4 + 0] = 0
        p[g * 4 + 1] = c
        p[g * 4 + 2] = c
        p[g * 4 + 3] = c
    return p


def as_chip(data, swap_off, swap_len=0x800):
    out = bytearray(data)
    for i in range(swap_off, min(swap_off + swap_len, len(out))):
        out[i] = swap_d0d1(out[i])
    return bytes(out)


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def write_c_header(path, blobs):
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write("// Generated by gen_hwtest.py — do not edit.\n")
        f.write("#pragma once\n#include <cstdint>\n#include <array>\n")
        f.write("namespace frogger { namespace hwtest {\n")
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
    """Landing-page thumbnail: original upright-marquee layout (black
    housing, backlit green plexi, the game's name). Not Konami/Sega art."""
    w, h = 286, 128
    pix = [[(14, 12, 10) for _ in range(w)] for _ in range(h)]

    def put(x, y, rgb):
        if 0 <= x < w and 0 <= y < h:
            pix[y][x] = rgb

    def lerp(a, b, t):
        return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

    x0, y0, x1, y1 = 8, 8, w - 8, h - 8
    for y in range(h):
        for x in range(w):
            in_lip = 5 <= x < w - 5 and 5 <= y < h - 5
            in_plexi = x0 <= x < x1 and y0 <= y < y1
            if in_plexi:
                ty = (y - y0) / (y1 - y0)
                tx = abs((x - w / 2) / (w / 2))
                base = lerp((80, 196, 72), (20, 96, 36), ty)
                hot = (200, 255, 180)
                pix[y][x] = lerp(base, hot, 0.28 * (1 - tx) * (1 - abs(ty - 0.35)))
            elif in_lip:
                pix[y][x] = (40, 120, 48) if (x == 5 or y == 5 or x == w - 6 or y == h - 6) else (16, 32, 16)

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

    text, scale, gap, ty = "FROGGER", 4, 3, 28
    fill, outline = (16, 80, 24), (0, 24, 8)
    cw = 8 * scale
    tw = len(text) * cw + (len(text) - 1) * gap
    tx = (w - tw) // 2
    for i, ch in enumerate(text):
        blit_char(ch, tx + i * (cw + gap), ty, scale, fill, outline)

    # Five lily-pad circles — original, not Konami sprites.
    pads = 5
    py = 98
    span = 160
    p0 = (w - span) // 2
    for i in range(pads):
        cx = p0 + i * (span // (pads - 1))
        for dy in range(-8, 9):
            for dx in range(-10, 11):
                if dx * dx / 100 + dy * dy / 64 <= 1:
                    put(cx + dx, py + dy, (32, 140, 48) if (dx + dy) & 1 else (20, 100, 36))

    rows = [bytearray(c for rgb in row for c in rgb) for row in pix]
    write_png(path, w, h, rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--header")
    ap.add_argument("--png")
    args = ap.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    s1 = pack_screen(*screen_crosshatch())
    s2 = pack_screen(*screen_color_bars())
    s3 = pack_screen(*screen_help())
    logical = {
        "program": assemble_main(s1, s2, s3),
        "sound": assemble_sound(),
        "gfx": gfx_rom(),
        "color_prom": color_prom(),
    }
    # Chip-order images: sound 608 (first 2K) and gfx 606 (second 2K) have
    # D0↔D1 swapped on the PCB. 607 is the high plane and is wired straight.
    blobs = {
        "program": logical["program"],
        "sound": as_chip(logical["sound"], 0),
        "gfx": as_chip(logical["gfx"], 0x800),
        "color_prom": logical["color_prom"],
    }
    names = {
        "program": "program.bin",
        "sound": "sound.bin",
        "gfx": "gfx.bin",
        "color_prom": "pr-91.6l",
    }
    for k, fname in names.items():
        with open(os.path.join(args.dir, fname), "wb") as f:
            f.write(blobs[k])
    # Loose-chip names a user zip might also use.
    for i, name in enumerate(("frogger.26", "frogger.27", "frsm3.7")):
        chunk = blobs["program"][i * 0x1000:(i + 1) * 0x1000]
        if i == 2:
            chunk = blobs["program"][0x2000:0x4000]
        with open(os.path.join(args.dir, name), "wb") as f:
            f.write(chunk)
    for i, name in enumerate(("frogger.608", "frogger.609", "frogger.610")):
        with open(os.path.join(args.dir, name), "wb") as f:
            f.write(blobs["sound"][i * 0x800:(i + 1) * 0x800])
    with open(os.path.join(args.dir, "frogger.607"), "wb") as f:
        f.write(blobs["gfx"][:0x800])
    with open(os.path.join(args.dir, "frogger.606"), "wb") as f:
        f.write(blobs["gfx"][0x800:])
    if args.header:
        write_c_header(args.header, blobs)
    if args.png:
        os.makedirs(os.path.dirname(os.path.abspath(args.png)) or ".", exist_ok=True)
        marquee_png(args.png)
    crcs = {names[k]: crc32(blobs[k]) for k in names}
    with open(os.path.join(args.dir, "crc.json"), "w") as f:
        json.dump({k: f"{v:08x}" for k, v in crcs.items()}, f)
    print("hwtest CRCs:")
    for k, v in crcs.items():
        print(f"  {k}: {v:08x}")


if __name__ == "__main__":
    main()
