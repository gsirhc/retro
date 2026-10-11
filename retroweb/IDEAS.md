# retroweb feature ideas

Candidate features for the browser machines. None are started. Each one
follows the realism rules in the root `CLAUDE.md`: the realistic behavior
stays the default, and nothing here speeds up the CPU or terminal output.

## 1. Machine debugger

A debugger panel on each machine page, off by default. It shows:

- registers and flags, live while paused
- disassembly around PC, with symbols where known (Altair BASIC entry points,
  BIOS vectors)
- a memory dump and editor
- breakpoints on execute, and watchpoints on memory read/write and I/O port
- single-step, step-over, run to cursor
- an instruction history ring buffer (the last N instructions before a break)

Timing stays honest. The machine is either paused or running at real speed,
and while it's paused the wall clock isn't counted against the CPU.

The best fit is the Altair, with the debugger next to the front panel so that
stepping in the debugger and the EXAMINE/SINGLE STEP switches drive the same
state. Then the 6502 Assembler, ibmpc-at and pc486. The arcades come last,
since they're less useful to step through.

Work involved:

- a per-core debug interface in C++ (read/write registers, memory peek/poke
  without bus side effects, a breakpoint hook in the step loop)
- a disassembler per CPU (8080, 6502, Z80, 80286, 80486)
- a shared panel UI in `retroweb/shared/`, themed like the rest of the site
- the breakpoint check must cost nothing when no breakpoints are set (pc486
  already keeps its perf counters out of the shipped binary for the same
  reason)
- GoogleTest coverage of the debug interface, and Playwright coverage of
  every panel control

## 2. Save and restore the whole machine

Snapshot the full machine (CPU, RAM, video, every device, mounted media,
pending terminal output) and resume it later, even after closing the tab.

- saved locally in the browser (IndexedDB), with named slots
- export/import a snapshot as a file
- versioned format, so an old snapshot fails cleanly instead of loading
  half-wrong after a core change
- a restored machine resumes at real speed

This is a way past slow loads without speeding them up: load 8K BASIC once
at the real 10 cps, snapshot it, and resume from there next time.

Work involved:

- `serialize()`/`deserialize()` on every core and device, round-trip tested
  in GoogleTest (snapshot, run N cycles, compare against a machine that
  never stopped)
- care with in-flight state: disk rotation position, UART shift registers,
  audio buffers, tape position
- pc486 and ibmpc-at snapshots include large disk images, so store disk
  deltas rather than whole images

## 3. VT100 running its own firmware

Replace the Altair's VT100 terminal skin with an actual emulated VT100: an
8080 running DEC's original terminal ROM, wired to the Altair over the
serial line at the selected baud rate.

What that gets right that a terminal library doesn't:

- exact escape-sequence behavior, including the bugs software worked around
- SET-UP mode (SET-UP A/B screens, tab stops, baud rate, answerback)
- smooth scroll, double-width/double-height lines, the 132-column mode limits
- the real character ROM and the video timing that goes with it

Work involved:

- a second instance of the existing 8080 core, plus the VT100's video
  processor, keyboard scanning, and the 8251 USART
- the ROM is DEC firmware; check its redistribution status before bundling,
  and fall back to a user-supplied ROM if it can't ship
- keep the current terminal as the default until the VT100 is complete,
  then make the real VT100 the default and drop the skin
- tests: the VT100 self-test passes, SET-UP round-trips, and a known
  escape-sequence torture test matches real hardware captures

## 5. Larger disk library

The Altair and the PCs already swap disk images. Grow what's on offer, and
let changed disks leave the browser.

- Altair: more CP/M 2.2 disks (Zork, WordStar, MBASIC, assemblers,
  languages), where redistribution is clear
- ibmpc-at and pc486: freely licensed DOS software on floppy images
- a library picker that shows title, year, and what the disk needs (memory,
  drive count)
- a "save this disk" button on every drive, so a modified image downloads
  (some machines already have this; make it consistent)
- loading your own image from a file or by drag and drop, consistently
  across machines

Work involved:

- each image pinned and checksum-verified in a `fetch-*.sh` script, never
  committed (`*.dsk`/`*.bin` stay git-ignored)
- a short provenance note for every image (where it came from, license)
- Playwright tests that mount each library disk and prove it boots or runs

## 9. On-screen keyboards

Clickable keyboards drawn to match the real hardware, for phones and tablets
and for anyone who wants to see the original layout. Still to do:

- ASR-33 Teletype keyboard for the Altair, including HERE IS, REPT, and the
  real key positions (no lowercase)
- 6502 Assembler: its own keypad

ibmpc-at and pc486 already have one (the IBM Enhanced Keyboard,
`shared/pc-keyboard.js`), and the arcades have touch controls
(`shared/arcade-touch.js`). Follow the PC keyboard's pattern:

- drawn to scale from the real layout, keys sized relative to each other
- a Keyboard checkbox in the machine's control bar shows it under that bar;
  off by default on desktop, on by default on touch-only devices, remembered
- keys feed the same input path as the physical keyboard, at the same rate
  (no typing faster than the machine accepts input)
- modifier and repeat behavior matches the real keyboard (the ASR-33 has
  no auto-repeat except REPT; its CTRL and SHIFT latch on a click the way
  the PC keyboard's do)
- a real key press lights the drawn key, and lamps follow the machine's own
  state rather than a guess
- Playwright tests that click every key and check the bytes the machine
  receives

## 10. Recorded drive and Teletype sounds

The machines are silent apart from the Altair's bell. Real ones made a lot of
noise, and the noise told you what was going on. Use recordings of the actual
hardware, triggered by the emulated mechanism rather than by a timer.

- Altair 88-DCDD: head load solenoid, the step for each track, spindle hum
  while the motor runs
- ASR-33: print head and carriage per character, carriage return, the motor
  idling, the paper-tape punch and reader
- ibmpc-at and pc486: floppy seek and grind, hard disk spin-up and seek
  chatter, the PC speaker already exists
- cassette: the tones actually on the tape while it plays (optional,
  because it's loud)

The sound follows what the emulator does: a seek across 30 tracks takes 30
steps, at the real step rate. On by default at a sensible volume, with a
mute toggle per machine.

Work involved:

- recordings of real drives and a real Teletype, with a clear license; no
  synthesized stand-ins
- events from the cores (step, head load, motor on/off, character printed)
  sent to the page with nothing added to the hot path when sound is muted
- mixed into each machine's existing audio output, so the volume and mute
  controls work as they already do
- Playwright tests that a seek produces the right number of step events

## 11. Code editor next to the emulator

Write a program in the browser, assemble it, and run it on the emulated
machine straight away.

- an editor panel with syntax highlighting for the machine's assembler
  (8080 for the Altair, 6502 for the Assembler board, Z80 for the arcades)
- assemble in the browser, load into memory, run at real speed
- cycle counts per line, measured from the real CPU timing tables
- for the arcades, an overlay showing where the beam is when a line runs, so
  you can see whether your code made it before vblank
- example programs for each machine, starting with the self-test ROMs
  already in the repo

Work involved:

- an assembler compiled to wasm per CPU (cc65 for the 6502 is already in the
  repo; 8080 and Z80 need one)
- tie into the debugger (idea 1) so breakpoints and stepping work from the
  editor
- beam position comes from the video emulation's own counters, not an
  estimate
- save programs locally and export them as a file
- tests: an example program assembles, runs, and produces known output

## 12. A machine you can live in

Small things that make a visit feel like sitting at the machine.

- **Period date.** The guest clock starts on a date from the machine's era
  (configurable), not today. DOS file dates, CP/M timestamps and Y2K
  behavior then look the way they did.
- **Drag files into the guest.** Drop a file on the page and it appears on a
  floppy or a shared drive inside the guest OS, ready to open.
- **A hard disk that stays.** ibmpc-at and pc486 keep your changes to C:
  across visits by default, with a clear "reset to factory" button. pc486
  already persists its HDD; make it consistent.

Work involved:

- the date is set through the emulated RTC/CMOS (or the BIOS tick count on
  machines without one), not by changing the host clock
- dropped files are written to a FAT or CP/M image with the same tools
  already used to build the disk images
- persisted disks store deltas, the same approach as snapshots (idea 2)
- tests for each: guest reports the period date, a dropped file is readable
  in the guest, an edit to C: survives a reload

## 14. Dial-up to a BBS

A modem for pc486 (and later ibmpc-at) that dials out to telnet BBSes still
running today. Run a period terminal program inside DOS, dial a number, and
log in.

- an emulated Hayes-compatible modem on COM1/COM2 (AT commands, result
  codes, S-registers)
- the modem's own speed limits: 2400 and 14.4k at their real rates, not as
  fast as the connection allows
- dialing sounds and the handshake screech, from a real modem recording
- a phone book page mapping numbers to BBS addresses

Work involved:

- the browser can't open raw TCP, so this needs a small relay
  (WebSocket to telnet) on a server, with an allow-list of BBS hosts
- the relay is an outside service and needs hosting, rate limits and abuse
  handling; decide that before building it
- the 8250/16550 UART on these machines already exists, the modem sits
  behind it
- tests against a local relay and a fake BBS, so CI never dials out

## 15. Catalog pages for every disk and tape

Each piece of media in the library gets its own page with what you'd want to
know about it.

- title, publisher, year, and what it needs to run
- scans of the original label, sleeve or manual where they exist
- provenance: where the image came from, its checksum, and its license
- a "run it" button that opens the machine with that disk already mounted
- the catalog is browsable by machine, by year, and by type (OS, game,
  language, utility)

Work involved:

- a metadata file per image, next to the `fetch-*.sh` entry that pins it
- static pages generated at site build time, staged with the rest of
  `_site/`
- a URL that opens a machine with a given disk mounted, which also makes a
  run shareable
- scans and manuals need the same license check as the images
