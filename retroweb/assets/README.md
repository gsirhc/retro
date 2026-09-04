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
