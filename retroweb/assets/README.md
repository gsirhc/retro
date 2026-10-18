# landing-page assets

- **`altair-panel.jpg`** — a screenshot of *this project's* rendered Altair 8800
  front panel (`#altair .fp-case`), captured from the emulator running Kill the
  Bit: MEMR / M1 / WO status lamps lit, the rotating bit on A11, the loop
  address on A0–A3. Not a photo of real hardware.

  Used as a landing-page tile (286×128 JPEG) — the LED labels aren't meant
  to be legible at that size, just the panel's shape.

  To regenerate after a panel restyle: build the front end
  (`make -C ../altair8800/web retro8080.js roms`), serve it, load
  `/?test=1&preset=baremetal`, click `#ptr .ptr-load`, wait for
  `__test.leds().addr & 0xff00`, then screenshot the `#altair .fp-case`
  element and resize to 286 px wide (progressive JPEG q≈88).

- **`doom-e1m1.jpg`**: the first room of E1M1 in shareware DOOM, turned
  toward the window and mountains, from *this project's* 486 DX2-66 running
  the `?disk=doom` preset C:. A new game (Esc, Enter ×3) on
  `pc486/?test=1&fast=1&disk=doom`, two steps forward and one turn right,
  then a frame read off `__test.frameCanvas` (320×200), rows 25 to 168
  cropped and resized to 286×128 (JPEG q90). Gameplay, not the title
  screen, so no logo or title art.

- **`wolf3d-e1m1.jpg`**: the first cell of E1M1 in shareware Wolfenstein 3-D,
  turned a little left, from *this project's* 486 DX2-66 running the
  `?disk=wolf3d` preset C:. Space, Space, Esc, Enter ×3 on
  `pc486/?test=1&fast=1&disk=wolf3d` starts a new game; one left turn, then
  a frame read off `__test.frameCanvas`, cropped inside the view border down
  to the bottom of the view and resized to 286×128 (JPEG q90).

- **`duke3d-e1l1.jpg`**: shareware Duke Nukem 3D's attract demo, a Pig Cop
  in the E1L1 adult bookstore, from *this project's* 486 DX2-66 core booting
  the `?disk=duke3d` preset C:. A 320×200 frame rendered by `RenderScreen`
  about 4.8 billion cycles after power-on, rows 20 to 162 (below the
  UNREGISTERED SHAREWARE tag, above the status bar) resized to 286×128
  (JPEG q90). Its card sets `data-title-y="30%"` so the libretro title
  screen shows the logo, not just the mushroom cloud.

- **`rott-demo.jpg`**: shareware Rise of the Triad's attract demo, two
  guards on a rooftop walkway with the dual pistols up, from *this
  project's* 486 DX2-66 core booting the `?disk=rott` preset C:. A 320×200
  frame rendered by `RenderScreen` about 7.6 billion cycles after power-on,
  rows 22 to 164 (between the score bar and the health bar) resized to
  286×128 (JPEG q90). Its card also sets `data-title-y="30%"`.

- **`keen1-mars.jpg`**: shareware Commander Keen 1 on the Mars overworld map,
  with Keen and the Bean-with-Bacon Megarocket, from *this project's* IBM
  PC/AT running the `?disk=keen1` preset C:. Space at the title on
  `ibmpc-at/?test=1&fast=1&disk=keen1` starts a game; a frame read off
  `__test.frameCanvas` (320×200), top 143 rows resized to 286×128 (JPEG q90).

- **`tyrian-demo.jpg`**: freeware Tyrian 2.1's attract demo, from *this
  project's* 486 DX2-66 core booting the `?disk=tyrian` preset C: with FM
  music and Sound Blaster effects on. A 320×200 frame rendered by
  `RenderScreen` about 2.6 billion cycles after power-on, cropped to the
  playfield below the INSERT COIN line (x 0–262, y 58–175) and resized to
  286×128 (JPEG q90).

- **`altair-startrek.jpg`**: Super Star Trek's opening short range scan on
  the VT100, from *this project's* Altair 8800 running the `?tape=startrek`
  quick play (8K BASIC off paper tape, Star Trek off the 88-ACR cassette,
  both at 50×). Load `altair8800/?test=1&tape=startrek`, answer Y at
  PRESS Y TO ACCEPT COMMAND?, then screenshot `#screen` at 2× device scale;
  the scan box and status column (x 0–1112, y 378–876 of 1556×922) resized
  to 286×128 (JPEG q90).

- **`altair-cpm.jpg`**: CP/M 2.2 (the MITS 63K build) after a `DIR` of the
  boot diskette, from *this project's* Altair 8800 running the `?disk=cpm`
  quick play (CP/M preset, BOOT with the 88-DCDD at 50×). Load
  `altair8800/?test=1&disk=cpm`, type `DIR` at A>, then screenshot
  `#screen` at 2× device scale; the listing from `A>DIR` to the next A>
  (x 0–932, y 226–643 of 1556×922) resized to 286×128 (JPEG q90).

  These eight are the shipped Ready to Play tiles. The landing page's
  Thumbnails dropdown (Original by default) can swap in libretro-thumbnails'
  DOS screenshot, title screen, or box art (`data-art`, cropped by
  `data-<style>-y`), fetched by the visitor's browser. Star Trek and CP/M
  have no `data-art` (they aren't DOS titles), so they always show their own
  tiles. None
  of it is staged.

- **`ibmpcat-panel.jpg`** — a screenshot of *this project's* rendered IBM
  PC/AT front panel (`#frontPanelCard .at-case`), captured from the emulator
  after auto-boot with a diskette inserted in drive A: (power LED lit,
  A: shown loaded, B: empty) -- not a photo of real hardware. The real 5170
  case fascia is a wide, short rectangle, so this thumbnail is naturally
  wider than the other cards' (712×128 vs. 286×128) rather than distorted
  to match; the 3-column tile crop (`object-fit: cover`, left-anchored)
  keeps the badge and power LED in frame.

  `.at-case` also contains each drive bay's page-UI chrome (`.at-bay-ctl`
  -- the A:/B: letter and filename/status text under each faceplate).
  Unlike `altair8800`'s `#altair .fp-case` (which has no controls inside
  it at all), a bare screenshot of `.at-case` picks that up too. Hide
  `.at-bay-ctl` first (inject `.at-bay-ctl { display: none !important; }`)
  so the screenshot shows only the case, switch, and drive faceplates.

  To regenerate: build the front end (`make -C ../ibmpc-at/web`, plus
  `roms`/`hdd-image` if not already built), serve it, load `/?test=1`, wait
  for `#powerLed.power-on`, insert any small file into drive A:'s file
  input, add a style tag hiding `.at-bay-ctl` (see above), then screenshot
  the `#frontPanelCard .at-case` element and resize to 128 px tall
  (progressive JPEG q≈88).

- **`pacman-cabinet.png`** / **`mspacman-cabinet.png`** /
  **`frogger-cabinet.png`** / **`galaga-cabinet.png`** / **`galaxian-cabinet.png`** /
  **`scramble-cabinet.png`** — original
  landing-page tiles. Spec, pinned by
  each arcade machine's `web/tests/home.spec.ts`:

  - **286×128** PNG (same box as `altair-panel.jpg`)
  - **Courier New Bold** titles from `retroweb/shared/courier_new_bold.atlas`
    (a raster of that system typeface — not a cabinet logotype, character
    sprite, or manufacturer mark)
  - generated starfield (nebula + planet, tinted per game)

  Built by `retroweb/shared/marquee.py` (`python3 retroweb/shared/marquee.py
  retroweb/assets`; atlas rebuild `--rebuild-atlas`). Also invoked from
  each machine's `gen_hwtest.py`.

  These are the only arcade tiles that ship. The hub's Thumbnails dropdown
  (Original by default) has the visitor's browser load each card's `data-art`
  image from libretro-thumbnails
  (`thumbnails.libretro.com/MAME/{Named_Snaps,Named_Titles,Named_Boxarts}/<MAME description>.png`),
  or its `data-marquee` marquee from the Arcade Database
  (`adb.arcadeitalia.net/media/mame.current/marquees/<set>.png`).
  None of that art is committed or staged.

- **`pc486-panel.jpg`** — a screenshot of *this project's* rendered 486
  front-panel turbo cluster (`#frontPanelCard .at-case` with `.at-drives`
  hidden), captured after auto-boot with Power + Turbo LEDs lit and the
  seven-segment readout on 66 -- not a photo of real hardware. Drive bays
  are trimmed so the left island fills a standard 286×128 card without
  squashing the whole fascia the way a full-width case shot would. Same
  progressive JPEG q≈88 as the Altair/AT tiles.

  `.at-case` also contains each drive bay's page-UI controls (`.at-bay-ctl`);
  hide those together with `.at-drives` before capture so the shot is
  hardware chrome only.

  To regenerate: build/serve the front end (`make -C ../pc486/web`, plus
  `roms`/`hdd-image` if needed), load `/?test=1&fast=1`, wait for
  `#powerLed.power-on`, inject
  `.at-drives, .at-bay-ctl { display: none !important; }` and
  `.at-case { width: max-content !important; }`, screenshot
  `#frontPanelCard .at-case`, then fit into 286×128 (uniform scale, beige
  letterbox) as a progressive JPEG q≈88.
