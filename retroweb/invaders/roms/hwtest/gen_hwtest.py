#!/usr/bin/env python3
"""Assemble the Space Invaders board self-test ROM (original code, not Taito's)."""
import argparse, json, os, sys, zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "shared"))
from hwtest_font import font  # noqa: E402

PROG_SIZE = 0x2000
VRAM = 0x2400
SIG = 0x2000
SHIFT_OK = 0x2004
SHIFT_RESULT = 0x2005
FRAMES = 0x2008
MIDS = 0x2009

# Upright monitor after ROT270: 28 columns x 32 rows of 8x8 cells.
U_COLS, U_ROWS = 28, 32

FONT = font(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.,!?'/:")

HELP_TITLE = "SPACE INVADERS"
STATUS_ROW = 7
HELP_LINES = [
    (4,  HELP_TITLE),
    (10, "THIS PAGE DOES NOT"),
    (12, "INCLUDE TAITO'S"),
    (14, "SPACE INVADERS ROMS"),
    (17, "THEY ARE STILL"),
    (19, "UNDER COPYRIGHT"),
    (22, "IF YOU OWN A"),
    (24, "MIDWAY INVADERS SET"),
    (26, "LOAD IT YOURSELF"),
    (29, "IT STAYS IN THIS"),
    (31, "BROWSER ONLY"),
]
STATUS_OK = "SELF TEST OK"
STATUS_BAD = "SHIFTER BAD"

# MB14241 check: two data writes, then a shift count; result is the
# 16-bit register (newest byte high) shifted left by the count, high byte out.
SHIFT_DATA = (0xA5, 0x3C)
SHIFT_COUNT = 3


def shift_expect():
    reg = (SHIFT_DATA[1] << 8) | SHIFT_DATA[0]
    return (reg >> (8 - SHIFT_COUNT)) & 0xFF


class Asm:
    def __init__(self, size):
        self.mem = bytearray(size)
        self.pc = 0
        self.labels = {}
        self.fixups = []

    def org(self, addr):
        self.pc = addr

    def label(self, name):
        self.labels[name] = self.pc

    def db(self, *bs):
        for b in bs:
            self.mem[self.pc] = b & 0xFF
            self.pc += 1

    def dw(self, v):
        if isinstance(v, str):
            self.fixups.append((self.pc, v))
            v = 0
        self.db(v & 0xFF, v >> 8)

    def op16(self, op, v):
        self.db(op)
        self.dw(v)

    def call(self, name):
        self.op16(0xCD, name)

    def resolve(self):
        for at, name in self.fixups:
            v = self.labels[name]
            self.mem[at] = v & 0xFF
            self.mem[at + 1] = v >> 8


# 8080 opcodes used below.
JMP, JZ, JNZ, JNC, RET, RZ = 0xC3, 0xCA, 0xC2, 0xD2, 0xC9, 0xC8
LXI_B, LXI_D, LXI_H, LXI_SP = 0x01, 0x11, 0x21, 0x31
STA, LDA = 0x32, 0x3A
MVI_A, MVI_B, MVI_C, MVI_M = 0x3E, 0x06, 0x0E, 0x36
OUT, IN = 0xD3, 0xDB
PUSH_PSW, POP_PSW, PUSH_H, POP_H, PUSH_D, POP_D = 0xF5, 0xF1, 0xE5, 0xE1, 0xD5, 0xD1
EI, DI = 0xFB, 0xF3
XRA_A, ORA_A, ORA_C, ANI, CPI, ADI, SUI, CMP_C, ADD_C, SUB_M = 0xAF, 0xB7, 0xB1, 0xE6, 0xFE, 0xC6, 0xD6, 0xB9, 0x81, 0x96
MOV_A_M, MOV_M_D, MOV_D_A, MOV_E_A, MOV_C_A, MOV_L_A, MOV_A_B, MOV_A_E = 0x7E, 0x72, 0x57, 0x5F, 0x4F, 0x6F, 0x78, 0x7B
MVI_H, STAX_D, INX_H, DCX_B, INR_A, INR_B, INR_D, DCR_C, DAD_H, DAD_B = 0x26, 0x12, 0x23, 0x0B, 0x3C, 0x04, 0x14, 0x0D, 0x29, 0x09


def text_table(lines):
    out = bytearray()
    for row, text in lines:
        col = max(0, (U_COLS - len(text)) // 2)
        out += bytes((col, row)) + text.encode("ascii") + b"\x00"
    return bytes(out + b"\xff")


def rotated_glyph(rows):
    # Upright cell column i is native line 8c+i; upright row j is bit 7-j.
    return bytes(sum(((rows[j] >> (7 - i)) & 1) << (7 - j) for j in range(8)) for i in range(8))


def font_table():
    out = bytearray()
    for code in range(0x20, 0x5B):
        out += rotated_glyph(FONT.get(chr(code), FONT[" "]))
    return bytes(out)


def assemble():
    a = Asm(PROG_SIZE)
    a.org(0x0000)
    a.op16(JMP, "start")
    a.org(0x0008)
    a.op16(JMP, "isr_mid")
    a.org(0x0010)
    a.op16(JMP, "isr_vblank")

    a.org(0x0040)
    a.label("isr_mid")
    a.db(PUSH_PSW)
    a.op16(LDA, MIDS)
    a.db(INR_A)
    a.op16(STA, MIDS)
    a.db(POP_PSW, EI, RET)

    a.label("isr_vblank")
    a.db(PUSH_PSW)
    a.op16(LDA, FRAMES)
    a.db(INR_A)
    a.op16(STA, FRAMES)
    a.db(POP_PSW, EI, RET)

    # wait: A frames, feeding the watchdog.
    a.label("wait")
    a.db(MOV_C_A)
    a.op16(LDA, FRAMES)
    a.db(ADD_C, MOV_C_A)
    a.label("wait_loop")
    a.db(OUT, 6)
    a.op16(LDA, FRAMES)
    a.db(CMP_C)
    a.op16(JNZ, "wait_loop")
    a.db(RET)

    # fill: VRAM with A.
    a.label("fill")
    a.db(MOV_D_A)
    a.op16(LXI_H, VRAM)
    a.op16(LXI_B, 0x1C00)
    a.label("fill_loop")
    a.db(MOV_M_D, INX_H, DCX_B, MOV_A_B, ORA_C)
    a.op16(JNZ, "fill_loop")
    a.db(RET)

    # crosshatch: a 16-pixel grid on the upright screen.
    a.label("cross")
    a.op16(LXI_H, VRAM)
    a.db(MVI_B, 0)
    a.label("cross_line")
    a.db(MOV_A_B, ANI, 0x0F)
    a.op16(JZ, "cross_full")
    a.db(MOV_A_B, CPI, 223)
    a.op16(JZ, "cross_full")
    a.db(MVI_C, 16)
    a.label("cross_odd")
    a.db(MVI_M, 0x00, INX_H, MVI_M, 0x80, INX_H, DCR_C)
    a.op16(JNZ, "cross_odd")
    a.op16(JMP, "cross_next")
    a.label("cross_full")
    a.db(MVI_C, 32)
    a.label("cross_full_loop")
    a.db(MVI_M, 0xFF, INX_H, DCR_C)
    a.op16(JNZ, "cross_full_loop")
    a.label("cross_next")
    a.db(INR_B, MOV_A_B, CPI, 224)
    a.op16(JNZ, "cross_line")
    a.db(RET)

    # text: HL -> (col, row, chars..., 0)* 0xFF.
    a.label("text")
    a.db(MOV_A_M, CPI, 0xFF, RZ)
    a.db(ADI, VRAM >> 8, MOV_D_A, INX_H)
    a.db(MVI_A, 31, SUB_M, MOV_E_A, INX_H)
    a.label("text_char")
    a.db(MOV_A_M, INX_H, ORA_A)
    a.op16(JZ, "text")
    a.db(PUSH_H, PUSH_D)
    a.db(SUI, 0x20, MOV_L_A, MVI_H, 0, DAD_H, DAD_H, DAD_H)
    a.op16(LXI_B, "font")
    a.db(DAD_B, MVI_C, 8)
    a.label("text_col")
    a.db(MOV_A_M, STAX_D, INX_H, MOV_A_E, ADI, 32, MOV_E_A)
    a.op16(JNC, "text_nc")
    a.db(INR_D)
    a.label("text_nc")
    a.db(DCR_C)
    a.op16(JNZ, "text_col")
    a.db(POP_D, INR_D, POP_H)
    a.op16(JMP, "text_char")

    a.label("start")
    a.op16(LXI_SP, VRAM)
    for i, ch in enumerate("TST1"):
        a.db(MVI_A, ord(ch))
        a.op16(STA, SIG + i)
    a.db(XRA_A)
    a.op16(STA, FRAMES)
    a.op16(STA, MIDS)
    a.db(MVI_A, 0x20, OUT, 3)
    a.db(XRA_A, OUT, 5)

    a.db(MVI_A, SHIFT_DATA[0], OUT, 4)
    a.db(MVI_A, SHIFT_DATA[1], OUT, 4)
    a.db(MVI_A, SHIFT_COUNT, OUT, 2)
    a.db(IN, 3)
    a.op16(STA, SHIFT_RESULT)
    a.db(CPI, shift_expect(), MVI_A, 0)
    a.op16(JNZ, "shift_done")
    a.db(INR_A)
    a.label("shift_done")
    a.op16(STA, SHIFT_OK)
    a.db(EI)

    a.call("cross")
    for note in (1, 2, 4, 8):
        a.db(MVI_A, note, OUT, 5, MVI_A, 12)
        a.call("wait")
        a.db(XRA_A, OUT, 5, MVI_A, 10)
        a.call("wait")
    a.db(MVI_A, 0xFF)
    a.call("fill")
    a.db(MVI_A, 0x21, OUT, 3, MVI_A, 60)
    a.call("wait")
    a.db(MVI_A, 0x20, OUT, 3)
    a.db(XRA_A)
    a.call("fill")
    a.op16(LXI_H, "help")
    a.call("text")
    a.op16(LDA, SHIFT_OK)
    a.db(ORA_A)
    a.op16(LXI_H, "status_bad")
    a.op16(JZ, "status")
    a.op16(LXI_H, "status_ok")
    a.label("status")
    a.call("text")
    a.label("hang")
    a.db(OUT, 6)
    a.op16(JMP, "hang")

    for name, data in (
        ("help", text_table(HELP_LINES)),
        ("status_ok", text_table([(STATUS_ROW, STATUS_OK)])),
        ("status_bad", text_table([(STATUS_ROW, STATUS_BAD)])),
        ("font", font_table()),
    ):
        a.label(name)
        a.db(*data)
    a.resolve()
    assert a.pc <= PROG_SIZE
    return bytes(a.mem)


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def write_c_header(path, program):
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write("// Generated by gen_hwtest.py, do not edit.\n")
        f.write("#pragma once\n#include <cstdint>\n#include <array>\n")
        f.write("namespace invaders { namespace hwtest {\n")
        f.write(f"inline const std::array<uint8_t, {len(program)}> program = {{")
        f.write(",".join(str(b) for b in program))
        f.write("};\n")
        f.write(f"constexpr uint8_t shift_expect = {shift_expect()};\n")
        cells = b"".join(rotated_glyph(FONT[ch]) for ch in STATUS_OK)
        f.write(f"constexpr int status_row = {STATUS_ROW};\n")
        f.write(f"constexpr int status_col = {(U_COLS - len(STATUS_OK)) // 2};\n")
        f.write(f"inline const std::array<uint8_t, {len(cells)}> status_ok = {{")
        f.write(",".join(str(b) for b in cells))
        f.write("};\n")
        f.write("}} // namespace\n")


def marquee_png(path):
    import sys
    shared = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "shared"))
    if shared not in sys.path:
        sys.path.insert(0, shared)
    from marquee import render_marquee
    render_marquee(path, "invaders")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--header")
    ap.add_argument("--png")
    args = ap.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    program = assemble()
    with open(os.path.join(args.dir, "program.bin"), "wb") as f:
        f.write(program)
    for i, name in enumerate(("9316b-0869_m739h.h1", "9316b-0856_m739g.g1",
                              "9316b-0855_m739f.f1", "9316b-0854_m739e.e1")):
        with open(os.path.join(args.dir, name), "wb") as f:
            f.write(program[i * 0x800:(i + 1) * 0x800])
    if args.header:
        write_c_header(args.header, program)
    if args.png:
        os.makedirs(os.path.dirname(os.path.abspath(args.png)) or ".", exist_ok=True)
        marquee_png(args.png)
    crc = crc32(program)
    with open(os.path.join(args.dir, "crc.json"), "w") as f:
        json.dump({"program.bin": f"{crc:08x}"}, f)
    print(f"hwtest CRC: program.bin {crc:08x}")


if __name__ == "__main__":
    main()
