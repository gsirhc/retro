#!/usr/bin/env python3
"""Build a PC machine's library floppies (FAT12, mtools) from its library.json."""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "cache")

# usable data bytes per format: clusters * cluster size from the BPB mformat writes
FORMATS = {
    360: {"sectors": 720, "cluster": 1024, "clusters": 354},
    1200: {"sectors": 2400, "cluster": 512, "clusters": 2371},
    1440: {"sectors": 2880, "cluster": 512, "clusters": 2847},
}
ROOT_ENTRIES = {360: 112, 1200: 224, 1440: 224}
SKIP_TOP = {"SOURCE", "APPINFO", "LINKS", "BATLINKS"}


def run(*cmd, **kw):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    return subprocess.run(cmd, check=True, env=env, **kw)


def die(msg):
    print("build-library: " + msg, file=sys.stderr)
    sys.exit(1)


def crlf(text):
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("cp437")


def on_disk(size, fmt):
    c = FORMATS[fmt]["cluster"]
    return (size + c - 1) // c * c


class Tree:
    """Files bound for a floppy: DOS path -> (bytes, zip date_time)."""

    def __init__(self):
        self.files = {}

    def add(self, path, data, when=None):
        path = path.upper()
        old = self.files.get(path)
        if old and old[0] != data:
            print(f"  note: {path} differs between sources, keeping the first", file=sys.stderr)
            return
        self.files[path] = (data, when)

    def size(self, fmt):
        dirs = {os.path.dirname(p) for p in self.files if os.path.dirname(p)}
        return sum(on_disk(len(d), fmt) for d, _ in self.files.values()) + \
            len(dirs) * FORMATS[fmt]["cluster"]


def iso_files(iso):
    """ISO9660 path -> (extent, size), from the primary volume descriptor."""
    out = {}
    with open(iso, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            die(f"{iso} is not an ISO9660 image")

        def walk(extent, size, prefix):
            f.seek(extent * 2048)
            data = f.read(size)
            pos = 0
            while pos < len(data):
                n = data[pos]
                if n == 0:
                    pos = (pos // 2048 + 1) * 2048
                    continue
                rec = data[pos:pos + n]
                ext, length = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
                name = rec[33:33 + rec[32]]
                pos += n
                if name in (b"\x00", b"\x01"):
                    continue
                name = name.decode("latin1").split(";")[0].rstrip(".").lower()
                if rec[25] & 2:
                    walk(ext, length, prefix + name + "/")
                else:
                    out[prefix + name] = (ext, length)

        root = pvd[156:190]
        walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")
    return out


_ISO = {}


def iso_package(iso, name, workdir):
    if iso not in _ISO:
        _ISO[iso] = iso_files(iso)
    hits = [p for p in _ISO[iso] if p.startswith("packages/") and p.endswith(f"/{name}.zip")]
    if len(hits) != 1:
        die(f"package {name}: expected one zip on the CD, found {hits}")
    ext, length = _ISO[iso][hits[0]]
    out = os.path.join(workdir, name + ".zip")
    with open(iso, "rb") as f, open(out, "wb") as o:
        f.seek(ext * 2048)
        o.write(f.read(length))
    return out


def lsm(zf):
    for n in zf.namelist():
        if n.upper().startswith("APPINFO/") and n.upper().endswith(".LSM"):
            fields = {}
            for line in zf.read(n).decode("latin1").splitlines():
                if ":" in line:
                    k, v = line.split(":", 1)
                    fields[k.strip().lower()] = v.strip()
            return fields
    return {}


def package_files(zpath, dirname, drop):
    """Package contents re-rooted under \\DIRNAME, the way it runs off the floppy."""
    out = []
    with zipfile.ZipFile(zpath) as zf:
        info = lsm(zf)
        for zi in zf.infolist():
            if zi.is_dir():
                continue
            parts = zi.filename.replace("\\", "/").split("/")
            top = parts[0].upper()
            if top in SKIP_TOP:
                continue
            rest = parts[1:] if top == "BIN" else parts[2:]
            if not rest or any(p.upper() == "SOURCE" for p in rest):
                continue
            rel = "/".join(rest)
            if any(rel.upper() == d.upper() or rel.upper().startswith(d.upper() + "/") for d in drop):
                continue
            out.append((f"{dirname}/{rel}", zf.read(zi), zi.date_time))
    return out, info


def make_image(path, fmt, label, tree):
    if tree.size(fmt) > FORMATS[fmt]["cluster"] * FORMATS[fmt]["clusters"]:
        die(f"{os.path.basename(path)}: {tree.size(fmt)} bytes won't fit a {fmt}K disk")
    if sum(1 for p in tree.files if "/" not in p) > ROOT_ENTRIES[fmt]:
        die(f"{os.path.basename(path)}: too many root entries")
    with open(path, "wb") as f:
        f.truncate(FORMATS[fmt]["sectors"] * 512)
    serial = int(hashlib.sha256(label.encode()).hexdigest()[:8], 16)
    run("mformat", "-i", path, "-f", str(fmt), "-v", label[:11], "-N", f"{serial:08x}", "::")
    made = set()
    with tempfile.TemporaryDirectory() as tmp:
        for dos, (data, when) in sorted(tree.files.items()):
            parts = dos.split("/")
            for i in range(1, len(parts)):
                d = "/".join(parts[:i])
                if d not in made:
                    run("mmd", "-i", path, "::" + d)
                    made.add(d)
            src = os.path.join(tmp, "f")
            with open(src, "wb") as f:
                f.write(data)
            if when:
                t = time.mktime(when + (0, 0, -1))
                os.utime(src, (t, t))
            run("mcopy", "-m", "-i", path, src, "::" + dos)
    used = tree.size(fmt)
    print(f"  {os.path.basename(path)}: {len(tree.files)} files, {used // 1024}K of {fmt}K")


def install_bat(title, dirname, count):
    # The Bochs BIOS reports no change line, so DOS can miss a quick swap and
    # read the old disk's cached directory; each disk's tag file catches that.
    lines = ["@ECHO OFF", f"ECHO Installing {title} to C:\\{dirname}", f"IF NOT EXIST C:\\{dirname}\\NUL MD C:\\{dirname}"]
    for n in range(1, count + 1):
        if n > 1:
            lines += [
                f":DISK{n}",
                "ECHO.",
                f"ECHO Put in disk {n} of {count}, then press any key.",
                "PAUSE >NUL",
                f"IF EXIST A:\\DISK{n}.TAG GOTO COPY{n}",
                f"ECHO Can't see disk {n} yet.",
                f"GOTO DISK{n}",
                f":COPY{n}",
            ]
        # no /Y and no >NUL: XCOPY before DOS 6.2 rejects /Y, and a later one's overwrite prompt must show
        lines.append(f"XCOPY A:\\{dirname}\\*.* C:\\{dirname} /S /E")
    lines += ["ECHO.", f"ECHO Done. {title} is in C:\\{dirname}."]
    return "\n".join(lines) + "\n"


def readme(entry, programs, n, count):
    out = [entry["title"]]
    if count > 1:
        out.append(f"Disk {n} of {count}")
    out += ["", entry.get("help", "").rstrip(), ""]
    if programs:
        out.append("What's here:")
        for p in programs:
            ver = f" {p['version']}" if p.get("version") else ""
            out.append(f"  {p['dir']:<9} {p['title']}{ver}. {p['license']}")
        out.append("")
        out.append("These come from the FreeDOS 1.3 LiveCD's package set, unchanged.")
    return "\n".join(out) + "\n"


def build_packages(entry, iso, outdir):
    b = entry["build"]
    fmt = b["format"]
    programs = []
    payload = []
    with tempfile.TemporaryDirectory() as work:
        for spec in b["packages"]:
            if isinstance(spec, str):
                spec = {"name": spec}
            name = spec["name"]
            dirname = spec.get("dir", name).upper()
            files, info = package_files(iso_package(iso, name, work), dirname, spec.get("drop", []))
            if not files:
                die(f"package {name}: nothing to copy")
            programs.append({
                "dir": dirname,
                "title": info.get("title", name),
                "version": info.get("version", ""),
                "license": info.get("copying-policy", "see the package docs"),
            })
            payload += files

    # first-fit by directory order, keeping each program's files together where they fit
    count = len(entry["images"])
    disks = [Tree() for _ in range(count)]
    cap = FORMATS[fmt]["cluster"] * FORMATS[fmt]["clusters"] - 3 * FORMATS[fmt]["cluster"]
    d = 0
    for path, data, when in payload:
        while d < count:
            disks[d].add(path, data, when)
            if disks[d].size(fmt) <= cap:
                break
            del disks[d].files[path.upper()]
            d += 1
        if d == count:
            die(f"{entry['id']}: needs more than the {count} disk(s) library.json lists")
    for i, t in enumerate(disks):
        if not t.files:
            die(f"{entry['id']}: library.json lists {count} disks but disk {i + 1} would be empty")
        t.add("README.TXT", crlf(readme(entry, programs, i + 1, count)))
        if count > 1:
            t.add("INSTALL.BAT", crlf(install_bat(entry["title"], b["install_dir"], count)))
            t.add(f"DISK{i + 1}.TAG", crlf(f"{entry['title']}, disk {i + 1} of {count}\n"))
        img = entry["images"][i]
        make_image(os.path.join(outdir, img["file"]), fmt, b.get("label", entry["id"]).upper() + (str(i + 1) if count > 1 else ""), t)


def build_shareware(entry, outdir):
    b = entry["build"]
    zpath = os.path.join(CACHE, b["shareware"])
    if not os.path.exists(zpath):
        die(f"{zpath} missing; run fetch-shareware.sh first")
    with zipfile.ZipFile(zpath) as zf:
        byname = {zi.filename.upper(): zi for zi in zf.infolist()}
        if len(b["disks"]) != len(entry["images"]):
            die(f"{entry['id']}: build.disks and images disagree")
        for i, names in enumerate(b["disks"]):
            t = Tree()
            for n in names:
                zi = byname.get(n.upper()) or die(f"{entry['id']}: {n} not in {b['shareware']}")
                # unzip, not zipfile: the 1992 releases use PKZIP 1.x implode
                data = subprocess.run(["unzip", "-p", zpath, zi.filename], check=True, capture_output=True).stdout
                if len(data) != zi.file_size:
                    die(f"{entry['id']}: {n} came out {len(data)} bytes, expected {zi.file_size}")
                t.add(n, data, zi.date_time)
            label = b.get("label", entry["id"]).upper() + (str(i + 1) if len(b["disks"]) > 1 else "")
            make_image(os.path.join(outdir, entry["images"][i]["file"]), b["format"], label, t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", required=True)
    ap.add_argument("--iso", required=True, help="FreeDOS 1.3 LiveCD (pc486/disks/freedos-cd.iso)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    if not os.path.exists(args.iso):
        die(f"{args.iso} missing; run 'make -C retroweb/pc486 disks/freedos-cd.iso'")
    os.makedirs(args.out, exist_ok=True)
    with open(args.library) as f:
        lib = json.load(f)
    for entry in lib["disks"]:
        b = entry.get("build")
        if not b:
            continue
        print(f"build-library: {entry['id']}")
        if "packages" in b:
            build_packages(entry, args.iso, args.out)
        elif "shareware" in b:
            build_shareware(entry, args.out)
        else:
            die(f"{entry['id']}: unknown build kind")


if __name__ == "__main__":
    main()
