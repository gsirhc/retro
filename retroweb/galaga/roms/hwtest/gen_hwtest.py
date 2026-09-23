#!/usr/bin/env python3
"""Assemble the Galaga board hardware self-test ROM (not Midway Galaga).

Three Z80 images: main paints a help screen and kicks the watchdog, sub and
sound write a shared-RAM signature once the main CPU releases their reset.
No Midway code or graphics.
"""
import argparse, json, os, struct, zlib

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
}

LINES = [
    (2, "GALAGA ARCADE"),
    (5, "SELF TEST OK"),
    (8, "THIS PAGE DOES NOT"),
    (10, "INCLUDE MIDWAYS"),
    (12, "GALAGA ROMS"),
    (15, "THEY ARE STILL"),
    (17, "UNDER COPYRIGHT"),
    (20, "IF YOU OWN A SET"),
    (22, "LOAD IT YOURSELF"),
    (25, "IT STAYS IN THIS"),
    (27, "BROWSER ONLY"),
]


def set_pix(tile, x, y):
    bi = (y + 8) if x < 4 else y
    xb = x if x < 4 else x - 4
    tile[bi] |= (1 << (7 - xb)) | (1 << (3 - xb))


def glyph(rows):
    # Font rows are upright. Video::render's ROT90 (FLIP_X | SWAP_XY) maps
    # native (px, py) to upright (right-to-left in py, downward in px), so
    # a font pixel (fx, fy) is stored at native (fy, 7-fx).
    tile = bytearray(16)
    for fy, bits in enumerate(rows):
        for fx in range(8):
            if bits & (0x80 >> fx):
                set_pix(tile, fy, 7 - fx)
    return tile


def gfx():
    rom = bytearray(0x1000)
    solid = bytearray(16)
    for y in range(8):
        for x in range(8):
            set_pix(solid, x, y)
    rom[0x40 * 16:0x40 * 16 + 16] = solid
    chars = " ABCDEFGHIJKLMNOPQRSTUVWXYZ.-0123456789"
    for i, ch in enumerate(chars):
        if ch == " ":
            continue
        code = i  # space is 0
        if ch != " " and code == 0:
            continue
        rom[code * 16:(code + 1) * 16] = glyph(FONT[ch])
    # Index 0 stays blank. Letters use their index in `chars`.
    return rom, {ch: i for i, ch in enumerate(chars)}


def place(codes, colors, mapping):
    for urow, text in LINES:
        ucol0 = max(0, (28 - len(text)) // 2)
        for i, ch in enumerate(text):
            offs = (29 - (ucol0 + i)) * 32 + (urow - 2)
            codes[offs] = mapping.get(ch, 0)
            colors[offs] = 1


def assemble_main():
    mem = bytearray(0x4000)
    # 0000 JP start, 0038 IRQ, 0066 NMI
    mem[0:3] = bytes([0xC3, 0x80, 0x00])
    # IRQ: inc $8804, kick watchdog, EI, RETI
    irq = bytes([
        0x21, 0x04, 0x88,       # LD HL,$8804
        0x34,                   # INC (HL)
        0x32, 0x30, 0x68,       # LD ($6830),A
        0xFB,                   # EI
        0xED, 0x4D,             # RETI
    ])
    mem[0x38:0x38 + len(irq)] = irq
    mem[0x66:0x68] = bytes([0xED, 0x45])  # RETN
    p = 0x80
    def b(*xs):
        nonlocal p
        for x in xs:
            mem[p] = x & 0xFF
            p += 1
    def fill(dest, value, pages):
        # B = pages, C counts 256. DEC rr does not set flags; DEC r does.
        b(0x21, dest & 0xFF, dest >> 8)  # LD HL,dest
        b(0x06, pages)                   # LD B,pages
        b(0x0E, 0x00)                    # LD C,0
        b(0x3E, value)                   # LD A,value
        loop = p
        b(0x77)                          # LD (HL),A
        b(0x23)                          # INC HL
        b(0x0D)                          # DEC C
        b(0x20, (loop - (p + 1)) & 0xFF) # JR NZ,loop
        b(0x05)                          # DEC B
        b(0x20, (loop - (p + 1)) & 0xFF)
    b(0xF3)                  # DI
    b(0xED, 0x56)            # IM 1
    b(0x31, 0x00, 0x8B)      # LD SP,$8B00
    b(0x3E, ord("T")); b(0x32, 0x00, 0x88)
    b(0x3E, ord("S")); b(0x32, 0x01, 0x88)
    b(0x3E, ord("T")); b(0x32, 0x02, 0x88)
    b(0x3E, ord("1")); b(0x32, 0x03, 0x88)
    # !NMION is active-low enable: write 1 to keep the sound NMI off,
    # then release sub + sound + the custom chips.
    b(0x3E, 0x01)
    b(0x32, 0x20, 0x68)      # IRQ1 enable
    b(0x32, 0x22, 0x68)      # NMI off
    b(0x32, 0x23, 0x68)      # release reset
    fill(0x8000, 1, 4)       # tile codes: solid
    # Text is patched into the ROM image after assemble, not by this loop.
    fill(0x8400, 1, 4)       # color
    b(0x3E, 0x01)
    b(0x32, 0x00, 0xA0)      # starfield speed latch
    # 06XX: write mode chip 0, command 05 (switch mode), read one byte, idle.
    b(0x3E, 0xA1); b(0x32, 0x00, 0x71)
    b(0x3E, 0x05); b(0x32, 0x00, 0x70)
    b(0x3E, 0x71); b(0x32, 0x00, 0x71)
    b(0x3A, 0x00, 0x70)      # LD A,($7000)
    b(0x32, 0x0A, 0x88)      # LD ($880A),A
    b(0x3E, 0x10); b(0x32, 0x00, 0x71)
    b(0xFB)                  # EI
    loop = p
    b(0x32, 0x30, 0x68)      # watchdog
    b(0x18, (loop - (p + 1)) & 0xFF)
    return mem


def cpu_stub(letter, addr):
    mem = bytearray(0x1000)
    # LD A,letter / LD (addr),A / HALT
    mem[0:8] = bytes([
        0x3E, ord(letter),
        0x32, addr & 0xFF, addr >> 8,
        0x76,
        0x18, 0xFE,
    ])
    return mem


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def write_c_header(path, blobs):
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write("// Generated by gen_hwtest.py — do not edit.\n")
        f.write("#pragma once\n#include <cstdint>\n#include <array>\n")
        f.write("namespace galaga { namespace hwtest {\n")
        for name, data in blobs.items():
            f.write(f"inline const std::array<uint8_t, {len(data)}> {name} = {{")
            f.write(",".join(str(b) for b in data))
            f.write("};\n")
        f.write("}} // namespace\n")


def marquee_png(path):
    import sys
    shared = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "shared"))
    if shared not in sys.path:
        sys.path.insert(0, shared)
    from marquee import render_marquee
    render_marquee(path, "galaga")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--header")
    ap.add_argument("--png")
    args = ap.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    tiles, mapping = gfx()
    codes = bytearray([1]) * 0x400
    colors = bytearray([1]) * 0x400
    place(codes, colors, mapping)
    # The CPU fills RAM itself; the tile ROM is what it indexes.
    # Pre-bake nothing into program RAM. Help text is drawn by... the CPU
    # fill is solid tile 1. Overlay text by encoding a small table the CPU
    # does not write — so paint the text into a second path: the generator
    # also emits the screen as part of the tile ROM only.
    # The running CPU overwrites videoram with tile 1. Patch the CPU fill
    # value? Text would be lost.
    # Instead, leave videoram fill as tile 1 and accept a solid screen, and
    # also store the help layout in the header for a unit test that pokes it.
    # The page should show the help text, so the CPU must write the text.
    # Emit the text as immediate stores after the fill. Rebuild main with
    # those stores appended by patching assemble... done below via a second
    # pass that is already in assemble? Not yet.
    main_rom = assemble_main()
    # Insert is too late if the fill already happened at runtime. Add the
    # text stores by appending them into a hole: rewrite is easier in
    # assemble. For now the solid tile still paints; text bytes are written
    # by a trailing table copied in assemble_text() below.
    main_rom = assemble_with_text(mapping)
    blobs = {
        "main": main_rom,
        "sub": cpu_stub("S", 0x8808),
        "sound": cpu_stub("N", 0x8809),
        "tiles": tiles,
        "sprites": bytes(0x2000),
        # Character pens OR 0x10 into the PROM index, so the upper half
        # mirrors the low half for the self-test colours.
        "palette": bytes([0, 0x3F, 0xFF]) + bytes(13) + bytes([0, 0x3F, 0xFF]) + bytes(13),
        "char_lut": bytes([0] * 7 + [1, 0, 0, 0, 2]) + bytes(244),
        "sprite_lut": bytes(256),
        "wave": bytes([(i & 15) for i in range(256)]),
    }
    names = {
        "main": "main.bin",
        "sub": "sub.bin",
        "sound": "sound.bin",
        "tiles": "2600j.bin",
        "sprites": "sprites.bin",
        "palette": "prom-5.5n",
        "char_lut": "prom-4.2n",
        "sprite_lut": "prom-3.1c",
        "wave": "prom-1.1d",
    }
    for k, fname in names.items():
        with open(os.path.join(args.dir, fname), "wb") as f:
            f.write(blobs[k])
    if args.header:
        write_c_header(args.header, blobs)
    if args.png:
        os.makedirs(os.path.dirname(os.path.abspath(args.png)) or ".", exist_ok=True)
        marquee_png(args.png)
    with open(os.path.join(args.dir, "crc.json"), "w") as f:
        json.dump({names[k]: f"{crc32(blobs[k]):08x}" for k in names}, f)
    print("hwtest CRCs:")
    for k, fname in names.items():
        print(f"  {fname}: {crc32(blobs[k]):08x}")


def assemble_with_text(mapping):
    mem = assemble_main()
    # The fill loop ends and then star/WSG/06XX/EI run. Text stores need to
    # happen after the fill. Easiest correct approach: the fill writes tile 1,
    # then a generated list of LD (addr),A for each glyph overwrites cells.
    # Find the EI (0xFB) that precedes the watchdog loop and insert before it.
    # assemble_main's EI is the last 0xFB before the JR loop. Inserting shifts
    # the JR displacement, so rebuild with an explicit text phase instead.
    return _build(mapping)


def _build(mapping):
    mem = bytearray(0x4000)
    mem[0:3] = bytes([0xC3, 0x80, 0x00])
    irq = bytes([0x21, 0x04, 0x88, 0x34, 0x32, 0x30, 0x68, 0xFB, 0xED, 0x4D])
    mem[0x38:0x38 + len(irq)] = irq
    mem[0x66:0x68] = bytes([0xED, 0x45])
    p = 0x80

    def b(*xs):
        nonlocal p
        for x in xs:
            mem[p] = x & 0xFF
            p += 1

    def fill(dest, value, pages):
        b(0x21, dest & 0xFF, dest >> 8)
        b(0x06, pages)
        b(0x0E, 0x00)
        b(0x3E, value)
        loop = p
        b(0x77, 0x23, 0x0D)
        b(0x20, (loop - (p + 1)) & 0xFF)
        b(0x05)
        b(0x20, (loop - (p + 1)) & 0xFF)

    b(0xF3, 0xED, 0x56, 0x31, 0x00, 0x8B)
    b(0x3E, ord("T")); b(0x32, 0x00, 0x88)
    b(0x3E, ord("S")); b(0x32, 0x01, 0x88)
    b(0x3E, ord("T")); b(0x32, 0x02, 0x88)
    b(0x3E, ord("1")); b(0x32, 0x03, 0x88)
    b(0x3E, 0x01)
    b(0x32, 0x20, 0x68)
    b(0x32, 0x22, 0x68)
    b(0x32, 0x23, 0x68)
    fill(0x8000, 0, 4)
    fill(0x8400, 0, 4)
    # Playfield address is (screen_row+2)*32 + (screen_col-2). Upright
    # column ucol is memory row 29-ucol; upright row urow is memory
    # column urow-2.
    for idx, (urow, text) in enumerate(LINES):
        ucol0 = max(0, (28 - len(text)) // 2)
        color = 1 if idx == 0 else 2
        for i, ch in enumerate(text):
            code = mapping.get(ch, 0)
            nrow = 29 - (ucol0 + i)
            ncol = urow - 2
            addr = 0x8000 + nrow * 32 + ncol
            b(0x3E, code)
            b(0x32, addr & 0xFF, (addr >> 8) & 0xFF)
            b(0x3E, color)
            b(0x32, (0x8400 + (addr - 0x8000)) & 0xFF,
              ((0x8400 + (addr - 0x8000)) >> 8) & 0xFF)
    b(0x3E, 0x01)
    b(0x32, 0x00, 0xA0)
    b(0x3E, 0xA1); b(0x32, 0x00, 0x71)
    b(0x3E, 0x05); b(0x32, 0x00, 0x70)
    b(0x3E, 0x71); b(0x32, 0x00, 0x71)
    b(0x3A, 0x00, 0x70)
    b(0x32, 0x0A, 0x88)
    b(0x3E, 0x10); b(0x32, 0x00, 0x71)
    b(0xFB)
    loop = p
    b(0x32, 0x30, 0x68)
    b(0x18, (loop - (p + 1)) & 0xFF)
    if p >= 0x4000:
        raise SystemExit(f"main ROM overflow at ${p:04X}")
    return mem


if __name__ == "__main__":
    main()
