#!/usr/bin/env python3
"""Generate the Battlezone hardware self-test ROM set (not Atari code).

Program, vector ROM and Math Box microcode are all original:
  - writes 'B','Z','N','1' at $0000
  - runs its own microcode on the Math Box (multiply, signed compare)
  - shows an AVG diagnostic picture, then a help screen

Usage:
  python3 gen_hwtest.py --dir ../../web/roms --header ../../web/hwtest_roms.h
"""

from __future__ import annotations

import argparse
import json
import zlib
from pathlib import Path

PROG_BASE = 0x5000
PROG_SIZE = 0x3000
VEC_BASE = 0x3000
VEC_WORD = (VEC_BASE - 0x2000) // 2
WAIT_NMIS = 370  # ~1.5 s at 246 Hz
FRAME_NMIS = 6   # 41 Hz picture, as Battlezone itself

MUL_A = 0x1234
MUL_B = 0x0101
MAX_A = 0x0123
MAX_B = 0xFF00  # negative, so a signed compare picks MAX_A

PROGRAM_CHIPS = [
    ("036414-02.e1", 0x5000),
    ("036413-01.h1", 0x5800),
    ("036412-01.j1", 0x6000),
    ("036411-01.k1", 0x6800),
    ("036410-01.lm1", 0x7000),
    ("036409-01.n1", 0x7800),
]
VECTOR_CHIPS = [("036422-01.bc3", 0x3000), ("036421-01.a3", 0x3800)]
MAP_CHIP = "036174-01.b1"
UCODE_CHIPS = ["036175-01.m1", "036176-01.l1", "036177-01.k1",
               "036178-01.j1", "036179-01.h1", "036180-01.f1"]

# Rows of the help screen; row 3 is the Math Box verdict.
HELP_LINES = [
    (0, "BATTLEZONE ARCADE", 15),
    (1, "SELF TEST OK", 12),
    (5, "THIS PAGE DOES NOT", 10),
    (6, "INCLUDE ATARI'S", 10),
    (7, "BATTLEZONE ROMS", 10),
    (8, "THEY ARE STILL", 15),
    (9, "UNDER COPYRIGHT", 15),
    (11, "IF YOU OWN A BZONE SET", 10),
    (12, "LOAD IT YOURSELF", 12),
    (14, "IT STAYS IN THIS", 10),
    (15, "BROWSER ONLY", 10),
]
MB_ROW = 3
MB_OK = "MATH BOX OK"
MB_FAIL = "MATH BOX FAIL"

# Text drawn at SCAL linear 64, so one unit is 191/256 of a picture unit.
TEXT_SCALE = 64
UNIT = (255 - TEXT_SCALE) / 256
CELL = 20
GLYPH = 2  # font grid step
ROW_GAP = 28
TOP_BASELINE = 221  # units above centre; rows 0-1 sit under the red overlay

FONT_UP = 0xFE
FONT_LAST = 0xFF


def _P(x: int, y: int) -> int:
    return ((x & 0xF) << 4) | (y & 0xF)


# Stroke font on an 8x12 grid, shared with this repo's Asteroids self-test.
FONT: dict[str, tuple[int, ...]] = {
    " ": (FONT_LAST,),
    "A": (_P(0, 0), _P(0, 8), _P(4, 12), _P(8, 8), _P(8, 0), FONT_UP, _P(0, 4), _P(8, 4), FONT_LAST),
    "B": (_P(0, 0), _P(0, 12), _P(4, 12), _P(8, 10), _P(4, 6), _P(8, 2), _P(4, 0), _P(0, 0), FONT_LAST),
    "C": (_P(8, 0), _P(0, 0), _P(0, 12), _P(8, 12), FONT_LAST),
    "D": (_P(0, 0), _P(0, 12), _P(4, 12), _P(8, 8), _P(8, 4), _P(4, 0), _P(0, 0), FONT_LAST),
    "E": (_P(8, 0), _P(0, 0), _P(0, 12), _P(8, 12), FONT_UP, _P(0, 6), _P(6, 6), FONT_LAST),
    "F": (_P(0, 0), _P(0, 12), _P(8, 12), FONT_UP, _P(0, 6), _P(6, 6), FONT_LAST),
    "G": (_P(6, 6), _P(8, 4), _P(8, 0), _P(0, 0), _P(0, 12), _P(8, 12), FONT_LAST),
    "H": (_P(0, 0), _P(0, 12), FONT_UP, _P(0, 6), _P(8, 6), FONT_UP, _P(8, 12), _P(8, 0), FONT_LAST),
    "I": (_P(0, 0), _P(8, 0), FONT_UP, _P(4, 0), _P(4, 12), FONT_UP, _P(0, 12), _P(8, 12), FONT_LAST),
    "J": (_P(0, 4), _P(4, 0), _P(8, 0), _P(8, 12), FONT_LAST),
    "K": (_P(0, 0), _P(0, 12), FONT_UP, _P(8, 12), _P(0, 6), _P(6, 0), FONT_LAST),
    "L": (_P(8, 0), _P(0, 0), _P(0, 12), FONT_LAST),
    "M": (_P(0, 0), _P(0, 12), _P(4, 8), _P(8, 12), _P(8, 0), FONT_LAST),
    "N": (_P(0, 0), _P(0, 12), _P(8, 0), _P(8, 12), FONT_LAST),
    "O": (_P(0, 0), _P(0, 12), _P(8, 12), _P(8, 0), _P(0, 0), FONT_LAST),
    "P": (_P(0, 0), _P(0, 12), _P(8, 12), _P(8, 6), _P(0, 5), FONT_LAST),
    "R": (_P(0, 0), _P(0, 12), _P(8, 12), _P(8, 6), _P(0, 5), FONT_UP, _P(4, 5), _P(8, 0), FONT_LAST),
    "S": (_P(0, 2), _P(2, 0), _P(8, 0), _P(8, 5), _P(0, 7), _P(0, 12), _P(6, 12), _P(8, 10), FONT_LAST),
    "T": (_P(0, 12), _P(8, 12), FONT_UP, _P(4, 12), _P(4, 0), FONT_LAST),
    "U": (_P(0, 12), _P(0, 2), _P(4, 0), _P(8, 2), _P(8, 12), FONT_LAST),
    "V": (_P(0, 12), _P(4, 0), _P(8, 12), FONT_LAST),
    "W": (_P(0, 12), _P(2, 0), _P(4, 4), _P(6, 0), _P(8, 12), FONT_LAST),
    "X": (_P(0, 0), _P(8, 12), FONT_UP, _P(0, 12), _P(8, 0), FONT_LAST),
    "Y": (_P(0, 12), _P(4, 6), _P(8, 12), FONT_UP, _P(4, 6), _P(4, 0), FONT_LAST),
    "Z": (_P(0, 12), _P(8, 12), _P(0, 0), _P(8, 0), FONT_UP, _P(2, 6), _P(6, 6), FONT_LAST),
    "'": (_P(2, 6), _P(6, 10), FONT_LAST),
}

# --- AVG display list ---

HALT = 0x2000
RTSL = 0xC000
CNTR = 0x8040  # normalizer leaves the beam 64 CPU cycles to settle


def vctr(dx: int, dy: int, z: int) -> list[int]:
    return [dy & 0x1FFF, ((z & 7) << 13) | (dx & 0x1FFF)]


def stat(intensity: int) -> list[int]:
    return [0x6400 | ((intensity & 0xF) << 4)]  # D10 set: clip switches untouched


def stat_clip(upper_right: bool, intensity: int) -> list[int]:
    return [0x6000 | (0x200 if upper_right else 0) | ((intensity & 0xF) << 4)]


def scal(binary: int, linear: int) -> list[int]:
    return [0x7000 | ((binary & 7) << 8) | (linear & 0xFF)]


def jsrl(word: int) -> list[int]:
    return [0xA000 | (word & 0x1FFF)]


def jmpl(word: int) -> list[int]:
    return [0xE000 | (word & 0x1FFF)]


def glyph_words(ch: str) -> list[int]:
    words: list[int] = []
    at = (0, 0)
    pen = False
    for p in FONT.get(ch, FONT[" "]):
        if p == FONT_LAST:
            break
        if p == FONT_UP:
            pen = False
            continue
        x, y = ((p >> 4) & 0xF) * GLYPH, (p & 0xF) * GLYPH
        words += vctr(x - at[0], y - at[1], 1 if pen else 0)
        pen = True
        at = (x, y)
    words += vctr(CELL - at[0], -at[1], 0)
    words.append(RTSL)
    return words


def text_line(text: str, row: int, intensity: int, glyphs: dict[str, int]) -> list[int]:
    x = -(len(text) * CELL) // 2
    y = TOP_BASELINE - row * ROW_GAP
    words = [CNTR] + scal(0, TEXT_SCALE) + vctr(x, y, 0) + stat(intensity)
    for ch in text:
        words += jsrl(glyphs[ch])
    return words


def diag_list() -> list[int]:
    w: list[int] = [CNTR] + scal(0, 0) + stat(12)
    w += vctr(-280, -190, 0)
    w += vctr(560, 0, 1) + vctr(0, 380, 1) + vctr(-560, 0, 1) + vctr(0, -380, 1)
    w += [CNTR] + vctr(-280, 0, 0) + vctr(560, 0, 1)
    w += [CNTR] + vctr(0, -190, 0) + vctr(0, 380, 1)
    w += [CNTR]
    for size, z in ((200, 7), (130, 5), (60, 3)):
        w += [CNTR] + vctr(-size // 2, -size // 2, 0)
        w += vctr(size, 0, z) + vctr(0, size, z) + vctr(-size, 0, z) + vctr(0, -size, z)
    w += [CNTR] + vctr(-250, -160, 0)
    for z in range(1, 8):
        w += vctr(0, 50, z) + vctr(14, 0, 0) + vctr(0, -50, 0) + vctr(14, 0, 0)
    # Clip window from (140,40) to (260,160); the diagonal only lights inside it.
    w += [CNTR] + vctr(260, 160, 0) + stat_clip(True, 15)
    w += [CNTR] + vctr(140, 40, 0) + stat_clip(False, 15)
    w += [CNTR] + vctr(100, 0, 0) + vctr(180, 200, 7)
    w.append(HALT)
    return w


def build_vector_rom() -> tuple[bytes, dict[str, int]]:
    words: list[int] = []
    labels: dict[str, int] = {}

    def place(name: str, body: list[int]) -> None:
        labels[name] = VEC_WORD + len(words)
        words.extend(body)

    chars = sorted({c for _, t, _ in HELP_LINES for c in t} | set(MB_OK + MB_FAIL))
    glyphs: dict[str, int] = {}
    for ch in chars:
        place("glyph_" + ch, glyph_words(ch))
        glyphs[ch] = labels["glyph_" + ch]
    help_words: list[int] = []
    for row, text, bri in HELP_LINES:
        help_words += text_line(text, row, bri, glyphs)
    place("help", help_words + [RTSL])
    place("mb_ok", text_line(MB_OK, MB_ROW, 15, glyphs) + [RTSL])
    place("mb_fail", text_line(MB_FAIL, MB_ROW, 15, glyphs) + [RTSL])
    place("diag", diag_list())
    if len(words) > 0x800:
        raise RuntimeError(f"vector ROM full: {len(words)} words")
    out = bytearray(0x1000)
    for i, w in enumerate(words):
        out[2 * i] = w & 0xFF
        out[2 * i + 1] = (w >> 8) & 0xFF
    return bytes(out), labels


# --- Math Box microcode (fields per MBUDOC.DOC) ---

SRC = {"AQ": (0, 0), "AB": (0, 1), "ZQ": (0, 2), "ZB": (0, 3),
       "ZA": (1, 0), "DA": (1, 1), "DQ": (1, 2), "DZ": (1, 3)}
FUNC = {"ADD": 0, "SUBR": 1, "SUBS": 2, "OR": 3, "AND": 4, "NOTRS": 5, "EXOR": 6, "EXNOR": 7}
DEST = {"QREG": 0, "NOP": 1, "RAMA": 2, "RAMF": 3, "RAMQD": 4, "RAMD": 5, "RAMQU": 6, "RAMU": 7}


def uop(func: str, src: str, dest: str = "NOP", a: int = 0, b: int = 0,
        src_lo: str | None = None, stall: bool = False, ldab: bool = False,
        sign: bool = False, jmp: bool = False, mult: bool = False, cin: int = 0) -> int:
    i2hi, i10 = SRC[src]
    i2lo, i10lo = SRC[src_lo or src]
    assert i10 == i10lo, "the two byte halves share I1-I0"
    return ((a & 0xF) << 20 | (b & 0xF) << 16 | i2hi << 15 | i2lo << 14 | i10 << 12
            | int(stall) << 11 | FUNC[func] << 8 | int(ldab) << 7 | DEST[dest] << 4
            | int(sign) << 3 | int(jmp) << 2 | int(mult) << 1 | (cin & 1))


def build_microcode() -> tuple[list[int], list[int]]:
    code: list[int] = []
    entry: dict[str, int] = {}

    def at(name: str) -> None:
        entry[name] = len(code)

    at("idle")
    code.append(uop("ADD", "ZQ", stall=True))
    # Byte loads: one half takes D+0, the other keeps 0+B (MBUDOC).
    for reg in (0, 1):
        at(f"r{reg}l")
        code.append(uop("ADD", "ZB", "RAMF", b=reg, src_lo="DZ"))
        code.append(uop("ADD", "ZB", b=reg, stall=True))
        at(f"r{reg}h")
        code.append(uop("ADD", "DZ", "RAMF", b=reg, src_lo="ZB"))
        code.append(uop("ADD", "ZB", b=reg, stall=True))
    # R2:Q = R0 * R1, R1 unsigned: shift-and-add with CADD, sign from N^V.
    at("mul")
    code.append(uop("ADD", "ZB", "QREG", b=1))
    code.append(uop("AND", "ZB", "RAMQD", b=2))
    for _ in range(16):
        code.append(uop("ADD", "AB", "RAMQD", a=0, b=2, mult=True, sign=True))
    code.append(uop("ADD", "ZB", b=2, stall=True))
    at("low")
    code.append(uop("ADD", "ZQ", stall=True))
    # Signed max(R0, R1): JPL on the true sign of R0 - R1.
    at("max")
    take_r0 = len(code) + 3
    code.append(uop("ADD", "ZQ", a=take_r0 >> 4, b=take_r0 & 0xF, ldab=True))
    code.append(uop("SUBS", "AB", a=0, b=1, sign=True, jmp=True, cin=1))
    code.append(uop("ADD", "ZB", b=1, stall=True))
    assert len(code) == take_r0
    code.append(uop("ADD", "ZB", b=0, stall=True))

    mapping = [entry["idle"]] * 32
    for off, name in enumerate(("r0l", "r0h", "r1l", "r1h", "mul", "low", "max")):
        mapping[off] = entry[name]
    code += [uop("ADD", "ZQ", stall=True)] * (256 - len(code))
    return mapping, code


def ucode_nibbles(code: list[int]) -> list[bytes]:
    return [bytes((w >> (20 - 4 * p)) & 0xF for w in code) for p in range(6)]


def signed16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


# --- 6502 program ---

OPS = {
    ("lda", "imm"): 0xA9, ("lda", "zp"): 0xA5, ("lda", "abs"): 0xAD, ("lda", "absx"): 0xBD,
    ("ldx", "imm"): 0xA2, ("ldy", "imm"): 0xA0, ("sta", "zp"): 0x85, ("sta", "abs"): 0x8D,
    ("sta", "absx"): 0x9D, ("sta", "indy"): 0x91, ("stx", "zp"): 0x86,
    ("cmp", "imm"): 0xC9, ("cmp", "zp"): 0xC5, ("cpx", "imm"): 0xE0,
    ("bit", "abs"): 0x2C, ("inc", "zp"): 0xE6, ("inx", None): 0xE8, ("iny", None): 0xC8,
    ("dex", None): 0xCA, ("txs", None): 0x9A, ("cld", None): 0xD8, ("sei", None): 0x78,
    ("rti", None): 0x40, ("rts", None): 0x60, ("jsr", "abs"): 0x20, ("jmp", "abs"): 0x4C,
    ("and", "imm"): 0x29, ("ora", "imm"): 0x09, ("sec", None): 0x38, ("sbc", "zp"): 0xE5,
    ("bne", "rel"): 0xD0, ("beq", "rel"): 0xF0, ("bmi", "rel"): 0x30, ("bpl", "rel"): 0x10,
    ("bcc", "rel"): 0x90, ("bcs", "rel"): 0xB0, ("bvc", "rel"): 0x50, ("bvs", "rel"): 0x70,
}
SIZE = {None: 1, "imm": 2, "zp": 2, "indy": 2, "rel": 2, "abs": 3, "absx": 3}


class Asm:
    def __init__(self, base: int) -> None:
        self.base = base
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str, str]] = []

    @property
    def pc(self) -> int:
        return self.base + len(self.code)

    def label(self, name: str) -> None:
        self.labels[name] = self.pc

    def __call__(self, op: str, mode: str | None = None, arg: int | str = 0) -> None:
        self.code.append(OPS[(op, mode)])
        if mode is None:
            return
        at = len(self.code)
        if SIZE[mode] == 3:
            self.code += b"\0\0"
        else:
            self.code.append(0)
        if isinstance(arg, str):
            self.fixups.append((at, arg, mode))
        else:
            self._put(at, arg, mode)

    def _put(self, at: int, value: int, mode: str) -> None:
        if mode == "rel":
            rel = value - (self.base + at + 1)
            assert -128 <= rel <= 127, f"branch out of range: {rel}"
            self.code[at] = rel & 0xFF
        elif SIZE[mode] == 3:
            self.code[at] = value & 0xFF
            self.code[at + 1] = (value >> 8) & 0xFF
        else:
            self.code[at] = value & 0xFF

    def link(self) -> None:
        for at, name, mode in self.fixups:
            self._put(at, self.labels[name], mode)


IN0, DSW0, GOADD, WTCHDG, VSTOP = 0x0800, 0x0A00, 0x1200, 0x1400, 0x1600
MSTAT, MOUTLO, MOUTHI, MBOX = 0x1800, 0x1810, 0x1818, 0x1860
POKEY, SOUND, VECRAM = 0x1820, 0x1840, 0x2000
NMI_LO, NMI_HI, LAST_GO, PHASE = 0x60, 0x61, 0x62, 0x63
MB_FLAGS, MB_BUSY, MB_RESULT = 0x10, 0x11, 0x12


def assemble_program(labels: dict[str, int]) -> tuple[bytes, dict]:
    a = Asm(PROG_BASE)
    a.label("reset")
    a("sei")
    a("cld")
    a("ldx", "imm", 0x7F)
    a("txs")
    a("lda", "imm", 0)
    a("ldx", "imm", 0)
    a.label("clear")
    for page in (0x0000, 0x0100, 0x0200, 0x0300):
        a("sta", "absx", page)
    a("sta", "abs", WTCHDG)
    a("inx")
    a("bne", "rel", "clear")
    for i, ch in enumerate(b"BZN1"):
        a("lda", "imm", ch)
        a("sta", "zp", i)
    a("sta", "abs", VSTOP)
    a("lda", "imm", 0x20)
    a("sta", "abs", SOUND)
    a("lda", "imm", 0x07)
    a("sta", "abs", POKEY + 0x0F)
    a("lda", "imm", 0x50)
    a("sta", "abs", POKEY + 0x00)
    a("lda", "imm", 0xA6)
    a("sta", "abs", POKEY + 0x01)

    # Math Box: load, start, spin on busy and count the spins.
    def mb_load(reg: int, value: int) -> None:
        a("lda", "imm", value & 0xFF)
        a("sta", "abs", MBOX + reg * 2)
        a("lda", "imm", (value >> 8) & 0xFF)
        a("sta", "abs", MBOX + reg * 2 + 1)

    mb_load(0, MUL_A)
    mb_load(1, MUL_B)
    a("ldx", "imm", 0)
    a("sta", "abs", MBOX + 4)
    a.label("mb_wait")
    a("bit", "abs", MSTAT)
    a("bpl", "rel", "mb_done")
    a("inx")
    a("bne", "rel", "mb_wait")
    a.label("mb_done")
    a("stx", "zp", MB_BUSY)
    a("lda", "abs", MOUTHI)
    a("sta", "zp", MB_RESULT + 0)
    a("lda", "abs", MOUTLO)
    a("sta", "zp", MB_RESULT + 1)
    a("sta", "abs", MBOX + 5)
    a.label("mb_wait2")
    a("bit", "abs", MSTAT)
    a("bmi", "rel", "mb_wait2")
    a("lda", "abs", MOUTHI)
    a("sta", "zp", MB_RESULT + 2)
    a("lda", "abs", MOUTLO)
    a("sta", "zp", MB_RESULT + 3)
    mb_load(0, MAX_A)
    mb_load(1, MAX_B)
    a("sta", "abs", MBOX + 6)
    a.label("mb_wait3")
    a("bit", "abs", MSTAT)
    a("bmi", "rel", "mb_wait3")
    a("lda", "abs", MOUTHI)
    a("sta", "zp", MB_RESULT + 4)
    a("lda", "abs", MOUTLO)
    a("sta", "zp", MB_RESULT + 5)

    product = MUL_A * MUL_B
    want_max = max(signed16(MAX_A), signed16(MAX_B)) & 0xFFFF
    want = [product >> 24, product >> 16, product >> 8, product, want_max >> 8, want_max]
    for i, v in enumerate(want):
        a("lda", "zp", MB_RESULT + i)
        a("cmp", "imm", v & 0xFF)
        a("bne", "rel", "mb_bad")
    a("lda", "imm", 1)
    a("sta", "zp", MB_FLAGS)
    a.label("mb_bad")

    def put_words(words: list[int]) -> None:
        for i, w in enumerate(words):
            a("lda", "imm", w & 0xFF)
            a("sta", "abs", VECRAM + 2 * i)
            a("lda", "imm", (w >> 8) & 0xFF)
            a("sta", "abs", VECRAM + 2 * i + 1)

    put_words(jmpl(labels["diag"]))
    a("sta", "abs", GOADD)

    a.label("main")
    a("sta", "abs", WTCHDG)
    a("lda", "zp", PHASE)
    a("bne", "rel", "frame")
    a("lda", "zp", NMI_HI)
    a("cmp", "imm", (WAIT_NMIS >> 8) & 0xFF)
    a("bcc", "rel", "frame")
    a("lda", "zp", NMI_LO)
    a("cmp", "imm", WAIT_NMIS & 0xFF)
    a("bcc", "rel", "frame")
    a("inc", "zp", PHASE)
    a("lda", "zp", MB_FLAGS)
    a("beq", "rel", "show_fail")
    put_words(jsrl(labels["help"]) + jsrl(labels["mb_ok"]) + [HALT])
    a("jmp", "abs", "frame")
    a.label("show_fail")
    put_words(jsrl(labels["help"]) + jsrl(labels["mb_fail"]) + [HALT])

    # One GO per six NMIs, and only once the AVG has halted (IN0 D6).
    a.label("frame")
    a("lda", "zp", NMI_LO)
    a("sec")
    a("sbc", "zp", LAST_GO)
    a("cmp", "imm", FRAME_NMIS)
    a("bcc", "rel", "main")
    a("bit", "abs", IN0)
    a("bvc", "rel", "main")
    a("lda", "zp", NMI_LO)
    a("sta", "zp", LAST_GO)
    a("sta", "abs", GOADD)
    a("jmp", "abs", "main")

    a.label("nmi")
    a("inc", "zp", NMI_LO)
    a("bne", "rel", "nmi_tone")
    a("inc", "zp", NMI_HI)
    a.label("nmi_tone")
    a("lda", "zp", NMI_LO)
    a("cmp", "imm", 50)
    a("bne", "rel", "nmi_out")
    a("lda", "zp", NMI_HI)
    a("bne", "rel", "nmi_out")
    a("lda", "imm", 0xA0)
    a("sta", "abs", POKEY + 0x01)
    a.label("nmi_out")
    a("sta", "abs", WTCHDG)
    a("rti")
    a.link()

    code = bytes(a.code)
    assert len(code) < 0x7FFA - PROG_BASE, f"hwtest program too large: {len(code)}"
    prog = bytearray(PROG_SIZE)
    prog[: len(code)] = code
    vec = 0x7FFA - PROG_BASE
    for i, name in enumerate(("nmi", "reset", "reset")):
        prog[vec + 2 * i] = a.labels[name] & 0xFF
        prog[vec + 2 * i + 1] = (a.labels[name] >> 8) & 0xFF
    expect = {
        "mb_hi": (product >> 16) & 0xFFFF,
        "mb_lo": product & 0xFFFF,
        "mb_max": want_max,
    }
    return bytes(prog), expect


def crc(data: bytes) -> str:
    return format(zlib.crc32(data) & 0xFFFFFFFF, "08x")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, required=True)
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--png", type=Path, default=None)
    args = ap.parse_args()
    args.dir.mkdir(parents=True, exist_ok=True)

    vector, labels = build_vector_rom()
    mapping, code = build_microcode()
    program, expect = assemble_program(labels)
    mb_map = bytes(mapping)
    nibbles = ucode_nibbles(code)
    assert len(program) == PROG_SIZE and len(vector) == 0x1000

    files: dict[str, bytes] = {}
    for name, addr in PROGRAM_CHIPS:
        files[name] = program[addr - PROG_BASE: addr - PROG_BASE + 0x800]
    for name, addr in VECTOR_CHIPS:
        files[name] = vector[addr - VEC_BASE: addr - VEC_BASE + 0x800]
    files[MAP_CHIP] = mb_map
    for name, data in zip(UCODE_CHIPS, nibbles):
        files[name] = data
    for name, data in files.items():
        (args.dir / name).write_bytes(data)
    (args.dir / "program.bin").write_bytes(program)
    (args.dir / "vector.bin").write_bytes(vector)

    meta = {
        "kind": "hwtest",
        "program": crc(program),
        "vector": crc(vector),
        "mathbox": crc(mb_map + b"".join(nibbles)),
        "chips": {name: crc(data) for name, data in files.items()},
    }
    (args.dir / "crc.json").write_text(json.dumps(meta, indent=2) + "\n")

    def arr(name: str, data: bytes) -> str:
        body = ", ".join(f"0x{b:02X}" for b in data)
        return f"inline constexpr std::array<uint8_t, {len(data)}> {name} = {{{body}}};\n"

    hdr = (
        "// Generated by roms/hwtest/gen_hwtest.py.\n"
        "#ifndef BATTLEZONE_HWTEST_ROMS_H\n"
        "#define BATTLEZONE_HWTEST_ROMS_H\n"
        "#include <array>\n"
        "#include <cstdint>\n"
        "namespace battlezone { namespace hwtest {\n"
        + arr("program", program)
        + arr("vector", vector)
        + arr("mb_map", mb_map)
        + "".join(arr(f"mb_ucode{i}", d) for i, d in enumerate(nibbles))
        + f"inline constexpr int kHelpWaitNmis = {WAIT_NMIS + 4 * FRAME_NMIS};\n"
        + f"inline constexpr uint16_t kMulHi = 0x{expect['mb_hi']:04X};\n"
        + f"inline constexpr uint16_t kMulLo = 0x{expect['mb_lo']:04X};\n"
        + f"inline constexpr uint16_t kMax = 0x{expect['mb_max']:04X};\n"
        + f"inline constexpr uint16_t kHelpWord = 0x{labels['help']:04X};\n"
        + f"inline constexpr uint16_t kMbOkWord = 0x{labels['mb_ok']:04X};\n"
        + f"inline constexpr uint16_t kMbFailWord = 0x{labels['mb_fail']:04X};\n"
        + f"inline constexpr uint16_t kDiagWord = 0x{labels['diag']:04X};\n"
        + "} }\n#endif\n"
    )
    args.header.write_text(hdr)
    print(f"wrote {args.dir} and {args.header} ({len(code)} microwords, "
          f"{len(labels)} vector labels)")


if __name__ == "__main__":
    main()
