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
| `os` | `"cpm"` (default) or `"dos"` — picks which OS primer the **?** dialog appends |

An entry whose `file` is missing shows greyed-out "(not installed)". The **?**
button on a loaded drive opens a "How to use" dialog: the entry's `description`
and `help`, then a general CP/M (or Altair DOS) primer.

## Format

Standard MITS 8-inch images are **337,568 bytes** — 77 tracks × 32 sectors ×
137 bytes, a flat physical-sector dump. This is the format SIMH's `AltairZ80`
and David Hansel's Altair8800 simulator use, so images from those ecosystems
work as-is. (Mini-disk / Tarbell / Cromemco images are *not* this format and
won't boot here.)

## Where to get them

These images are **git-ignored** (`*.dsk`) — they bundle Digital Research's CP/M
and MITS software — so drop your own copies in here. The de-facto standard
collection is Mike Douglas's, mirrored at:

<https://github.com/dhansel/Altair8800/tree/master/disks>

| This project wants | dhansel file | contents |
|---|---|---|
| `cpm63k.dsk`   | `DISK01.DSK` | CP/M 2.2 (63K), bootable |
| `games.dsk`    | `DISK05.DSK` | CP/M game disk (boots CP/M) |
| `wordstar.dsk` | `DISK07.DSK` | WordStar 3.0 |
| `zork1.dsk`    | `DISK08.DSK` | Zork I |
| `altairdos.dsk`| `DISK02.DSK` | Altair DOS 1.0 |

```sh
cd Emulator8080/web/disks
curl -L -o cpm63k.dsk https://raw.githubusercontent.com/dhansel/Altair8800/master/disks/DISK01.DSK
```

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
