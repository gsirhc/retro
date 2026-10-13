#!/usr/bin/env python3
"""Generate the Asteroids hardware self-test ROM (not Atari game code).

Builds a 6K program image + 2K vector ROM that:
  - writes signature bytes 'A','S','T','1' at $0000
  - runs a DVG diagnostic picture briefly
  - then holds a vector help screen (original stroke font, not Atari's)
    explaining that Atari's ROMs are still under copyright

Usage:
  python3 gen_hwtest.py --dir ../../web/roms --header ../../web/hwtest_roms.h
"""

from __future__ import annotations

import argparse
import json
import zlib
from pathlib import Path

PROG_BASE = 0x6800
PROG_SIZE = 0x1800
WAIT_NMIS = 370  # ~1.5 s at ~246 Hz NMI

HELP_TITLE = "ASTEROIDS ARCADE"
# Row indices; blanks are the skipped integers (Pac-Man-style grouping).
HELP_LINES = [
    (0, HELP_TITLE, 15),
    (1, "SELF TEST OK", 12),
    (3, "THIS PAGE DOES NOT", 10),
    (4, "INCLUDE ATARI'S", 10),
    (5, "ASTEROIDS ROMS", 10),
    (7, "THEY ARE STILL", 15),
    (8, "UNDER COPYRIGHT", 15),
    (10, "IF YOU OWN A", 10),
    (11, "ATARI ASTEROID SET", 10),
    (12, "LOAD IT YOURSELF", 12),
    (14, "IT STAYS IN THIS", 10),
    (15, "BROWSER ONLY", 10),
]

# Cell size in DVG units (global scale 9 = /1).
# app.js shows a 4:3 window on DVG Y 64..960; keep clear of the focus-hint bar.
CELL_W = 42
LINE_GAP = 44
SCALE = 3  # 8×12 stroke grid → 24×36 glyph
FONT_UP = 0xFE
FONT_LAST = 0xFF
# Glyph tops must stay ≤ SAFE_TOP; baselines ≥ SAFE_BOT.
SAFE_TOP = 900
SAFE_BOT = 160

def _P(x: int, y: int) -> int:
    return ((x & 0xF) << 4) | (y & 0xF)


# Stroke font on an 8×12 grid (Ed Logg Asteroids notes / public recreations).
# FONT_UP starts a new stroke; FONT_LAST ends the glyph. Not a dump of Atari ROM.
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
    "Q": (_P(0, 0), _P(0, 12), _P(8, 12), _P(8, 4), _P(0, 0), FONT_UP, _P(4, 4), _P(8, 0), FONT_LAST),
    "R": (_P(0, 0), _P(0, 12), _P(8, 12), _P(8, 6), _P(0, 5), FONT_UP, _P(4, 5), _P(8, 0), FONT_LAST),
    "S": (_P(0, 2), _P(2, 0), _P(8, 0), _P(8, 5), _P(0, 7), _P(0, 12), _P(6, 12), _P(8, 10), FONT_LAST),
    "T": (_P(0, 12), _P(8, 12), FONT_UP, _P(4, 12), _P(4, 0), FONT_LAST),
    "U": (_P(0, 12), _P(0, 2), _P(4, 0), _P(8, 2), _P(8, 12), FONT_LAST),
    "V": (_P(0, 12), _P(4, 0), _P(8, 12), FONT_LAST),
    "W": (_P(0, 12), _P(2, 0), _P(4, 4), _P(6, 0), _P(8, 12), FONT_LAST),
    "X": (_P(0, 0), _P(8, 12), FONT_UP, _P(0, 12), _P(8, 0), FONT_LAST),
    "Y": (_P(0, 12), _P(4, 6), _P(8, 12), FONT_UP, _P(4, 6), _P(4, 0), FONT_LAST),
    "Z": (_P(0, 12), _P(8, 12), _P(0, 0), _P(8, 0), FONT_UP, _P(2, 6), _P(6, 6), FONT_LAST),
    "0": (_P(0, 0), _P(8, 0), _P(8, 12), _P(0, 12), _P(0, 0), _P(8, 12), FONT_LAST),
    "1": (_P(4, 0), _P(4, 12), _P(3, 10), FONT_LAST),
    "2": (_P(0, 12), _P(8, 12), _P(8, 7), _P(0, 5), _P(0, 0), _P(8, 0), FONT_LAST),
    "3": (_P(0, 12), _P(8, 12), _P(8, 0), _P(0, 0), FONT_UP, _P(0, 6), _P(8, 6), FONT_LAST),
    "4": (_P(0, 12), _P(0, 6), _P(8, 6), FONT_UP, _P(8, 12), _P(8, 0), FONT_LAST),
    "5": (_P(0, 0), _P(8, 0), _P(8, 6), _P(0, 7), _P(0, 12), _P(8, 12), FONT_LAST),
    "6": (_P(0, 12), _P(0, 0), _P(8, 0), _P(8, 5), _P(0, 7), FONT_LAST),
    "7": (_P(0, 12), _P(8, 12), _P(8, 6), _P(4, 0), FONT_LAST),
    "8": (_P(0, 0), _P(8, 0), _P(8, 12), _P(0, 12), _P(0, 0), FONT_UP, _P(0, 6), _P(8, 6), FONT_LAST),
    "9": (_P(8, 0), _P(8, 12), _P(0, 12), _P(0, 7), _P(8, 5), FONT_LAST),
    "'": (_P(2, 6), _P(6, 10), FONT_LAST),
    "-": (_P(2, 6), _P(6, 6), FONT_LAST),
    ".": (_P(3, 0), _P(4, 0), FONT_LAST),
}


def u16(n: int) -> bytes:
    return bytes((n & 0xFF, (n >> 8) & 0xFF))


def labs(x: int, y: int, scale: int = 9) -> list[int]:
    return [0xA000 | (y & 0x3FF), ((scale & 0xF) << 12) | (x & 0x3FF)]


def vec(bri: int, dx: int, dy: int, local: int = 0) -> list[int]:
    wy = (local & 0xF) << 12
    if dy < 0:
        wy |= 0x400 | ((-dy) & 0x3FF)
    else:
        wy |= dy & 0x3FF
    wx = (bri & 0xF) << 12
    if dx < 0:
        wx |= 0x400 | ((-dx) & 0x3FF)
    else:
        wx |= dx & 0x3FF
    return [wy, wx]


def rect(x: int, y: int, w: int, h: int, bri: int = 15) -> list[int]:
    return labs(x, y) + vec(bri, w, 0) + vec(bri, 0, h) + vec(bri, -w, 0) + vec(bri, 0, -h)


def build_display_list() -> list[int]:
    words: list[int] = []
    words += rect(64, 64, 896, 896, 12)
    words += labs(512, 64)
    words += vec(10, 0, 896)
    words += labs(64, 512)
    words += vec(10, 896, 0)
    for size, bri in ((320, 15), (200, 10), (80, 6)):
        words += rect(512 - size // 2, 512 - size // 2, size, size, bri)
    words += labs(200, 120)
    for i in range(8):
        bri = 2 + i * 2
        words += vec(bri, 0, 60)
        words += vec(0, 40, 0)
        words += vec(0, 0, -60)
    words += labs(200, 700)
    words.append(0xC000 | 0x0800)
    words += labs(750, 700)
    words.append(0xC000 | 0x0810)
    words.append(0xB000)
    return words


def glyph_words(ch: str, bri: int) -> list[int]:
    """Relative stroke list ending with blank advance + RTS."""
    pts = FONT.get(ch.upper(), FONT[" "])
    words: list[int] = []
    at = (0, 0)
    need_move = True
    for p in pts:
        if p == FONT_LAST:
            break
        if p == FONT_UP:
            need_move = True
            continue
        x = ((p >> 4) & 0xF) * SCALE
        y = (p & 0xF) * SCALE
        dx, dy = x - at[0], y - at[1]
        words += vec(0 if need_move else bri, dx, dy)
        need_move = False
        at = (x, y)
    words += vec(0, CELL_W - at[0], -at[1])
    words.append(0xD000)  # RTS
    return words


def build_font_rom() -> tuple[bytes, dict[str, int]]:
    """Vector ROM with diamond/ship + one JSR target per glyph."""
    rom_words = [0] * 0x400
    addrs: dict[str, int] = {}

    def put(word_addr: int, words: list[int]) -> None:
        for i, w in enumerate(words):
            rom_words[word_addr - 0x0800 + i] = w

    diamond = (
        vec(15, 40, 40) + vec(15, 40, -40) + vec(15, -40, -40) + vec(15, -40, 40) + [0xD000]
    )
    put(0x0800, diamond)
    ship = (
        vec(15, 0, 50) + vec(15, 30, -50) + vec(15, -60, 0) + vec(15, 30, 50) + [0xD000]
    )
    put(0x0810, ship)

    cursor = 0x0820
    # Brightness baked per call site; glyphs use bri=15 and the help list
    # can still vary overall intensity by drawing dimmer with a scale trick.
    # Simpler: store one glyph set at bri=15; help list uses it for all lines.
    needed = sorted({ch.upper() for _, text, _ in HELP_LINES for ch in text if ch != " "})
    needed.append(" ")
    for ch in needed:
        words = glyph_words(ch if ch != " " else " ", 15)
        if cursor - 0x0800 + len(words) > 0x400:
            raise RuntimeError(f"vector ROM full at {ch!r}")
        put(cursor, words)
        addrs[ch] = cursor
        cursor += len(words)

    out = bytearray(0x0800)
    for i, w in enumerate(rom_words):
        out[i * 2] = w & 0xFF
        out[i * 2 + 1] = (w >> 8) & 0xFF
    return bytes(out), addrs


def build_help_display_list(addrs: dict[str, int]) -> list[int]:
    # Center the block in the visible 4:3 tube. DVG y grows up.
    ascent = 12 * SCALE
    max_i = max(i for i, _, _ in HELP_LINES)
    span = max_i * LINE_GAP + ascent
    room = SAFE_TOP - SAFE_BOT
    top_y = SAFE_TOP - ascent - max(0, (room - span) // 2)
    words: list[int] = []
    for line_i, text, _bri in HELP_LINES:
        y = top_y - line_i * LINE_GAP
        x0 = 512 - (len(text) * CELL_W) // 2
        words += labs(x0, y)
        for ch in text.upper():
            if ch == " ":
                words += vec(0, CELL_W, 0)
            else:
                words.append(0xC000 | addrs[ch])
    words.append(0xB000)
    return words


def words_to_bytes(words: list[int]) -> bytes:
    out = bytearray()
    for w in words:
        out.append(w & 0xFF)
        out.append((w >> 8) & 0xFF)
    return bytes(out)


def assemble_program(diag: bytes, help_dl: bytes) -> bytes:
    code = bytearray()

    def org() -> int:
        return PROG_BASE + len(code)

    def emit(*bs: int) -> None:
        code.extend(b & 0xFF for b in bs)

    def emit_jsr(target: int) -> None:
        emit(0x20, target & 0xFF, (target >> 8) & 0xFF)

    def emit_jmp(target: int) -> None:
        emit(0x4C, target & 0xFF, (target >> 8) & 0xFF)

    emit(0xA9, ord("A"), 0x85, 0x00)
    emit(0xA9, ord("S"), 0x85, 0x01)
    emit(0xA9, ord("T"), 0x85, 0x02)
    emit(0xA9, ord("1"), 0x85, 0x03)
    emit(0xA9, 0x00, 0x85, 0x60)  # NMI counter lo; $60+ is clear of HIGH SCORE RAM $1D-$51
    emit(0x85, 0x61)              # hi
    emit(0x85, 0x68)              # last frame (NMI counter / 4)

    def emit_jsr_named(name: str) -> int:
        emit(0x20, 0x00, 0x00)
        return len(code) - 2

    jsr_sites: dict[str, list[int]] = {k: [] for k in ("copy_diag", "copy_help", "wait")}

    jsr_sites["copy_diag"].append(emit_jsr_named("copy_diag"))
    emit(0x8D, 0x00, 0x30)  # DVG GO
    jsr_sites["wait"].append(emit_jsr_named("wait"))
    jsr_sites["copy_help"].append(emit_jsr_named("copy_help"))

    # One GO per 4 NMIs (~61.5 Hz), like the game ROM; the beam can't redraw faster.
    loop = org()
    emit(0x8D, 0x00, 0x34)  # watchdog
    emit(0xA5, 0x60)        # LDA counter
    emit(0x4A, 0x4A)        # LSR LSR
    emit(0xC5, 0x68)        # CMP last frame
    rel = loop - (org() + 2)
    emit(0xF0, rel & 0xFF)  # BEQ
    emit(0x85, 0x68)
    wait_halt = org()
    emit(0x2C, 0x02, 0x20)  # BIT $2002
    emit(0x30, 0xFB)        # BMI wait while busy
    emit(0x8D, 0x00, 0x30)  # GO
    emit_jmp(loop)

    nmi = org()
    emit(0xE6, 0x60)
    emit(0xD0, 0x02)
    emit(0xE6, 0x61)
    emit(0x8D, 0x00, 0x34)
    emit(0x40)

    # copy: $62/$63 src, $64/$65 dst=$4000, $66/$67 count.
    copy = org()
    emit(0xA9, 0x00, 0x85, 0x64)
    emit(0xA9, 0x40, 0x85, 0x65)
    emit(0xA0, 0x00)
    cs = org()
    emit(0xB1, 0x62)
    emit(0x91, 0x64)
    emit(0xC8)
    emit(0xD0, 0x04)
    emit(0xE6, 0x63)
    emit(0xE6, 0x65)
    emit(0xA5, 0x66)
    emit(0xD0, 0x02)
    emit(0xC6, 0x67)
    emit(0xC6, 0x66)
    emit(0xA5, 0x66)
    emit(0x05, 0x67)
    rel = cs - (org() + 2)
    emit(0xD0, rel & 0xFF)
    emit(0x60)

    wait = org()
    wloop = org()
    emit(0x8D, 0x00, 0x34)
    emit(0xA5, 0x61)
    emit(0xC9, (WAIT_NMIS >> 8) & 0xFF)
    rel = wloop - (org() + 2)
    emit(0x90, rel & 0xFF)
    emit(0xD0, 0x06)
    emit(0xA5, 0x60)
    emit(0xC9, WAIT_NMIS & 0xFF)
    rel = wloop - (org() + 2)
    emit(0x90, rel & 0xFF)
    emit(0x60)

    # Align data
    while len(code) & 0xFF:
        emit(0x00)
    diag_addr = org()
    code.extend(diag)
    while len(code) & 0xFF:
        emit(0x00)
    help_addr = org()
    code.extend(help_dl)

    def make_thunk(data: int, size: int) -> int:
        t = org()
        emit(0xA9, data & 0xFF, 0x85, 0x62)
        emit(0xA9, (data >> 8) & 0xFF, 0x85, 0x63)
        emit(0xA9, size & 0xFF, 0x85, 0x66)
        emit(0xA9, (size >> 8) & 0xFF, 0x85, 0x67)
        emit_jsr(copy)
        emit(0x60)
        return t

    copy_diag = make_thunk(diag_addr, len(diag))
    copy_help = make_thunk(help_addr, len(help_dl))

    def patch(site: int, target: int) -> None:
        code[site] = target & 0xFF
        code[site + 1] = (target >> 8) & 0xFF

    for site in jsr_sites["copy_diag"]:
        patch(site, copy_diag)
    for site in jsr_sites["copy_help"]:
        patch(site, copy_help)
    for site in jsr_sites["wait"]:
        patch(site, wait)

    assert len(code) < 0x17FA, f"hwtest program too large: {len(code)}"
    prog = bytearray(PROG_SIZE)
    prog[: len(code)] = code
    off = 0x7FFA - PROG_BASE
    prog[off + 0 : off + 2] = u16(nmi)
    prog[off + 2 : off + 4] = u16(PROG_BASE)
    prog[off + 4 : off + 6] = u16(nmi)
    return bytes(prog)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, required=True)
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--png", type=Path, default=None)
    args = ap.parse_args()
    args.dir.mkdir(parents=True, exist_ok=True)

    vector, addrs = build_font_rom()
    diag = words_to_bytes(build_display_list())
    help_dl = words_to_bytes(build_help_display_list(addrs))
    program = assemble_program(diag, help_dl)
    assert len(program) == PROG_SIZE
    assert len(vector) == 0x0800

    (args.dir / "program.bin").write_bytes(program)
    (args.dir / "vector.bin").write_bytes(vector)
    (args.dir / "035145-04e.ef2").write_bytes(program[0:0x800])
    (args.dir / "035144-04e.h2").write_bytes(program[0x800:0x1000])
    (args.dir / "035143-02.j2").write_bytes(program[0x1000:0x1800])
    (args.dir / "035127-02.np3").write_bytes(vector)

    crc = {
        "kind": "hwtest",
        "program": format(zlib.crc32(program) & 0xFFFFFFFF, "08x"),
        "vector": format(zlib.crc32(vector) & 0xFFFFFFFF, "08x"),
        "chips": {
            "035145-04e.ef2": format(zlib.crc32(program[0:0x800]) & 0xFFFFFFFF, "08x"),
            "035144-04e.h2": format(zlib.crc32(program[0x800:0x1000]) & 0xFFFFFFFF, "08x"),
            "035143-02.j2": format(zlib.crc32(program[0x1000:0x1800]) & 0xFFFFFFFF, "08x"),
            "035127-02.np3": format(zlib.crc32(vector) & 0xFFFFFFFF, "08x"),
        },
    }
    (args.dir / "crc.json").write_text(json.dumps(crc, indent=2) + "\n")

    def arr(name: str, data: bytes) -> str:
        body = ", ".join(f"0x{b:02X}" for b in data)
        return f"inline constexpr std::array<uint8_t, {len(data)}> {name} = {{{body}}};\n"

    # First 8 bytes of the help display list (LABS for the title line).
    help_prefix = help_dl[:8]
    hdr = (
        "// Auto-generated by roms/hwtest/gen_hwtest.py — do not edit.\n"
        "#ifndef ASTEROIDS_HWTEST_ROMS_H\n"
        "#define ASTEROIDS_HWTEST_ROMS_H\n"
        "#include <array>\n"
        "#include <cstdint>\n"
        "namespace asteroids { namespace hwtest {\n"
        + arr("program", program)
        + arr("vector", vector)
        + arr("help_vector_prefix", help_prefix)
        + f"inline constexpr int kHelpWaitNmis = {WAIT_NMIS + 40};\n"
        + "} }\n#endif\n"
    )
    args.header.write_text(hdr)
    print(
        f"wrote {args.dir} and {args.header} "
        f"(diag {len(diag)} B, help {len(help_dl)} B, {len(addrs)} glyphs)"
    )


if __name__ == "__main__":
    main()
