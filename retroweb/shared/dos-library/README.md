# DOS disk library (`shared/dos-library/`)

Builds the floppies behind the **Disk library** button on `ibmpc-at` and
`pc486`. Each machine lists its disks in its own `disks/library.json`; `make
-C retroweb/<machine> library` builds them into `disks/library/` and the web
Makefile's `library-images` copies them, with `library.json`, into
`web/disks/library/`. Images are git-ignored and rebuilt by CI.

- `fetch-shareware.sh` — the five shareware releases and freeware Tyrian
  2.1, pinned by SHA-256 into `cache/`.
- `build-library.py` — FAT12 images via mtools (`mformat`, `mmd`, `mcopy`).
  Formats: 360K, 1.2MB, 1.44MB. Volume serials are derived from the label so a
  rebuild is byte-stable apart from directory timestamps.

## `library.json`

| key | meaning |
|---|---|
| `id`, `title`, `year`, `publisher`, `type` | shown in the dialog |
| `needs` | what it takes to run |
| `description`, `help`, `license` | shown in the dialog; FreeDOS disks also get `help` in `README.TXT` |
| `images` | `[{ file, label }]`, one per disk, in order |
| `format` | size in K, only for an entry with no `build` (a prebuilt image) |
| `build.format` | 360, 1200 or 1440 |
| `build.packages` | FreeDOS package names, unpacked under `\NAME\` and run from the floppy |
| `build.install_dir` | set for a set too big for one disk: files are split across the disks and every disk carries the same `INSTALL.BAT` (COMMAND.COM re-reads it after each disk swap) and a `DISKn.TAG` it checks for before copying. It sticks to DOS 3.3 batch and XCOPY options (no `/Y`), since a visitor may have put an older DOS on C: |
| `build.shareware`, `build.disks` | a zip in `cache/` and the files on each disk |

The builder fails if a set needs more or fewer disks than `images` lists, or a
file won't fit.

## Provenance

**FreeDOS packages** come out of the FreeDOS 1.3 LiveCD ISO that pc486 already
fetches and verifies (`pc486/disks/fetch-freedos-cd.sh`, the zip's published
SHA-256). The live 1.3 package repository has drifted from the CD, so the CD is
the pinned source. Packages are copied unchanged, minus `SOURCE/`, `APPINFO/`
and the link batch files. Licenses are GPL, BSD or public domain per program;
each disk's `README.TXT` lists them from the package's LSM file.

| package | needs |
|---|---|
| senet, hangman, eliza, zmiy, e3, uhex, doszip, freemacs | 8086/286 |
| vitetris | `TETRIS16.EXE` on a 286 with NANSI.SYS; `TETRIS.EXE` is DJGPP, 386 |
| tde, pico, sudoku86, blkdrop, dn2, wing, bolitare | 386 (TDE's `TDER.EXE` uses a `66h` prefix, so it is 386-only too) |

NetHack is left out: its `NETHACK.EXE` alone is larger than a 1.44MB disk.

**Shareware**, each in the layout its publisher shipped:

| file | source | notes |
|---|---|---|
| `1keen.zip` | <https://image.dosgamesarchive.com/games/1keen.zip> | Commander Keen 1, v1.31 "BBS and Online Edition", DEICE set, files dated 1 Nov 1991 |
| `1wolf14.zip` | <https://www.gamers.org/pub/games/wolf3d/official/1wolf14.zip> | Wolfenstein 3-D 1.4. `VENDOR.DOC` says to put disk one's files plus as many `*.n` pieces as fit on each disk; on 1.44MB that's one disk. `CHKLIST.CPS` (an uploader's antivirus checksum) is left off |
| `doom19s.zip` | <https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/doom/doom19s.zip> (mirror of `ftp.idsoftware.com`) | DOOM 1.9 shareware. Disk 1 is `INSTALL.BAT`, `DEICE.EXE`, `DOOMS_19.DAT`, `DOOMS_19.1` and is full to the last cluster; disk 2 is `DOOMS_19.2` |

All three were released as shareware for free copying of the unmodified
episode; id's `doom19s.txt` marks DOOM "Electronic Distribution is Permitted"
and its installer says "PLEASE DISTRIBUTE!!!".

**Shareware**, used only for pc486's `?disk=duke3d` and `?disk=rott` preset
C:s, not library disks (each more than a floppy holds):

| file | source | notes |
|---|---|---|
| `1rott13.zip` | <https://icculus.org/rott/share/1rott13.zip> | Rise of the Triad: The HUNT Begins 1.3 shareware, Apogee's own release: `INSTALL.EXE`, `ROTTSW13.SHR`, `FILE_ID.DIZ`, dated 1 Aug 1995. MD5 `0fafd6b629eab80278fc726e31f9cf41` matches the one Linux packagers of the icculus port publish. A repack split into `ROT13BBS._1`–`._3` floppy pieces also circulates (dosgamesarchive.com) and isn't used |
| `3dduke13.zip` | <https://archive.org/download/3dduke13/3dduke13.zip> | Duke Nukem 3D 1.3D shareware, 3D Realms' own release: `INSTALL.EXE`, `DN3DSW13.SHR`, `LICENSE.TXT`, `FILE_ID.DIZ`, dated 24 Apr 1996. A repack split into `DUKE3DS._1`–`._5` floppy pieces also circulates (dosgamesarchive.com); it has no `LICENSE.TXT` and isn't used |

3D Realms' `LICENSE.TXT` lets individuals give copies away free and lets free
WWW and FTP sites "make the Game available for downloading", provided every
file is included unmodified. The installer copies `LICENSE.TXT` into
`C:\DUKE3D` with the game. Apogee's ROTT `VENDOR.DOC`, installed into
`C:\ROTT`, encourages everyone "to copy, upload and generally pass around
this Program without charging for it", on the same unmodified-files
condition.

**Freeware**, used only for pc486's `?disk=tyrian` preset C:, not a library
disk:

| file | source | notes |
|---|---|---|
| `tyrian21.zip` | <https://camanis.net/tyrian/tyrian21.zip> | Tyrian 2.1, Epic MegaGames / Eclipse, as an installed `TYRIAN\` directory, not an installer |

Tyrian's license is less clean than the shareware above. Neither Jason
Emery nor Epic left a license text with the freeware release. The closest
sources are the OpenTyrian project, which Emery gave the Tyrian 2.1 source
to, and its Camanis site, which hosts this zip:

- [OpenTyrian FAQ](https://github.com/opentyrian/opentyrian/wiki/FAQ):
  "Tyrian was, however, released as freeware (under no specific license)
  by Jason Emery in August 2004."
- [Camanis](https://www.camanis.net/): "Tyrian was released as freeware by
  Jason Emery in 2004."

Wikipedia and FreeBSD's port repeat the 2004 date. No original announcement
turned up. The files themselves are the 1995 retail release unchanged, so
`LICENSE.DOC`, `FILE_ID.DIZ`, the splash ("NOT LICENSED FOR COMMERCIAL
DISTRIBUTION") and the exit screen ("This game is NOT shareware. Please
enjoy it but don't copy it.") still carry the original terms.

## Disk swaps

Both PCs run the Bochs BIOS stand-in, which reports "drive present, does not
support change line" (rombios.c, INT 13h AH=15h/16h). DOS then can't use the
FDC's DSKCHG and falls back to its timing check, so a disk swapped and
confirmed within a second or two of the last access can be missed, and XCOPY
then reads the previous disk's cached directory. `INSTALL.BAT` checks for the
disk's `DISKn.TAG` and asks again ("Can't see disk n yet.") until DOS sees the
new disk.
