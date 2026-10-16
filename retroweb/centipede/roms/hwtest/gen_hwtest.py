#!/usr/bin/env python3
"""Generate the Centipede hardware self-test ROM (not Atari game code).

Boots a short diagnostic (border / crosshair / boxes), then holds a help
screen whose 8×8 font is original — not a dump of Centipede's gfx ROMs —
explaining that Atari's ROMs are still under copyright and the user has to
load their own set. No Atari graphics or code.
"""

from __future__ import annotations

import argparse
import json
import zlib
from pathlib import Path

PROG_BASE = 0x2000
PROG_SIZE = 0x2000
VRAM_SIZE = 0x03C0  # 30×32 playfield
WAIT_IRQS = 90 * 16  # ~1.5 s (16 IRQs/frame × 90 frames)

# Upright tile grid after ROT270: 30 columns × 32 rows (240×256).
U_COLS, U_ROWS = 30, 32

# Original 8×8 caps. Bit 7 = left. Stored pre-rotated for ROT270.
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
    "'": (0x18, 0x18, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00),
}

# pen: 1=white, 2=red, 3=green (palette programmed below).
HELP_TITLE = "CENTIPEDE ARCADE"
HELP_TITLE_ROW = 3
HELP_LINES = [
    (3,  HELP_TITLE,            3),
    (6,  "SELF TEST OK",        1),
    (9,  "THIS PAGE DOES NOT",  1),
    (11, "INCLUDE ATARI'S",     1),
    (13, "CENTIPEDE ROMS",      1),
    (16, "THEY ARE STILL",      2),
    (18, "UNDER COPYRIGHT",     2),
    (21, "IF YOU OWN A",        1),
    (23, "ATARI CENTIPEDE SET", 1),
    (25, "LOAD IT YOURSELF",    3),
    (28, "IT STAYS IN THIS",    1),
    (30, "BROWSER ONLY",        1),
]


def center_col(text: str) -> int:
    return max(0, (U_COLS - len(text)) // 2)


def upright_to_machine(ucol: int, urow: int) -> tuple[int, int]:
    """Upright (ucol, urow) → machine (tx, ty) under ROT270."""
    return 31 - urow, ucol


def build_char_map(lines) -> dict[tuple[str, int], int]:
    """Map (char, pen) → VRAM tile code. Codes 0x01–0x03 are diagnostic."""
    m: dict[tuple[str, int], int] = {}
    next_code = 0x04
    for _, text, pen in lines:
        for ch in text.upper():
            key = (ch, pen)
            if key in m:
                continue
            if ch not in FONT:
                raise ValueError(f"missing font glyph for {ch!r}")
            if next_code > 0x3F:
                raise ValueError("tile code space exhausted")
            m[key] = next_code
            next_code += 1
    return m


def put_upright(vram: bytearray, char_map, ucol: int, urow: int, ch: str, pen: int) -> None:
    if not (0 <= ucol < U_COLS and 0 <= urow < U_ROWS):
        return
    tx, ty = upright_to_machine(ucol, urow)
    if not (0 <= tx < 32 and 0 <= ty < 30):
        return
    vram[ty * 32 + tx] = char_map[(ch.upper(), pen)] & 0x3F


def screen_diagnostic() -> bytearray:
    vram = bytearray(VRAM_SIZE)
    for x in range(32):
        vram[0 * 32 + x] = 0x01
        vram[29 * 32 + x] = 0x01
    for y in range(30):
        vram[y * 32 + 0] = 0x01
        vram[y * 32 + 31] = 0x01
    for x in range(1, 31):
        vram[15 * 32 + x] = 0x02
    for y in range(1, 29):
        vram[y * 32 + 16] = 0x02
    for size, tile in ((10, 0x01), (6, 0x02), (2, 0x03)):
        x0, y0 = 16 - size // 2, 15 - size // 2
        for i in range(size):
            vram[y0 * 32 + x0 + i] = tile
            vram[(y0 + size - 1) * 32 + x0 + i] = tile
            vram[(y0 + i) * 32 + x0] = tile
            vram[(y0 + i) * 32 + x0 + size - 1] = tile
    for i, tile in enumerate((0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02)):
        vram[27 * 32 + 12 + i] = tile
    return vram


def screen_help(char_map) -> bytearray:
    vram = bytearray(VRAM_SIZE)
    for urow, text, pen in HELP_LINES:
        uc0 = center_col(text)
        for i, ch in enumerate(text.upper()):
            put_upright(vram, char_map, uc0 + i, urow, ch, pen)
    return vram


def build_gfx(char_map) -> bytes:
    gfx = bytearray(0x1000)

    def set_char(code: int, rows_p0: list[int], rows_p1: list[int]) -> None:
        for y in range(8):
            gfx[code * 8 + y] = rows_p0[y]
            gfx[0x800 + code * 8 + y] = rows_p1[y]

    def set_sprite(code: int, rows_p0: list[int], rows_p1: list[int]) -> None:
        for y in range(16):
            gfx[code * 16 + y] = rows_p0[y]
            gfx[0x800 + code * 16 + y] = rows_p1[y]

    # VRAM codes map to gfx index code+$40.
    set_char(0x41, [0xFF] * 8, [0x00] * 8)
    set_char(0x42, [0xAA, 0x55] * 4, [0x00] * 8)
    set_char(0x43, [0x00] * 8, [0xFF] * 8)
    set_sprite(1, [0xFF] * 16, [0x00] * 16)

    for (ch, pen), vcode in char_map.items():
        rows = FONT[ch]
        p0 = [0] * 8
        p1 = [0] * 8
        for ly, bits in enumerate(rows):
            for lx in range(8):
                if not (bits & (0x80 >> lx)):
                    continue
                # Pre-rotate for ROT270: upright (lx,ly) → machine (7-ly, lx).
                px, py = 7 - ly, lx
                if pen == 1:
                    p0[py] |= 0x80 >> px
                elif pen == 2:
                    p1[py] |= 0x80 >> px
                else:
                    p0[py] |= 0x80 >> px
                    p1[py] |= 0x80 >> px
        set_char(0x40 + vcode, p0, p1)
    return bytes(gfx)


def build_program(diag: bytes, help_scr: bytes) -> bytes:
    prog = bytearray(PROG_SIZE)
    pc = 0

    def org() -> int:
        return PROG_BASE + pc

    def put(*bs: int) -> None:
        nonlocal pc
        for b in bs:
            prog[pc] = b & 0xFF
            pc += 1

    def put_jmp(target: int) -> None:
        put(0x4C, target & 0xFF, (target >> 8) & 0xFF)

    def put_jsr(target: int) -> None:
        put(0x20, target & 0xFF, (target >> 8) & 0xFF)

    def patch16(at: int, value: int) -> None:
        prog[at - PROG_BASE] = value & 0xFF
        prog[at - PROG_BASE + 1] = (value >> 8) & 0xFF

    for i, ch in enumerate(b"CEN1"):
        put(0xA9, ch)
        put(0x8D, i, 0x00)

    # Palette: pen0 black, pen1 white, pen2 red, pen3 green.
    for addr, val in (
        (0x1404, 0x0F),
        (0x1405, 0x08),
        (0x1406, 0x0E),
        (0x1407, 0x0D),
        (0x140C, 0x08),
        (0x140D, 0x0E),
    ):
        put(0xA9, val)
        put(0x8D, addr & 0xFF, addr >> 8)

    put(0xA9, 0x00)
    put(0x85, 0x30)  # IRQ counter lo (keep $00–$03 = CEN1)
    put(0x85, 0x31)
    put(0x58)  # CLI, IRQ bumps $30/$31 for the wait

    put_jsr(0)
    jsr_diag_at = org() - 2
    for a, v in ((0x07C0, 0x02), (0x07D0, 0x70), (0x07E0, 0x78), (0x07F0, 0x01)):
        put(0xA9, v)
        put(0x8D, a & 0xFF, a >> 8)
    put_jsr(0)
    jsr_wait_at = org() - 2
    put_jsr(0)
    jsr_hide_at = org() - 2
    put_jsr(0)
    jsr_help_at = org() - 2

    loop = org()
    put(0x8D, 0x00, 0x20)
    put(0x8D, 0x00, 0x18)
    put_jmp(loop)

    irq = org()
    put(0xE6, 0x30)
    put(0xD0, 0x02)
    put(0xE6, 0x31)
    put(0x8D, 0x00, 0x20)
    put(0x8D, 0x00, 0x18)
    put(0x40)

    # copy_screen: $20/$21 src, $22/$23 dst=$0400, $24/$25 count.
    copy_screen = org()
    put(0xA9, 0x00)
    put(0x85, 0x22)
    put(0xA9, 0x04)
    put(0x85, 0x23)
    put(0xA0, 0x00)
    cs = org()
    put(0xB1, 0x20)          # LDA (src),Y
    put(0x91, 0x22)          # STA (dst),Y
    put(0xC8)                # INY
    put(0xD0, 0x04)          # BNE +4
    put(0xE6, 0x21)          # INC src_hi
    put(0xE6, 0x23)          # INC dst_hi
    put(0xA5, 0x24)          # LDA count_lo
    put(0xD0, 0x02)          # BNE +2
    put(0xC6, 0x25)          # DEC count_hi
    put(0xC6, 0x24)          # DEC count_lo
    put(0xA5, 0x24)
    put(0x05, 0x25)          # ORA count_hi
    put(0xD0, (cs - (org() + 2)) & 0xFF)
    put(0x60)

    def emit_copy_thunk(data_addr: int) -> int:
        thunk = org()
        put(0xA9, data_addr & 0xFF)
        put(0x85, 0x20)
        put(0xA9, (data_addr >> 8) & 0xFF)
        put(0x85, 0x21)
        put(0xA9, VRAM_SIZE & 0xFF)
        put(0x85, 0x24)
        put(0xA9, (VRAM_SIZE >> 8) & 0xFF)
        put(0x85, 0x25)
        put_jsr(copy_screen)
        put(0x60)
        return thunk

    wait_irqs = org()
    wloop = org()
    put(0x8D, 0x00, 0x20)
    put(0x8D, 0x00, 0x18)
    put(0xA5, 0x31)
    put(0xC9, (WAIT_IRQS >> 8) & 0xFF)
    put(0x90, (wloop - (org() + 2)) & 0xFF)  # hi < target
    put(0xD0, 0x07)  # hi > target → RTS
    put(0xA5, 0x30)
    put(0xC9, WAIT_IRQS & 0xFF)
    put(0x90, (wloop - (org() + 2)) & 0xFF)  # lo < target
    put(0x60)

    hide_sprite = org()
    put(0xA9, 0x00)
    for a in (0x07C0, 0x07D0, 0x07E0, 0x07F0):
        put(0x8D, a & 0xFF, a >> 8)
    put(0x60)

    while pc & 0xFF:
        put(0x00)
    diag_addr = org()
    prog[pc : pc + VRAM_SIZE] = diag
    pc += VRAM_SIZE
    while pc & 0xFF:
        put(0x00)
    help_addr = org()
    prog[pc : pc + VRAM_SIZE] = help_scr
    pc += VRAM_SIZE

    copy_diag = emit_copy_thunk(diag_addr)
    copy_help = emit_copy_thunk(help_addr)

    patch16(jsr_diag_at, copy_diag)
    patch16(jsr_wait_at, wait_irqs)
    patch16(jsr_hide_at, hide_sprite)
    patch16(jsr_help_at, copy_help)

    assert pc < 0x1FFA, f"program too large: {pc:#x}"
    prog[0x1FFA] = irq & 0xFF
    prog[0x1FFB] = (irq >> 8) & 0xFF
    prog[0x1FFC] = 0x00
    prog[0x1FFD] = 0x20
    prog[0x1FFE] = irq & 0xFF
    prog[0x1FFF] = (irq >> 8) & 0xFF
    return bytes(prog)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def write_header(path: Path, program: bytes, gfx: bytes, char_map) -> None:
    def arr(name: str, data: bytes) -> str:
        body = ", ".join(f"0x{b:02x}" for b in data)
        return (
            f"inline constexpr std::array<uint8_t, {len(data)}> {name} = {{\n"
            f"    {body}\n}};\n"
        )

    # Probe: upright title row → machine (tx, ty) under ROT270.
    title = HELP_TITLE
    title_pen = next(p for r, t, p in HELP_LINES if t == HELP_TITLE)
    uc0 = center_col(title)
    title_codes = [char_map[(ch, title_pen)] for ch in title]
    # First char at (uc0, HELP_TITLE_ROW) → (tx, ty)
    tx0, ty0 = upright_to_machine(uc0, HELP_TITLE_ROW)
    # Along the title, ucol increases → ty increases, tx fixed.
    title_offs = [ty0 * 32 + tx0 + i * 32 for i in range(len(title))]

    copy_line = "UNDER COPYRIGHT"
    copy_pen = next(p for _, t, p in HELP_LINES if t == copy_line)
    copy_row = next(r for r, t, _ in HELP_LINES if t == copy_line)
    cc0 = center_col(copy_line)
    ctx, cty = upright_to_machine(cc0, copy_row)
    copy_codes = [char_map[(ch, copy_pen)] for ch in copy_line]
    copy_offs = [cty * 32 + ctx + i * 32 for i in range(len(copy_line))]

    def u8arr(name: str, vals) -> str:
        body = ", ".join(str(v) for v in vals)
        return f"inline constexpr std::array<uint8_t, {len(vals)}> {name} = {{{body}}};\n"

    def iarr(name: str, vals) -> str:
        body = ", ".join(str(v) for v in vals)
        return f"inline constexpr std::array<int, {len(vals)}> {name} = {{{body}}};\n"

    text = (
        "#ifndef CENTIPEDE_HWTEST_ROMS_H\n"
        "#define CENTIPEDE_HWTEST_ROMS_H\n\n"
        "#include <array>\n"
        "#include <cstdint>\n\n"
        "namespace centipede::hwtest {\n\n"
        f"{arr('program', program)}\n"
        f"{arr('gfx', gfx)}\n"
        f"{u8arr('help_title_codes', title_codes)}\n"
        f"{iarr('help_title_offs', title_offs)}\n"
        f"{u8arr('help_copy_codes', copy_codes)}\n"
        f"{iarr('help_copy_offs', copy_offs)}\n"
        f"inline constexpr int kHelpWaitFrames = 100;\n"
        "}  // namespace centipede::hwtest\n\n"
        "#endif\n"
    )
    path.write_text(text)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, required=True)
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--png", type=Path, default=None)
    args = ap.parse_args()

    char_map = build_char_map(HELP_LINES)
    diag = screen_diagnostic()
    help_scr = screen_help(char_map)
    program = build_program(diag, help_scr)
    gfx = build_gfx(char_map)

    args.dir.mkdir(parents=True, exist_ok=True)
    args.header.parent.mkdir(parents=True, exist_ok=True)

    (args.dir / "program.bin").write_bytes(program)
    (args.dir / "gfx.bin").write_bytes(gfx)

    chips = {
        "136001-307.d1": program[0x0000:0x0800],
        "136001-308.e1": program[0x0800:0x1000],
        "136001-309.fh1": program[0x1000:0x1800],
        "136001-310.j1": program[0x1800:0x2000],
        "136001-211.f7": gfx[0x0000:0x0800],
        "136001-212.hj7": gfx[0x0800:0x1000],
    }
    for name, data in chips.items():
        (args.dir / name).write_bytes(data)

    crc = {
        "kind": "hwtest",
        "program": f"{crc32(program):08x}",
        "gfx": f"{crc32(gfx):08x}",
        "chips": {n: f"{crc32(d):08x}" for n, d in chips.items()},
    }
    (args.dir / "crc.json").write_text(json.dumps(crc, indent=2) + "\n")
    write_header(args.header, program, gfx, char_map)
    print(f"wrote {args.dir} and {args.header} ({len(char_map)} glyphs)")


if __name__ == "__main__":
    main()
