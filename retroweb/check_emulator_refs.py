"""Fails if a tracked file names another emulator outside the sanctioned interop uses (the Bochs BIOS is guest firmware)."""
import re
import subprocess
import sys

NAMES = re.compile(r"\b(?:mame|simh|altirra|nuked|dosbox|86box|pcem|qemu|mister|fbneo|finalburn|couriersud|hiscore\.dat)\b", re.I)
SKIP_FILES = {"CLAUDE.md", "retroweb/about.html", "retroweb/news_info.html", "retroweb/check_emulator_refs.py"}
SKIP_DIRS = ("node_modules/", "vendor/")
ALLOWED_LINES = (
    "thumbnails.libretro.com/",
    "adb.arcadeitalia.net/media/mame.current/",
    '"retroweb.arcadeArt", "MAME"',
    "qemu/vgabios",
    "DosBox/QEMU/VirtualBox/VMware",
)


def offending(path, text):
    for n, line in enumerate(text.splitlines(), 1):
        if NAMES.search(line) and not any(a in line for a in ALLOWED_LINES):
            yield n, line.strip()


def selftest():
    assert list(offending("x.cpp", "// ported from MAME foo.cpp"))
    assert list(offending("x.md", "SIMH latches it"))
    assert not list(offending("x.js", 'const L = "https://thumbnails.libretro.com/MAME/";'))
    assert not list(offending("x.cpp", "// mamba, simhash"))


def main():
    if "--selftest" in sys.argv:
        selftest()
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
    files = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, text=True, check=True).stdout.split("\0")
    bad = 0
    for f in files:
        if not f or f in SKIP_FILES or any(d in f for d in SKIP_DIRS):
            continue
        try:
            with open(f"{root}/{f}", encoding="utf-8") as fh:
                text = fh.read()
        except (UnicodeDecodeError, FileNotFoundError, IsADirectoryError):
            continue
        for n, line in offending(f, text):
            print(f"{f}:{n}: {line[:160]}")
            bad += 1
    if bad:
        print(f"{bad} emulator reference(s); see CLAUDE.md \"Never copy or reference other emulators\"", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
