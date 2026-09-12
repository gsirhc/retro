# landing-page assets

- **`altair-panel.jpg`** — a screenshot of *this project's* rendered Altair 8800
  front panel (`#altair .fp-case`), captured from the emulator running Kill the
  Bit: MEMR / M1 / WO status lamps lit, the rotating bit on A11, the loop
  address on A0–A3. Not a photo of real hardware.

  Used as a small (~64 px tall) card thumbnail, so it's a 286×128 JPEG — the
  LED labels aren't meant to be legible at that size, just the panel's shape.

  To regenerate after a panel restyle: build the front end
  (`make -C ../altair8800/web retro8080.js roms`), serve it, load
  `/?test=1&preset=baremetal`, click `#ptr .ptr-load`, wait for
  `__test.leds().addr & 0xff00`, then screenshot the `#altair .fp-case`
  element and resize to 286 px wide (progressive JPEG q≈88).

- **`ibmpcat-panel.jpg`** — a screenshot of *this project's* rendered IBM
  PC/AT front panel (`#frontPanelCard .at-case`), captured from the emulator
  after auto-boot with a diskette inserted in drive A: (power LED lit,
  A: shown loaded, B: empty) -- not a photo of real hardware. The real 5170
  case fascia is a wide, short rectangle, so this thumbnail is naturally
  wider than the other cards' (712×128 vs. 286×128) rather than distorted
  to match; `.machine-card .shot`'s `height:64px;width:auto` CSS already
  expects each card's thumbnail to keep its own natural aspect ratio.

  `.at-case` also contains each drive bay's own page-UI controls
  (`.at-bay-ctl` -- the Insert.../Eject buttons and filename/status text),
  nested right alongside the visual drive slot for this page's own layout
  convenience. Unlike `altair8800`'s `#altair .fp-case` (which has no
  controls inside it at all), a bare screenshot of `.at-case` picks up that
  UI chrome too -- an earlier version of this thumbnail did exactly that,
  which is why it read as cluttered/button-covered next to the other two
  cards' clean hardware-only shots. Hide `.at-bay-ctl` first (inject
  `.at-bay-ctl { display: none !important; }`) so the screenshot shows only
  the case, switch, and drive slots -- real hardware has no such buttons to
  begin with.

  To regenerate: build the front end (`make -C ../ibmpc-at/web`, plus
  `roms`/`hdd-image` if not already built), serve it, load `/?test=1`, wait
  for `#powerLed.power-on`, insert any small file into drive A:'s file
  input, add a style tag hiding `.at-bay-ctl` (see above), then screenshot
  the `#frontPanelCard .at-case` element and resize to 128 px tall
  (progressive JPEG q≈88).
