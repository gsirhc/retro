#!/usr/bin/env python3
"""Emit the small built-in ROM images the browser front end serves from roms/.

These are hand-assembled 8080 programs for the 88-2SIO console at ports
0x10 (control/status) and 0x11 (data). Run by `make -C web roms`.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))


def write(name, data):
    path = os.path.join(HERE, name)
    with open(path, "wb") as f:
        f.write(bytes(data))
    print(f"  {name}  ({len(data)} bytes)")


# ---------------------------------------------------------------------------
# echo.bin — identical to the core's built-in default ROM.
#   reset ACIA ch A, set 8N1, then loop: read a byte when RDRF, echo when TDRE.
# ---------------------------------------------------------------------------
ECHO = [
    0x3E, 0x03, 0xD3, 0x10,                    # MVI A,03h / OUT 10h  (master reset)
    0x3E, 0x11, 0xD3, 0x10,                    # MVI A,11h / OUT 10h  (8N1, /16)
    0xDB, 0x10, 0xE6, 0x01, 0xCA, 0x08, 0x00,  # IN 10h / ANI 01h / JZ  0008h
    0xDB, 0x11, 0x47,                          # IN 11h / MOV B,A
    0xDB, 0x10, 0xE6, 0x02, 0xCA, 0x12, 0x00,  # IN 10h / ANI 02h / JZ  0012h
    0x78, 0xD3, 0x11,                          # MOV A,B / OUT 11h
    0xC3, 0x08, 0x00,                          # JMP 0008h
]

# ---------------------------------------------------------------------------
# hello.bin — print a greeting to the console, then HLT.
#   0000 MVI A,03h / OUT 10h          master reset
#   0004 MVI A,11h / OUT 10h          8N1
#   0008 LXI H,001Fh                  -> msg
#   000B MOV A,M / ORA A / JZ 001Eh   loop: stop at NUL
#   0010 IN 10h / ANI 02h / JZ 0010h  wait TDRE
#   0017 MOV A,M / OUT 11h / INX H
#   001B JMP 000Bh
#   001E HLT
#   001F "HELLO FROM THE INTEL 8080!" CR LF NUL
# ---------------------------------------------------------------------------
MSG = b"HELLO FROM THE INTEL 8080!\r\n\x00"
HELLO = [
    0x3E, 0x03, 0xD3, 0x10,
    0x3E, 0x11, 0xD3, 0x10,
    0x21, 0x1F, 0x00,
    0x7E, 0xB7, 0xCA, 0x1E, 0x00,
    0xDB, 0x10, 0xE6, 0x02, 0xCA, 0x10, 0x00,
    0x7E, 0xD3, 0x11, 0x23,
    0xC3, 0x0B, 0x00,
    0x76,
] + list(MSG)

# ---------------------------------------------------------------------------
# killbits.bin — "Kill the Bit", Dean McDaniel, MITS, 1975. A bit sweeps across
# the A8..A15 address lamps; flip the matching sense switch to knock it out.
# Freely published; the canonical 24-byte listing.
#   0000 LXI H,0 / MVI D,80h / LXI B,000Eh
#   0008 LDAX D x4 / DAD B / JNC 0008h        (speed delay + accumulate)
#   0010 IN 0FFh / XRA D / RRC / MOV D,A      (mix in the switches, rotate)
#   0016 JMP 0008h
# ---------------------------------------------------------------------------
KILLBITS = [
    0x21, 0x00, 0x00, 0x16, 0x80, 0x01, 0x0E, 0x00,
    0x1A, 0x1A, 0x1A, 0x1A, 0x09, 0xD2, 0x08, 0x00,
    0xDB, 0xFF, 0xAA, 0x0F, 0x57, 0xC3, 0x08, 0x00,
]

if __name__ == "__main__":
    print("generating built-in ROMs:")
    write("echo.bin", ECHO)
    write("hello.bin", HELLO)
    write("killbits.bin", KILLBITS)
