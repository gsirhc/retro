"""Writes VGA-ROM.F08 and VGA-ROM.F14 from Yossi Gil's fntcol16.zip as nasm includes."""
import sys
import zipfile

zf = zipfile.ZipFile(sys.argv[1])
for member, height, out in (("VGA-ROM.F08", 8, "font8.inc"), ("VGA-ROM.F14", 14, "font14.inc")):
    data = zf.read(member)
    assert len(data) == 256 * height, (member, len(data))
    with open(out, "w") as f:
        for i in range(0, len(data), height):
            f.write("        db " + ",".join("0x%02X" % b for b in data[i:i + height]) + "\n")
