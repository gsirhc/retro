#!/usr/bin/env python3
"""Generate the Missile Command hardware self-test ROM (not Atari game code).

Boots a short diagnostic (palette, bars, border, MADSEL crosshair, third-bit
strip), then holds a help screen drawn from an original 8x8 font explaining
that Atari's ROMs are still under copyright and the user has to load their
own set. No Atari graphics or code.
"""

from __future__ import annotations

import argparse
import json
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "shared"))
from hwtest_font import font  # noqa: E402

PROG_BASE = 0x5000
PROG_SIZE = 0x3000
INITIALS_ROM = 0x74F3
SCORES_ROM = 0x750B
DATA_BASE = 0x7523
DIAG_FRAMES = 90
WAIT_IRQS = DIAG_FRAMES * 4  # 4 IRQs per frame
HELP_WAIT_FRAMES = 150
TEXT_TOP = 32
PROBE_ROW = 3

PEN_WHITE, PEN_RED, PEN_GREEN = 2, 4, 6

# Palette bits 3,2,1 = R,G,B, active low.
PALETTE = (0x0E, 0x0C, 0x00, 0x08, 0x06, 0x02, 0x0A, 0x04)

INITIALS = b"HWTROMRAMCPUVIDSNDIRQBUS"
SCORES = b"".join(bytes([0x00, d << 4, 0x00]) for d in range(1, 9))

FONT = font(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.'")

HELP_TITLE = "MISSILE COMMAND"
HELP_COPY = "UNDER COPYRIGHT"
HELP_LINES = [
    (3,  HELP_TITLE,             PEN_GREEN),
    (6,  "SELF TEST OK",         PEN_WHITE),
    (9,  "THIS PAGE DOES NOT",   PEN_WHITE),
    (11, "INCLUDE ATARI'S",      PEN_WHITE),
    (13, "MISSILE COMMAND ROMS", PEN_WHITE),
    (16, "THEY ARE STILL",       PEN_RED),
    (18, HELP_COPY,              PEN_RED),
    (21, "IF YOU OWN A",         PEN_WHITE),
    (23, "MISSILE COMMAND SET",  PEN_WHITE),
    (25, "LOAD IT YOURSELF",     PEN_GREEN),
    (28, "IT STAYS IN THIS",     PEN_WHITE),
    (30, "BROWSER ONLY",         PEN_WHITE),
]

COLS = 32


def line_y(row: int) -> int:
    return TEXT_TOP + (row - 3) * 8


def line_xb(text: str) -> int:
    return (COLS - len(text)) // 2 * 2


def glyph_bytes(ch: str, pen: int) -> bytes:
    out = bytearray()
    for bits in FONT[ch]:
        for half in (0, 4):
            v = 0
            for p in range(4):
                if bits & (0x80 >> (half + p)):
                    if pen & 4:
                        v |= 1 << (4 + p)
                    if pen & 2:
                        v |= 1 << p
            out.append(v)
    return bytes(out)


class Data:
    def __init__(self, base: int) -> None:
        keys: list[tuple[str, int]] = [(" ", 0)]
        for _, text, pen in HELP_LINES:
            for ch in text:
                key = (ch, 0 if ch == " " else pen)
                if ch not in FONT:
                    raise ValueError(f"missing font glyph for {ch!r}")
                if key not in keys:
                    keys.append(key)
        self.keys = keys
        self.glyphs = {k: glyph_bytes(*k) for k in keys}
        n = len(keys)
        self.glyph_addr = {k: base + 16 * i for i, k in enumerate(keys)}
        self.glo = base + 16 * n
        self.ghi = self.glo + n
        pos = self.ghi + n
        self.str_addr: dict[str, int] = {}
        self.strings = bytearray()
        for _, text, _pen in HELP_LINES:
            self.str_addr[text] = pos + len(self.strings)
            for ch in text:
                pen = next(p for _, t, p in HELP_LINES if t == text)
                self.strings.append(keys.index((ch, 0 if ch == " " else pen)))
            self.strings.append(0xFF)
        self.base = base
        self.end = pos + len(self.strings)

    def blob(self) -> bytes:
        out = bytearray()
        for k in self.keys:
            out += self.glyphs[k]
        out += bytes(self.glyph_addr[k] & 0xFF for k in self.keys)
        out += bytes(self.glyph_addr[k] >> 8 for k in self.keys)
        out += self.strings
        return bytes(out)


class Asm:
    def __init__(self) -> None:
        self.prog = bytearray(PROG_SIZE)
        self.pc = 0
        self.labels: dict[str, int] = {}
        self.fix: list[tuple[int, str, str]] = []

    def org(self) -> int:
        return PROG_BASE + self.pc

    def put(self, *bs: int) -> None:
        for b in bs:
            self.prog[self.pc] = b & 0xFF
            self.pc += 1

    def label(self, name: str) -> None:
        self.labels[name] = self.org()

    def abs_ref(self, op: int, name: str) -> None:
        self.put(op, 0, 0)
        self.fix.append((self.pc - 2, name, "abs"))

    def jsr(self, name: str) -> None:
        self.abs_ref(0x20, name)

    def jmp(self, name: str) -> None:
        self.abs_ref(0x4C, name)

    def br(self, op: int, name: str) -> None:
        self.put(op, 0)
        self.fix.append((self.pc - 1, name, "rel"))

    def resolve(self) -> None:
        for at, name, kind in self.fix:
            t = self.labels[name]
            if kind == "abs":
                self.prog[at] = t & 0xFF
                self.prog[at + 1] = t >> 8
            else:
                d = t - (PROG_BASE + at + 1)
                assert -128 <= d <= 127, name
                self.prog[at] = d & 0xFF


# 6502 opcodes. None of the plain-flow ones have (op & 0x1F) == 1, which
# would arm MADSEL.
SEI, CLI, CLD, TXS, TAX, TAY, PHA, PLA, RTI, RTS, CLC = (
    0x78, 0x58, 0xD8, 0x9A, 0xAA, 0xA8, 0x48, 0x68, 0x40, 0x60, 0x18)
INX, INY, DEY, LSR, ASL = 0xE8, 0xC8, 0x88, 0x4A, 0x0A
LDA_I, LDX_I, LDY_I, CMP_I, CPX_I = 0xA9, 0xA2, 0xA0, 0xC9, 0xE0
LDA_Z, STA_Z, STX_Z, STY_Z, LDY_Z = 0xA5, 0x85, 0x86, 0x84, 0xA4
INC_Z, DEC_Z, ADC_Z = 0xE6, 0xC6, 0x65
AND_I = 0x29
STA_A, LDA_AX = 0x8D, 0xBD
STA_ZX, STA_IY, LDA_IY = 0x95, 0x91, 0xB1
STA_INDX = 0x81
BNE, BEQ, BCC, BPL = 0xD0, 0xF0, 0x90, 0x10

Z_Y, Z_ROWS, Z_XB, Z_N, Z_VAL = 0x82, 0x83, 0x84, 0x85, 0x86
Z_GLYPH, Z_CUR, Z_SCOL, Z_GIDX = 0x88, 0x8B, 0x8E, 0x8F
Z_PTR = 0x80
Z_STR = 0x8C
Z_PX, Z_PY = 0xA0, 0xA1
Z_IRQ_LO, Z_IRQ_HI, Z_DONE = 0xF4, 0xF5, 0xF6

PROBE_X, PROBE_Y = 64, 128


def build_program(data: Data) -> bytes:
    a = Asm()
    put = a.put

    put(SEI, CLD, LDX_I, 0xFF, TXS)
    put(LDA_I, 0x00, STA_A, 0x00, 0x4D)
    for i, ch in enumerate(b"MIS1"):
        put(LDA_I, ch, STA_Z, 0xF0 + i)
    put(LDA_I, 0x00, STA_Z, Z_IRQ_LO, STA_Z, Z_IRQ_HI, STA_Z, Z_DONE)

    put(LDA_I, 0x03, STA_A, 0x0F, 0x40)
    put(LDA_I, 0x00)
    for r in (0x01, 0x03, 0x05, 0x07):
        put(STA_A, r, 0x40)

    for pen, v in enumerate(PALETTE):
        put(LDA_I, v, STA_A, pen, 0x4B)

    put(LDX_I, 23)
    a.label("hs_copy")
    put(LDA_AX, INITIALS_ROM & 0xFF, INITIALS_ROM >> 8, STA_ZX, 0x2C)
    put(LDA_AX, SCORES_ROM & 0xFF, SCORES_ROM >> 8, STA_ZX, 0x44)
    put(0xCA)  # DEX
    a.br(BPL, "hs_copy")

    put(LDA_I, 0x00, STA_A, 0x00, 0x4D, CLI)

    def fill(y: int, rows: int, xb: int, n: int, val: int) -> None:
        for z, v in ((Z_Y, y), (Z_ROWS, rows), (Z_XB, xb), (Z_N, n), (Z_VAL, val)):
            put(LDA_I, v, STA_Z, z)
        a.jsr("fill")

    fill(26, 2, 0, 64, 0x0F)
    for i, v in enumerate((0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0)):
        fill(40, 32, 8 * i, 8, v)
    fill(28, 224, 0, 1, 0x03)
    fill(28, 224, 63, 1, 0x0C)
    fill(252, 2, 0, 64, 0x0F)

    # MADSEL crosshair, pen 6.
    put(LDA_I, PROBE_Y, STA_Z, Z_PY, LDA_I, 0x00, STA_Z, Z_PX)
    a.label("hline")
    put(LDX_I, 0x00, LDA_I, 0xC0, STA_INDX, Z_PX, INC_Z, Z_PX)
    a.br(BNE, "hline")
    put(LDA_I, 128, STA_Z, Z_PX, LDA_I, 25, STA_Z, Z_PY)
    a.label("vline")
    put(LDX_I, 0x00, LDA_I, 0xC0, STA_INDX, Z_PX, INC_Z, Z_PY)
    a.br(BNE, "vline")

    # Eight-pen strip through MADSEL, lines 236-239 use the third-bit plane.
    put(LDA_I, 236, STA_Z, Z_PY)
    a.label("strip_y")
    put(LDA_I, 0x00, STA_Z, Z_PX)
    a.label("strip_x")
    put(LDA_Z, Z_PX, AND_I, 0xE0, LDX_I, 0x00, STA_INDX, Z_PX, INC_Z, Z_PX)
    a.br(BNE, "strip_x")
    put(INC_Z, Z_PY, LDA_Z, Z_PY, CMP_I, 240)
    a.br(BNE, "strip_y")

    a.label("wait")
    put(LDA_Z, Z_IRQ_HI, CMP_I, WAIT_IRQS >> 8)
    a.br(BCC, "wait")
    a.br(BNE, "waited")
    put(LDA_Z, Z_IRQ_LO, CMP_I, WAIT_IRQS & 0xFF)
    a.br(BCC, "wait")
    a.label("waited")

    a.jsr("clear")
    for row, text, _pen in HELP_LINES:
        s = data.str_addr[text]
        put(LDA_I, s & 0xFF, STA_Z, Z_STR, LDA_I, s >> 8, STA_Z, Z_STR + 1)
        put(LDA_I, line_y(row), STA_Z, Z_Y, LDA_I, line_xb(text), STA_Z, Z_XB)
        a.jsr("draw_string")
    put(LDA_I, 0xFF, STA_Z, Z_DONE)
    a.label("hold")
    a.jmp("hold")

    a.label("irq")
    put(PHA, LDA_I, 0x00, STA_A, 0x00, 0x4D, STA_A, 0x00, 0x4C)
    put(INC_Z, Z_IRQ_LO)
    a.br(BNE, "irq_out")
    put(INC_Z, Z_IRQ_HI)
    a.label("irq_out")
    put(PLA, RTI)

    a.label("clear")
    put(LDA_I, 0x00, STA_Z, Z_PTR, TAY, LDX_I, 0x02)
    a.label("clear_page")
    put(STX_Z, Z_PTR + 1)
    a.label("clear_byte")
    put(STA_IY, Z_PTR, INY)
    a.br(BNE, "clear_byte")
    put(INX, CPX_I, 0x40)
    a.br(BNE, "clear_page")
    put(RTS)

    a.label("fill")
    put(LDA_Z, Z_Y, LSR, LSR, STA_Z, Z_PTR + 1)
    put(LDA_Z, Z_Y, ASL, ASL, ASL, ASL, ASL, ASL, CLC, ADC_Z, Z_XB, STA_Z, Z_PTR)
    put(LDY_I, 0x00, LDA_Z, Z_VAL)
    a.label("fill_byte")
    put(STA_IY, Z_PTR, INY, 0xC4, Z_N)  # CPY zp
    a.br(BNE, "fill_byte")
    put(INC_Z, Z_Y, DEC_Z, Z_ROWS)
    a.br(BNE, "fill")
    put(RTS)

    a.label("draw_string")
    put(LDA_I, 0x00, STA_Z, Z_SCOL)
    a.label("next_char")
    put(LDY_Z, Z_SCOL, LDA_IY, Z_STR, CMP_I, 0xFF)
    a.br(BEQ, "ds_done")
    put(TAX)
    put(LDA_AX, data.glo & 0xFF, data.glo >> 8, STA_Z, Z_GLYPH)
    put(LDA_AX, data.ghi & 0xFF, data.ghi >> 8, STA_Z, Z_GLYPH + 1)
    put(LDA_Z, Z_Y, STA_Z, Z_CUR, LDA_I, 0x00, STA_Z, Z_GIDX)
    a.label("glyph_row")
    put(LDA_Z, Z_CUR, LSR, LSR, STA_Z, Z_PTR + 1)
    put(LDA_Z, Z_CUR, ASL, ASL, ASL, ASL, ASL, ASL, CLC, ADC_Z, Z_XB, STA_Z, Z_PTR)
    put(LDY_Z, Z_GIDX, LDA_IY, Z_GLYPH, PHA, INY, LDA_IY, Z_GLYPH, INY, STY_Z, Z_GIDX)
    put(LDY_I, 0x01, STA_IY, Z_PTR, PLA, DEY, STA_IY, Z_PTR)
    put(INC_Z, Z_CUR, LDA_Z, Z_GIDX, CMP_I, 16)
    a.br(BNE, "glyph_row")
    put(INC_Z, Z_XB, INC_Z, Z_XB, INC_Z, Z_SCOL)
    a.jmp("next_char")
    a.label("ds_done")
    put(RTS)

    assert a.pc < INITIALS_ROM - PROG_BASE, f"code too large: {a.pc:#x}"
    a.resolve()
    prog = a.prog

    prog[INITIALS_ROM - PROG_BASE:INITIALS_ROM - PROG_BASE + 24] = INITIALS
    prog[SCORES_ROM - PROG_BASE:SCORES_ROM - PROG_BASE + 24] = SCORES
    blob = data.blob()
    off = DATA_BASE - PROG_BASE
    assert off + len(blob) < 0x2FFA, "data too large"
    prog[off:off + len(blob)] = blob

    irq = a.labels["irq"]
    for at, v in ((0x2FFA, irq), (0x2FFC, PROG_BASE), (0x2FFE, irq)):
        prog[at] = v & 0xFF
        prog[at + 1] = v >> 8
    return bytes(prog)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def probe(data: Data, text: str) -> tuple[list[int], list[int]]:
    row, _, pen = next(l for l in HELP_LINES if l[1] == text)
    y = line_y(row) + PROBE_ROW
    xb = line_xb(text)
    offs: list[int] = []
    vals: list[int] = []
    for i, ch in enumerate(text):
        g = data.glyphs[(ch, 0 if ch == " " else pen)]
        for k in range(2):
            offs.append(y * 64 + xb + 2 * i + k)
            vals.append(g[PROBE_ROW * 2 + k])
    return offs, vals


def write_header(path: Path, program: bytes, data: Data) -> None:
    body = ", ".join(f"0x{b:02x}" for b in program)

    def arr(ctype: str, name: str, vals) -> str:
        items = ", ".join(str(v) for v in vals)
        return f"inline constexpr std::array<{ctype}, {len(vals)}> {name} = {{{items}}};\n"

    to, tb = probe(data, HELP_TITLE)
    co, cb = probe(data, HELP_COPY)
    text = (
        "#ifndef MISSILE_HWTEST_ROMS_H\n"
        "#define MISSILE_HWTEST_ROMS_H\n\n"
        "#include <array>\n"
        "#include <cstdint>\n\n"
        "namespace missile::hwtest {\n\n"
        f"inline constexpr std::array<uint8_t, 0x3000> program = {{\n    {body}\n}};\n\n"
        f"{arr('int', 'help_title_offs', to)}\n"
        f"{arr('uint8_t', 'help_title_bytes', tb)}\n"
        f"{arr('int', 'help_copy_offs', co)}\n"
        f"{arr('uint8_t', 'help_copy_bytes', cb)}\n"
        f"inline constexpr int kHelpWaitFrames = {HELP_WAIT_FRAMES};\n"
        f"inline constexpr int kMadselProbeOff = {PROBE_Y * 64 + PROBE_X // 4};\n"
        "inline constexpr uint8_t kMadselProbeMask = 0x11;\n"
        f"inline constexpr uint16_t kInitialsRom = 0x{INITIALS_ROM:04x};\n"
        f"inline constexpr uint16_t kScoresRom = 0x{SCORES_ROM:04x};\n\n"
        "}  // namespace missile::hwtest\n\n"
        "#endif\n"
    )
    path.write_text(text)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, required=True)
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--png", type=Path, default=None)
    args = ap.parse_args()

    data = Data(DATA_BASE)
    program = build_program(data)

    args.dir.mkdir(parents=True, exist_ok=True)
    args.header.parent.mkdir(parents=True, exist_ok=True)
    (args.dir / "program.bin").write_bytes(program)

    names = ("035820-02.h1", "035821-02.jk1", "035822-03e.kl1",
             "035823-02.lm1", "035824-02.np1", "035825-02.r1")
    chips = {n: program[i * 0x800:(i + 1) * 0x800] for i, n in enumerate(names)}
    for name, blob in chips.items():
        (args.dir / name).write_bytes(blob)

    crc = {
        "kind": "hwtest",
        "program": f"{crc32(program):08x}",
        "chips": {n: f"{crc32(d):08x}" for n, d in chips.items()},
    }
    (args.dir / "crc.json").write_text(json.dumps(crc, indent=2) + "\n")
    write_header(args.header, program, data)
    print(f"wrote {args.dir} and {args.header} ({len(data.keys)} glyphs)")


if __name__ == "__main__":
    main()
