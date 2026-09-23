"""Original landing-page arcade tiles.

Not scans or redraws of Namco/Midway/Konami/Sega art — no official logotype,
character sprites, or manufacturer mark. Background is a generated starfield;
lettering is Courier New Bold (a generic system typeface, not a cabinet
logotype). Glyphs are a raster atlas of that face so CI does not need the TTF.
"""
from __future__ import annotations

import math
import os
import struct
import sys
import zlib


def write_png(path, w, h, rgb_rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(
            ">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def _lerp(a, b, t):
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def _mix(dst, src, a):
    if a <= 0:
        return dst
    if a >= 1:
        return src
    return tuple(int(dst[i] + (src[i] - dst[i]) * a) for i in range(3))


def _fill_rect(pix, x0, y0, x1, y1, rgb, alpha=1.0):
    w, h = len(pix[0]), len(pix)
    if x1 < x0:
        x0, x1 = x1, x0
    if y1 < y0:
        y0, y1 = y1, y0
    if alpha <= 0 or x1 <= 0 or y1 <= 0 or x0 >= w or y0 >= h:
        return
    ix0 = max(0, int(math.floor(x0)))
    iy0 = max(0, int(math.floor(y0)))
    ix1 = min(w - 1, int(math.ceil(x1) - 1e-6))
    iy1 = min(h - 1, int(math.ceil(y1) - 1e-6))
    for y in range(iy0, iy1 + 1):
        ya = min(y + 1.0, y1) - max(float(y), y0)
        if ya <= 0:
            continue
        for x in range(ix0, ix1 + 1):
            xa = min(x + 1.0, x1) - max(float(x), x0)
            if xa <= 0:
                continue
            a = xa * ya * alpha
            if a > 0:
                pix[y][x] = _mix(pix[y][x], rgb, a if a < 1 else 1.0)


def _stamp(pix, cx, cy, r, rgb, alpha=1.0):
    if r <= 0 or alpha <= 0:
        return
    w, h = len(pix[0]), len(pix)
    x0 = max(0, int(cx - r) - 1)
    x1 = min(w - 1, int(cx + r) + 1)
    y0 = max(0, int(cy - r) - 1)
    y1 = min(h - 1, int(cy + r) + 1)
    for y in range(y0, y1 + 1):
        dy = y + 0.5 - cy
        for x in range(x0, x1 + 1):
            dx = x + 0.5 - cx
            d = math.hypot(dx, dy)
            if d > r + 0.55:
                continue
            a = alpha if d <= r - 0.45 else alpha * max(0.0, 1.0 - (d - (r - 0.45)) / 1.0)
            if a > 0:
                pix[y][x] = _mix(pix[y][x], rgb, a)


def _hash01(*parts):
    n = 0x9E3779B97F4A7C15
    for p in parts:
        x = int(p * 0x100000 if isinstance(p, float) else p)
        n ^= (x + 0x9E3779B9 + ((n << 6) & 0xFFFFFFFFFFFFFFFF) + (n >> 2))
        n &= 0xFFFFFFFFFFFFFFFF
    n = (n ^ (n >> 30)) * 0xBF58476D1CE4E5B9 & 0xFFFFFFFFFFFFFFFF
    n = (n ^ (n >> 27)) * 0x94D049BB133111EB & 0xFFFFFFFFFFFFFFFF
    return ((n ^ (n >> 31)) & 0xFFFFFFFF) / 4294967295.0


def _noise(x, y):
    return (0.50 * math.sin(x * 1.31 + y * 0.73)
            * math.cos(x * 0.87 - y * 1.19)
            + 0.30 * math.sin(x * 2.63 + y * 2.11)
            + 0.20 * math.sin(x * 5.17 - y * 3.41))


def _galaxy(pix, w, h, spec):
    void = spec["void"]
    neb_a = spec["nebula_a"]
    neb_b = spec["nebula_b"]
    neb_c = spec["nebula_c"]
    n1x, n1y = spec["neb1"]
    n2x, n2y = spec["neb2"]
    for y in range(h):
        ny = (y + 0.5) / h
        for x in range(w):
            nx = (x + 0.5) / w
            d1 = math.hypot(nx - n1x, ny - n1y)
            d2 = math.hypot(nx - n2x, ny - n2y)
            swirl = 0.55 + 0.45 * _noise(nx * 4.2 + spec["seed"], ny * 3.6)
            n1 = math.exp(-(d1 * 2.35) ** 2) * swirl
            n2 = math.exp(-(d2 * 2.80) ** 2) * (0.7 + 0.3 * swirl)
            band = math.exp(-((ny - 0.48 - 0.10 * math.sin(nx * 3.4 + spec["seed"])) * 3.6) ** 2)
            rgb = void
            rgb = _lerp(rgb, neb_a, 0.95 * n1)
            rgb = _lerp(rgb, neb_b, 0.88 * n2)
            rgb = _lerp(rgb, neb_c, 0.55 * band * swirl)
            edge = 1.0 - 0.28 * ((nx * 2 - 1) ** 2) - 0.16 * ((ny * 2 - 1) ** 2)
            rgb = _lerp(void, rgb, _clamp(edge, 0.40, 1.0))
            pix[y][x] = rgb

    n_stars = 90
    for i in range(n_stars):
        sx = _hash01(spec["seed"], i, 11) * (w - 8) + 4
        sy = _hash01(spec["seed"], i, 23) * (h - 8) + 4
        mag = _hash01(spec["seed"], i, 47)
        col = _lerp(spec["star"], (255, 255, 255), mag)
        if mag > 0.70:
            _stamp(pix, sx, sy, 1.05, col, 0.95)
            _stamp(pix, sx, sy, 2.2, col, 0.22)
        elif mag > 0.40:
            _stamp(pix, sx, sy, 0.80, col, 0.85)
        else:
            _stamp(pix, sx, sy, 0.50, col, 0.45 + 0.4 * mag)
    for px, py in spec["spikes"]:
        col = spec["star"]
        _stamp(pix, px, py, 1.4, col, 1.0)
        _stamp(pix, px, py, 2.8, col, 0.30)
        _fill_rect(pix, px - 6.0, py - 0.30, px + 6.0, py + 0.30, col, 0.50)
        _fill_rect(pix, px - 0.30, py - 6.0, px + 0.30, py + 6.0, col, 0.50)

    pcx, pcy, pr = spec["planet"]
    lit = spec["planet_lit"]
    shade = spec["planet_shade"]
    x0 = max(0, int(pcx - pr) - 1)
    x1 = min(w - 1, int(pcx + pr) + 1)
    y0 = max(0, int(pcy - pr) - 1)
    y1 = min(h - 1, int(pcy + pr) + 1)
    for y in range(y0, y1 + 1):
        dy = (y + 0.5 - pcy) / pr
        for x in range(x0, x1 + 1):
            dx = (x + 0.5 - pcx) / pr
            d2 = dx * dx + dy * dy
            if d2 > 1.0:
                continue
            d = math.sqrt(d2)
            ndot = _clamp((-dx * 0.45 - dy * 0.55 + 0.35), 0.0, 1.0)
            rgb = _lerp(shade, lit, ndot ** 1.4)
            a = 0.82 if d < 0.92 else 0.82 * (1.0 - d) / 0.08
            pix[y][x] = _mix(pix[y][x], rgb, a)


# Raster atlas of Courier New Bold. Rebuilt with: python3 marquee.py --rebuild-atlas
_ATLAS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "courier_new_bold.atlas")
_COURIER_PATHS = (
    "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
    "/Library/Fonts/Courier New Bold.ttf",
    "C:\\Windows\\Fonts\\courbd.ttf",
    "/usr/share/fonts/truetype/msttcorefonts/Courier_New_Bold.ttf",
)
_ATLAS_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ-."
_ATLAS = None


class _Atlas:
    def __init__(self, blob):
        data = zlib.decompress(blob)
        if data[:4] != b"CNB1":
            raise ValueError("bad courier atlas magic")
        self.cw, self.ch, self.advance, self.pad = struct.unpack(">HHHH", data[4:12])
        z = data.index(b"\0", 12)
        self.chars = data[12:z].decode("ascii")
        raw = data[z + 1:]
        n = self.cw * self.ch
        self._glyph = {c: raw[i * n:(i + 1) * n] for i, c in enumerate(self.chars)}

    def glyph(self, ch):
        return self._glyph.get(ch)


def _atlas():
    global _ATLAS
    if _ATLAS is None:
        with open(_ATLAS_PATH, "rb") as f:
            _ATLAS = _Atlas(f.read())
    return _ATLAS


def _blit_mask(pix, mask, mw, mh, dx, dy, dw, dh, rgb, alpha_scale=1.0):
    w, h = len(pix[0]), len(pix)
    if dw <= 0 or dh <= 0 or alpha_scale <= 0:
        return
    x0 = max(0, int(math.floor(dx)))
    y0 = max(0, int(math.floor(dy)))
    x1 = min(w - 1, int(math.ceil(dx + dw) - 1e-6))
    y1 = min(h - 1, int(math.ceil(dy + dh) - 1e-6))
    for y in range(y0, y1 + 1):
        v = ((y + 0.5) - dy) / dh * mh
        if v < 0 or v >= mh:
            continue
        yi = int(v)
        yf = v - yi
        yi2 = yi + 1 if yi + 1 < mh else yi
        row0 = yi * mw
        row1 = yi2 * mw
        for x in range(x0, x1 + 1):
            u = ((x + 0.5) - dx) / dw * mw
            if u < 0 or u >= mw:
                continue
            xi = int(u)
            xf = u - xi
            xi2 = xi + 1 if xi + 1 < mw else xi
            a00 = mask[row0 + xi]
            a01 = mask[row0 + xi2]
            a10 = mask[row1 + xi]
            a11 = mask[row1 + xi2]
            a = ((a00 * (1 - xf) + a01 * xf) * (1 - yf)
                 + (a10 * (1 - xf) + a11 * xf) * yf) / 255.0 * alpha_scale
            if a > 0.02:
                pix[y][x] = _mix(pix[y][x], rgb, a if a < 1 else 1.0)


def _char_advance(ch, em):
    # Period and space keep Courier's glyph, not its full em box — otherwise
    # "MS. PAC-MAN" reads as three words.
    if ch == ".":
        return em * 0.40
    if ch == " ":
        return em * 0.50
    return em


def _draw_text(pix, text, fill, outline, glow):
    atlas = _atlas()
    w, h = len(pix[0]), len(pix)
    units = sum(_char_advance(ch, 1.0) for ch in text)
    dest_em = min(38.0, (w - 22) / max(units, 1.0))
    scale = dest_em / atlas.advance
    dw = atlas.cw * scale
    dh = atlas.ch * scale
    tw = units * dest_em
    x0 = (w - tw) / 2 - atlas.pad * scale
    y0 = (h - dh) / 2
    xs = []
    x = x0
    for ch in text:
        xs.append(x)
        x += _char_advance(ch, dest_em)

    def pass_at(ox, oy, extra, rgb, a):
        for i, ch in enumerate(text):
            mask = atlas.glyph(ch)
            if not mask:
                continue
            _blit_mask(
                pix, mask, atlas.cw, atlas.ch,
                xs[i] + ox, y0 + oy,
                dw + extra, dh + extra, rgb, a,
            )

    pass_at(-2.2, -2.2, 4.4, glow, 0.32)
    pass_at(-1.0, -1.0, 2.0, outline, 1.0)
    pass_at(0.0, 0.0, 0.0, fill, 1.0)


def rebuild_atlas(dest=None):
    """Raster Courier New Bold into the committed atlas. Needs Pillow + the TTF."""
    from PIL import Image, ImageDraw, ImageFont
    font_path = next((p for p in _COURIER_PATHS if os.path.isfile(p)), None)
    if not font_path:
        raise FileNotFoundError("Courier New Bold .ttf not found")
    font = ImageFont.truetype(font_path, 64)
    advance = int(round(font.getlength("M")))
    pad, top, bottom = 4, 18, 55
    cw = advance + 2 * pad
    ch = (bottom - top) + 2 * pad
    raw = bytearray()
    for c in _ATLAS_CHARS:
        im = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(im).text((pad, pad - top), c, font=font, fill=255)
        raw.extend(im.tobytes())
    payload = (b"CNB1" + struct.pack(">HHHH", cw, ch, advance, pad)
               + _ATLAS_CHARS.encode("ascii") + b"\0" + bytes(raw))
    dest = dest or _ATLAS_PATH
    with open(dest, "wb") as f:
        f.write(zlib.compress(payload, 9))
    return dest


SCHEMES = {
    "pacman": {
        "text": "PAC-MAN",
        "void": (8, 4, 22),
        "nebula_a": (200, 80, 16),
        "nebula_b": (120, 20, 90),
        "nebula_c": (60, 24, 110),
        "neb1": (0.32, 0.42),
        "neb2": (0.78, 0.62),
        "seed": 1.7,
        "star": (255, 230, 160),
        "planet": (36.0, 118.0, 30.0),
        "planet_lit": (230, 170, 50),
        "planet_shade": (50, 20, 8),
        "spikes": ((18.0, 18.0), (268.0, 108.0)),
        "fill": (255, 228, 48),
        "outline": (12, 6, 0),
        "glow": (255, 180, 40),
        "lip": (48, 32, 8),
    },
    "mspacman": {
        "text": "MS PAC-MAN",
        "void": (4, 6, 22),
        "nebula_a": (40, 70, 190),
        "nebula_b": (150, 30, 110),
        "nebula_c": (20, 40, 120),
        "neb1": (0.28, 0.55),
        "neb2": (0.74, 0.38),
        "seed": 4.2,
        "star": (200, 220, 255),
        "planet": (258.0, 16.0, 32.0),
        "planet_lit": (110, 160, 245),
        "planet_shade": (12, 18, 60),
        "spikes": ((16.0, 112.0), (270.0, 20.0)),
        "fill": (255, 210, 80),
        "outline": (8, 4, 24),
        "glow": (255, 120, 180),
        "lip": (32, 40, 80),
    },
    "frogger": {
        "text": "FROGGER",
        "void": (2, 12, 10),
        "nebula_a": (12, 90, 50),
        "nebula_b": (20, 40, 90),
        "nebula_c": (8, 60, 40),
        "neb1": (0.70, 0.40),
        "neb2": (0.22, 0.68),
        "seed": 8.9,
        "star": (180, 255, 200),
        "planet": (40.0, 16.0, 32.0),
        "planet_lit": (50, 180, 90),
        "planet_shade": (4, 30, 18),
        "spikes": ((20.0, 110.0), (268.0, 18.0)),
        "fill": (180, 255, 70),
        "outline": (0, 12, 4),
        "glow": (80, 220, 120),
        "lip": (12, 40, 24),
    },
    "scramble": {
        "text": "SCRAMBLE",
        "void": (4, 6, 18),
        "nebula_a": (20, 40, 120),
        "nebula_b": (80, 30, 20),
        "nebula_c": (10, 20, 70),
        "neb1": (0.62, 0.38),
        "neb2": (0.28, 0.72),
        "seed": 11.3,
        "star": (220, 230, 255),
        "planet": (48.0, 20.0, 28.0),
        "planet_lit": (220, 170, 60),
        "planet_shade": (40, 18, 8),
        "spikes": ((18.0, 108.0), (266.0, 16.0)),
        "fill": (255, 230, 90),
        "outline": (8, 8, 24),
        "glow": (255, 180, 40),
        "lip": (16, 20, 48),
    },
    "galaga": {
        "text": "GALAGA",
        "void": (2, 4, 18),
        "nebula_a": (20, 40, 140),
        "nebula_b": (180, 30, 40),
        "nebula_c": (10, 20, 80),
        "neb1": (0.30, 0.40),
        "neb2": (0.72, 0.58),
        "seed": 17.4,
        "star": (255, 240, 200),
        "planet": (40.0, 90.0, 22.0),
        "planet_lit": (80, 220, 255),
        "planet_shade": (8, 20, 48),
        "spikes": ((20.0, 104.0), (262.0, 20.0)),
        "fill": (80, 220, 255),
        "outline": (4, 12, 40),
        "glow": (40, 140, 255),
        "lip": (8, 16, 48),
    },
    "galaxian": {
        "text": "GALAXIAN",
        "void": (4, 4, 16),
        "nebula_a": (30, 20, 90),
        "nebula_b": (180, 40, 20),
        "nebula_c": (20, 20, 80),
        "neb1": (0.35, 0.45),
        "neb2": (0.78, 0.62),
        "seed": 13.7,
        "star": (255, 240, 180),
        "planet": (220.0, 18.0, 30.0),
        "planet_lit": (255, 200, 40),
        "planet_shade": (40, 16, 8),
        "spikes": ((22.0, 106.0), (264.0, 18.0)),
        "fill": (255, 220, 50),
        "outline": (24, 8, 0),
        "glow": (255, 140, 30),
        "lip": (32, 16, 8),
    },
}


def render_marquee(path, scheme):
    """286×128 tile: starfield + Courier New Bold title."""
    spec = SCHEMES[scheme]
    w, h = 286, 128
    pix = [[spec["void"] for _ in range(w)] for _ in range(h)]
    _galaxy(pix, w, h, spec)
    _draw_text(pix, spec["text"], spec["fill"], spec["outline"], spec["glow"])

    lip = spec["lip"]
    for x in range(w):
        pix[0][x] = _lerp(pix[0][x], lip, 0.7)
        pix[h - 1][x] = _lerp(pix[h - 1][x], lip, 0.7)
        pix[1][x] = _lerp(pix[1][x], (0, 0, 0), 0.45)
        pix[h - 2][x] = _lerp(pix[h - 2][x], (0, 0, 0), 0.45)
    for y in range(h):
        pix[y][0] = _lerp(pix[y][0], lip, 0.7)
        pix[y][w - 1] = _lerp(pix[y][w - 1], lip, 0.7)
        pix[y][1] = _lerp(pix[y][1], (0, 0, 0), 0.45)
        pix[y][w - 2] = _lerp(pix[y][w - 2], (0, 0, 0), 0.45)

    rows = [bytearray(c for rgb in row for c in rgb) for row in pix]
    write_png(path, w, h, rows)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--rebuild-atlas"]
    if "--rebuild-atlas" in sys.argv[1:]:
        print("rebuilt", rebuild_atlas())
    out = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "assets")
    os.makedirs(out, exist_ok=True)
    names = ("pacman", "mspacman", "frogger", "galaga", "galaxian", "scramble")
    for name in names:
        render_marquee(os.path.join(out, f"{name}-cabinet.png"), name)
    print(f"wrote {len(names)} tiles to {out}")
