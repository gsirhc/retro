"""Converts vgafont8 and vgafont14 from vgabios's vgafonts.h into nasm includes."""
import re
import sys

src = open(sys.argv[1]).read()
for name, height, out in (("vgafont8", 8, "font8.inc"), ("vgafont14", 14, "font14.inc")):
    body = re.search(r"%s\[256\*%d\]\s*=\s*\{(.*?)\}" % (name, height), src, re.S).group(1)
    data = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]{2}", body)]
    assert len(data) == 256 * height, (name, len(data))
    with open(out, "w") as f:
        for i in range(0, len(data), height):
            f.write("        db " + ",".join("0x%02X" % b for b in data[i:i + height]) + "\n")
