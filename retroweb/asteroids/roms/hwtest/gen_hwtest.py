#!/usr/bin/env python3
"""Generate the Asteroids hardware self-test ROM (not Atari game code).

Builds a 6K program image + 2K vector ROM that:
  - writes signature bytes 'A','S','T','1' at $0000
  - kicks the watchdog at $3400
  - runs a DVG diagnostic picture (border, crosshair, nested boxes,
    intensity ramp, JSR'd diamond + ship triangle in vector ROM)
  - loops, re-kicking the watchdog and re-pulsing GO

Usage:
  python3 gen_hwtest.py --dir ../../web/roms --header ../../web/hwtest_roms.h
"""

from __future__ import annotations

import argparse
import json
import zlib
from pathlib import Path


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
    # Global scale 9 (/1). Picture proves LABS, VEC, intensity, and JSR/RTS.
    words: list[int] = []

    # Outer frame inset from the 1024×1024 DVG space.
    words += rect(64, 64, 896, 896, 12)

    # Crosshair through centre.
    words += labs(512, 64)
    words += vec(10, 0, 896)   # vertical
    words += labs(64, 512)
    words += vec(10, 896, 0)   # horizontal

    # Nested boxes around centre.
    for size, bri in ((320, 15), (200, 10), (80, 6)):
        words += rect(512 - size // 2, 512 - size // 2, size, size, bri)

    # Intensity ramp: 8 short vertical ticks, brightness 2..15.
    words += labs(200, 120)
    for i in range(8):
        bri = 2 + i * 2
        words += vec(bri, 0, 60)
        words += vec(0, 40, 0)      # blank move right
        words += vec(0, 0, -60)     # blank move down to baseline

    # JSR diamond at word $0800, then ship triangle at $0810.
    words += labs(200, 700)
    words.append(0xC000 | 0x0800)  # JSR diamond
    words += labs(750, 700)
    words.append(0xC000 | 0x0810)  # JSR ship
    words.append(0xB000)           # HALT
    return words


def build_vector_rom() -> bytes:
    """DVG subroutines at word $0800 / $0810 (CPU $5000 / $5020)."""
    rom_words = [0] * 0x400  # 1K words = 2K bytes

    def put(word_addr: int, words: list[int]) -> None:
        for i, w in enumerate(words):
            rom_words[word_addr - 0x0800 + i] = w

    # Diamond (relative, starts wherever LABS left the beam).
    diamond = (
        vec(15, 40, 40)
        + vec(15, 40, -40)
        + vec(15, -40, -40)
        + vec(15, -40, 40)
        + [0xD000]  # RTS
    )
    put(0x0800, diamond)

    # Small "ship" triangle pointing up.
    ship = (
        vec(15, 0, 50)
        + vec(15, 30, -50)
        + vec(15, -60, 0)
        + vec(15, 30, 50)
        + [0xD000]
    )
    put(0x0810, ship)

    out = bytearray(0x0800)
    for i, w in enumerate(rom_words):
        out[i * 2] = w & 0xFF
        out[i * 2 + 1] = (w >> 8) & 0xFF
    return bytes(out)


def assemble_program(display_words: list[int]) -> bytes:
    code = bytearray()

    def org() -> int:
        return 0x6800 + len(code)

    def emit(*bs: int) -> None:
        code.extend(bs)

    emit(0xA9, ord("A"), 0x85, 0x00)
    emit(0xA9, ord("S"), 0x85, 0x01)
    emit(0xA9, ord("T"), 0x85, 0x02)
    emit(0xA9, ord("1"), 0x85, 0x03)

    dl = bytearray()
    for w in display_words:
        dl.append(w & 0xFF)
        dl.append((w >> 8) & 0xFF)

    for i, b in enumerate(dl):
        addr = 0x4000 + i
        emit(0xA9, b)
        emit(0x8D, addr & 0xFF, (addr >> 8) & 0xFF)

    emit(0x8D, 0x00, 0x30)  # DVG GO

    loop = org()
    emit(0x8D, 0x00, 0x34)  # watchdog
    wait = org()
    emit(0x2C, 0x02, 0x20)  # BIT $2002
    emit(0x30, 0xFB)        # BMI wait while busy
    emit(0x8D, 0x00, 0x30)  # GO
    emit(0x4C, loop & 0xFF, (loop >> 8) & 0xFF)

    nmi = org()
    emit(0x8D, 0x00, 0x34)
    emit(0x40)

    prog = bytearray(0x1800)
    assert len(code) < 0x17FA, f"hwtest program too large: {len(code)}"
    prog[: len(code)] = code
    reset = 0x6800
    off = 0x7FFA - 0x6800
    prog[off + 0 : off + 2] = u16(nmi)
    prog[off + 2 : off + 4] = u16(reset)
    prog[off + 4 : off + 6] = u16(nmi)
    return bytes(prog)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, required=True)
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--png", type=Path, default=None)
    args = ap.parse_args()
    args.dir.mkdir(parents=True, exist_ok=True)

    display = build_display_list()
    program = assemble_program(display)
    vector = build_vector_rom()
    assert len(program) == 0x1800
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

    hdr = (
        "// Auto-generated by roms/hwtest/gen_hwtest.py — do not edit.\n"
        "#ifndef ASTEROIDS_HWTEST_ROMS_H\n"
        "#define ASTEROIDS_HWTEST_ROMS_H\n"
        "#include <array>\n"
        "#include <cstdint>\n"
        "namespace asteroids { namespace hwtest {\n"
        + arr("program", program)
        + arr("vector", vector)
        + "} }\n#endif\n"
    )
    args.header.write_text(hdr)
    print(f"wrote {args.dir} and {args.header} ({len(display)} DVG words)")


if __name__ == "__main__":
    main()
