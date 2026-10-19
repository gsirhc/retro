# Diskette images (`disks/`)

The front end's **88-DCDD drive cabinet** mounts 8-inch floppy images from this
directory and lists them in the *Insert Diskette* dialog. `manifest.json` is the
catalog; each entry is:

| key | meaning |
|---|---|
| `name` | shown in the picker |
| `file` | image file, fetched from `disks/` |
| `description` | shown when the entry is selected, and atop its help |
| `help` | multi-line text (use `\n`) for the drive's **?** button — how to run and quit this disk's programs |
| `os` | `"cpm"` (default), `"dos"` or `"basic"` — picks which primer the **?** dialog appends |
| `year`, `publisher` | shown in the picker and the **?** dialog (left out when unknown, e.g. compilations) |
| `needs` | what it takes to run: RAM, drives, terminal type |
| `license` | one line on its redistribution status, shown in the **?** dialog |

The picker only sends a `HEAD` per entry; an image is downloaded when it's
inserted. An entry whose `file` is missing shows greyed-out "(unavailable)". The **?**
button on a loaded drive opens a "How to use" dialog: the entry's `description`
and `help`, then a general CP/M (or Altair DOS) primer.

## Format

Standard MITS 8-inch images are **337,568 bytes** — 77 tracks × 32 sectors ×
137 bytes, a flat physical-sector dump: each sector is the 137 bytes the
controller reads after the sync bit, in the layout MITS documents in the Disk
BASIC manual (Appendix H). Images from the usual community collections work
as-is. (Mini-disk / Tarbell / Cromemco images are *not* this format and
won't boot here.)

## Where to get them

`*.dsk` is git-ignored. The shipped images come from Mike Douglas's
collection, mirrored at
<https://github.com/dhansel/Altair8800/tree/master/disks>, fetched pinned
(one commit, so this can't drift) and SHA-256 checked by `fetch-disks.sh`; CI
runs it before deploying to Pages.

```sh
./web/disks/fetch-disks.sh        # --force to re-fetch
```

| This project wants | dhansel file | contents |
|---|---|---|
| `cpm63k.dsk`    | `DISK01.DSK` | CP/M 2.2 (bootable) |
| `games.dsk`    | `DISK05.DSK` | CP/M game disk (boots CP/M) |
| `altairdos.dsk`| `DISK02.DSK` | Altair DOS 1.0 |
| `diskbasic.dsk`| `DISK03.DSK` | Altair Disk Extended BASIC 300-5-F (18 May 1979, bootable) |
| `basicprg.dsk` | `DISK04.DSK` | ~75 Disk BASIC programs (not bootable) |
| `cpm3boot.dsk` | `DISK13.DSK` | CP/M 3.0 non-banked, Altair 8" BIOS (bootable) |
| `cpm3util.dsk` | `DISK14.DSK` | CP/M 3.0 utilities (also bootable) |

`cpm3boot.dsk` and `cpm3util.dsk` are 337,664 bytes, 96 more than standard; the
88-DCDD mounts the first 337,568 and the tail is unused.

Redistribution:

- **`cpm63k.dsk`** — freely redistributable (DRDOS Inc. / Bryan Sparks, 2001,
  reconfirmed 2022; <http://cpm.z80.de/license.html>).
- **`cpm3boot.dsk`** / **`cpm3util.dsk`** — the same CP/M grant covers CP/M 3.
- **`games.dsk`** / **`altairdos.dsk`** / **`diskbasic.dsk`** /
  **`basicprg.dsk`** — decades-old, unchallenged,
  effectively-orphaned hobbyist / MITS-era material (no formal release letter
  for Altair DOS or Disk BASIC; MITS closed in 1979). Disk BASIC has the same
  standing as the 8K BASIC tapes in `../roms/`.

More MITS software (and the originals) live at
<https://deramp.com/downloads/mits/> and <https://altairclone.com>.

## Booting

Insert a bootable image in **drive A:** and press **BOOT** on the cabinet, or do
it the period way from the front panel: `EXAMINE` address `0xFF00`, then `RUN`.
`bootDisk()` in `wasm_machine.cpp` drops the 256-byte MITS bootstrap PROM
(`disk_bootrom.h`) at `0xFF00` and starts there. CP/M needs the full 64 KB of
RAM, which the machine already has.

## Writes

Diskette writes (`SAVE`, `ED`, `PIP`, …) go to the in-memory image for the
session. When a drive has unsaved changes a **SAVE** button appears on its bay —
it downloads the modified `.dsk`. Changes are lost on reload otherwise.
