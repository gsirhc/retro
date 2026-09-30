"use strict";
(() => {
  // ---- page theme (Win95 / mid-90s Mosaic web / Modern / Dark Modern) ---
  // Shared with every other page on the site via the retro8080.theme
  // localStorage key -- a theme picked here or on the landing page carries
  // across. See shared/theme-picker.js for the actual mechanism.
  initThemePicker();

  // "Last built" = the wasm's own mtime on the server -- same mechanism as
  // altair8800's and assembler6502's own footers.
  (async () => {
    const el = document.getElementById("buildDate");
    for (const url of ["pc486.wasm", "pc486.js", "app.js"]) {
      try {
        const r = await fetch(url, { method: "HEAD", cache: "no-store" });
        const lm = r.headers.get("Last-Modified");
        if (lm) {
          el.textContent = new Date(lm).toLocaleString(undefined,
            { year: "numeric", month: "long", day: "numeric", hour: "numeric", minute: "2-digit" });
          return;
        }
      } catch {}
    }
    /* v8 ignore next -- only if every HEAD request fails / lacks Last-Modified */
    el.textContent = "unknown";
  })();

  // Automated-test-only CPU speed multiplier: `?test=1&fast=1`. A real visitor
  // has no control that reaches this -- it exists solely so the Playwright
  // suite (whose real cost is a genuine POST + FreeDOS boot at real 66 MHz,
  // not just a device-transfer wait) doesn't pay that in full on every test.
  // `?test=1` alone still runs the real, wall-clock-paced 66 MHz clock -- the
  // suite's shared boot() helper opts most tests into `fast=1` explicitly,
  // and a couple of smoke tests deliberately don't, to verify the real-speed
  // contract itself still holds. See CLAUDE.md "Current sanctioned
  // overrides" (automated-test CPU clock multiplier). The multiplier value
  // (20, matching ibmpc-at's) is a starting point, not yet tuned against
  // this machine's own real boot time -- flagging per PC486_REVIEW.md
  // rather than asserting it's already the right number.
  const testParams = new URLSearchParams(location.search);
  const TEST_CPU_MULTIPLIER =
    testParams.get("test") === "1" && testParams.get("fast") === "1" ? 20 : 1;

  // Opt-in diagnostic capture, not part of the machine: `?fmtrace` exposes
  // console hooks to record every OPL3 register write with its CPU cycle
  // stamp, for offline analysis of a real DOS game's FM music. Independent
  // of `?test=1`/`?perf`; costs nothing unless a visitor calls __fm.start().
  if (testParams.has("fmtrace")) {
    window.__fm = {
      start: (n) => machine.fmStartTrace(n || 400000),
      save: () => {
        const events = machine.fmDrainTrace();
        const blob = new Blob([JSON.stringify(events)], { type: "application/json" });
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = "fmtrace.json";
        a.click();
        URL.revokeObjectURL(url);
      },
    };
  }

  // ---- keyboard: physical key -> real IBM AT Set 1 scan code -----------
  // i8042.h's inject_scancode() is a verbatim Set-1 pass-through (see its
  // header) -- this table supplies exactly what a real AT keyboard's own
  // Set-2-to-Set-1 translation would hand the host. A plain number is a
  // one-byte code; a two-entry array is an 0xE0-prefixed "extended" key
  // (real hardware fact: the second AT keyboard block -- right Ctrl/Alt,
  // the arrow/Insert/Delete/Home/End/PageUp/PageDown cluster, numpad
  // Enter/Divide -- all send this prefix precisely because they were
  // added after the original 84-key layout already claimed every
  // unprefixed code). Break code = make code with bit 7 set, on whichever
  // byte carries the actual key (never the 0xE0 prefix itself).
  const SET1 = {
    Escape: 0x01,
    Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06,
    Digit6: 0x07, Digit7: 0x08, Digit8: 0x09, Digit9: 0x0A, Digit0: 0x0B,
    Minus: 0x0C, Equal: 0x0D, Backspace: 0x0E,
    Tab: 0x0F, KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14,
    KeyY: 0x15, KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19,
    BracketLeft: 0x1A, BracketRight: 0x1B, Enter: 0x1C,
    ControlLeft: 0x1D,
    KeyA: 0x1E, KeyS: 0x1F, KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23,
    KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26,
    Semicolon: 0x27, Quote: 0x28, Backquote: 0x29,
    ShiftLeft: 0x2A, Backslash: 0x2B,
    KeyZ: 0x2C, KeyX: 0x2D, KeyC: 0x2E, KeyV: 0x2F, KeyB: 0x30, KeyN: 0x31, KeyM: 0x32,
    Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36,
    NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39, CapsLock: 0x3A,
    F1: 0x3B, F2: 0x3C, F3: 0x3D, F4: 0x3E, F5: 0x3F, F6: 0x40,
    F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44,
    NumLock: 0x45, ScrollLock: 0x46,
    Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4A,
    Numpad4: 0x4B, Numpad5: 0x4C, Numpad6: 0x4D, NumpadAdd: 0x4E,
    Numpad1: 0x4F, Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52, NumpadDecimal: 0x53,
    F11: 0x57, F12: 0x58,
    ControlRight: [0xE0, 0x1D], AltRight: [0xE0, 0x38],
    Insert: [0xE0, 0x52], Delete: [0xE0, 0x53],
    Home: [0xE0, 0x47], End: [0xE0, 0x4F], PageUp: [0xE0, 0x49], PageDown: [0xE0, 0x51],
    ArrowUp: [0xE0, 0x48], ArrowLeft: [0xE0, 0x4B], ArrowRight: [0xE0, 0x4D], ArrowDown: [0xE0, 0x50],
    NumpadEnter: [0xE0, 0x1C], NumpadDivide: [0xE0, 0x35],
    // Print Screen and Pause/Break don't fit the simple prefix+break-bit
    // convention above -- real AT hardware sends each as its own fixed byte
    // sequence. Print Screen: a real 4-byte E0-prefixed make and a distinct
    // 4-byte break. Pause/Break: one fixed 6-byte sequence sent entirely on
    // press, with NO break code at all -- genuine, well-documented AT
    // keyboard controller behavior (Scan Code Set 1), not an emulator
    // simplification.
    PrintScreen: { make: [0xE0, 0x2A, 0xE0, 0x37], break: [0xE0, 0xB7, 0xE0, 0xAA] },
    Pause: { make: [0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5], break: [] },
  };

  let machine = null;
  // The powered-off machine's embind handle, kept only so powerOn() can
  // free it (see there). Never used to run anything.
  let lastMachine = null;

  // The 8042 model has one single-byte output register, exactly like real
  // hardware -- a second byte written before the guest's IRQ1 handler has
  // read the first just overwrites it, the byte never delivered. A real
  // keyboard can't outrun that (it clocks one bit at a time over a slow
  // serial line), but a JS loop calling injectScancode() twice in the same
  // synchronous turn can: nothing runs the emulator's real-time run loop
  // (rAF-paced) in between, so the CPU never gets a chance to read byte one
  // before byte two clobbers it. Any multi-byte Set 1 sequence -- every
  // E0-prefixed extended key (arrows, Home/End/PgUp/PgDn, Insert/Delete,
  // NumpadEnter/Divide, CtrlRight/AltRight), Print Screen's 4-byte
  // sequences, Pause's 6-byte sequence, and the Ctrl-Alt-Del combo below --
  // needs real spacing between EVERY byte, not just between make and
  // break. See IBM_PCAT_REVIEW.md.
  // gapMs defaults to 20 -- enough for INT 9 to drain one byte on an idle
  // host. Longer sequences that the BIOS's own multi-key checks depend on
  // (Ctrl-Alt-Del) pass 50 so a contended main thread cannot collapse the
  // makes into the 8042's single-byte buffer.
  function injectScancodeSequence(codes, gapMs = 20) {
    let i = 0;
    (function step() {
      if (!machine || i >= codes.length) return;
      machine.injectScancode(codes[i++]);
      if (i < codes.length) setTimeout(step, gapMs);
    })();
  }

  function sendKey(code, isBreak) {
    const entry = SET1[code];
    if (entry === undefined || !machine) return;
    if (entry && typeof entry === "object" && !Array.isArray(entry)) {
      // Fixed, non-standard scancode sequences that don't fit the simple
      // "prefix bytes + break-bit-on-the-last-byte" convention every other
      // key uses -- Print Screen's real make/break are each their own
      // 4-byte E0-prefixed sequences, and Pause/Break sends one fixed
      // 6-byte sequence on press and has no real break code at all (a
      // genuine, well-documented AT keyboard quirk -- see the comment you
      // add at the SET1 entry).
      injectScancodeSequence(isBreak ? entry.break : entry.make);
      return;
    }
    const bytes = Array.isArray(entry) ? entry.slice() : [entry];
    const last = bytes.length - 1;
    bytes[last] = isBreak ? (bytes[last] | 0x80) : bytes[last];
    injectScancodeSequence(bytes);
  }
  // Tracks which physical keys are currently down so a lost keyup -- focus
  // moving off screenEl mid-hold, the tab losing visibility, or Pointer
  // Lock releasing (which a captured mouse's own Escape handling can
  // trigger without ever dispatching that Escape to the page at all, see
  // below) -- can't leave a key permanently "held" from the guest's side.
  // A real keyboard has no such failure mode (its own up-transition is
  // physical, not routed through a page that can lose focus), so this is
  // purely a browser-integration safety net, not a hardware behavior.
  const heldKeys = new Set();
  function releaseAllHeldKeys() {
    for (const code of heldKeys) sendKey(code, true);
    heldKeys.clear();
  }
  // Doom 1.2 and its contemporaries predate WASD: they default to the arrow
  // cluster, with Ctrl/Alt/Shift for fire/strafe/run. This translates the
  // modern habit at the browser edge, so the guest still receives genuine
  // arrow-key scancodes -- nothing in the emulated keyboard changes, and a
  // program that reads the arrows cannot tell the difference.
  // A and D strafe rather than turn, which is what the modern habit expects:
  // Doom's own strafe modifier is Alt (key_strafe), so they send Alt plus the
  // arrow and the guest sees exactly the combination a player would hold.
  const kWasdToArrows = {
    KeyW: ["ArrowUp"], KeyS: ["ArrowDown"],
    KeyA: ["AltLeft", "ArrowLeft"], KeyD: ["AltLeft", "ArrowRight"],
  };
  const wasdCheckbox = document.getElementById("wasdArrows");
  function mapKey(code) {
    return wasdCheckbox.checked ? (kWasdToArrows[code] || [code]) : [code];
  }
  // Alt is shared by both strafe keys, so it is released only once neither is
  // still down -- releasing it with the other held would turn a strafe into a
  // turn mid-move.
  let strafeHeld = 0;
  const screenEl = document.getElementById("screen");
  screenEl.addEventListener("keydown", (e) => {
    const codes = mapKey(e.code);
    if (codes.length > 1 && !e.repeat) strafeHeld++;
    for (const code of codes) {
      if (heldKeys.has(code)) continue;
      heldKeys.add(code); sendKey(code, false);
    }
    e.preventDefault();
  });
  screenEl.addEventListener("keyup", (e) => {
    const codes = mapKey(e.code);
    if (codes.length > 1 && strafeHeld > 0) strafeHeld--;
    // Innermost key first, and keep Alt down while the other strafe key is.
    for (let i = codes.length - 1; i >= 0; i--) {
      const code = codes[i];
      if (code === "AltLeft" && strafeHeld > 0) continue;
      heldKeys.delete(code); sendKey(code, true);
    }
    e.preventDefault();
  });
  // heldKeys tracks the *mapped* code, so toggling mid-hold would otherwise
  // leave the guest holding a key whose break code never arrives.
  wasdCheckbox.addEventListener("change", () => { strafeHeld = 0; releaseAllHeldKeys(); });
  screenEl.addEventListener("click", () => {
    screenEl.focus();
    if (mouseCaptureCheckbox.checked && document.pointerLockElement !== screenEl) screenEl.requestPointerLock();
  });
  window.addEventListener("blur", releaseAllHeldKeys);
  document.addEventListener("visibilitychange", () => { if (document.hidden) releaseAllHeldKeys(); });

  // ---- PS/2 mouse (8042 AUX port) -----------------------------------
  // Off by default, opt-in like sound above -- Pointer Lock is itself a
  // browser permission gate, and capturing the pointer without the user
  // asking for it would trap their cursor on a page they didn't expect to.
  const mouseCaptureCheckbox = document.getElementById("mouseCaptureEnabled");
  mouseCaptureCheckbox.checked = false;
  let mouseButtons = 0;
  document.addEventListener("pointerlockchange", () => {
    if (document.pointerLockElement !== screenEl) {
      mouseButtons = 0;  // released -- all buttons up
      // The browser is required to exit Pointer Lock on Escape and is
      // allowed to consume that Escape keypress entirely -- it can fire
      // this event without ever dispatching a keyup (or even a keydown)
      // for it to the page. Without this, whatever movement key was held
      // when Escape released the lock would stay "pressed" from the
      // guest's side forever (see releaseAllHeldKeys()'s own comment).
      releaseAllHeldKeys();
    }
  });
  // DOM's MouseEvent.button numbering (0=left, 1=middle, 2=right) to the
  // PS/2 AUX packet's own bit order (bit0=left, bit1=right, bit2=middle,
  // see i8042.h's kMouseLeft/kMouseRight/kMouseMiddle) -- these don't match.
  function mouseButtonBit(button) {
    if (button === 0) return 0x01;
    if (button === 2) return 0x02;
    if (button === 1) return 0x04;
    return 0;
  }
  screenEl.addEventListener("mousedown", (e) => {
    if (!machine || document.pointerLockElement !== screenEl) return;
    mouseButtons |= mouseButtonBit(e.button);
    machine.injectMouseEvent(0, 0, mouseButtons);
    e.preventDefault();
  });
  screenEl.addEventListener("mouseup", (e) => {
    if (!machine || document.pointerLockElement !== screenEl) return;
    mouseButtons &= ~mouseButtonBit(e.button);
    machine.injectMouseEvent(0, 0, mouseButtons);
    e.preventDefault();
  });
  screenEl.addEventListener("contextmenu", (e) => {
    if (document.pointerLockElement === screenEl) e.preventDefault();
  });
  document.addEventListener("mousemove", (e) => {
    if (!machine || document.pointerLockElement !== screenEl) return;
    if (e.movementX === 0 && e.movementY === 0) return;
    // dy follows the mouse's own axis convention (+Y away from the user) --
    // the opposite of the browser's movementY (+Y is down the screen).
    machine.injectMouseEvent(e.movementX, -e.movementY, mouseButtons);
  });

  // "Click to type" banner and fullscreen mechanism: both purely web-UI
  // conveniences (a real AT keyboard/monitor has no such state), not
  // something CLAUDE.md's realism rules govern -- see shared/focus-hint.js
  // and shared/fullscreen.js. poweredOn is declared further down; passing
  // it as a predicate (not a captured value) lets these read its live
  // value from event handlers that all run after the whole script has
  // executed and poweredOn actually exists.
  const isRunning = () => poweredOn;
  const updateFocusHint = initFocusHint(screenEl, isRunning);
  // escBtn/sendEscape omitted: #escBtn already rides the same [data-key]
  // scancode-injection handling as the F-key row below (see its own
  // comment), so fullscreen.js only needs to show/hide it via CSS.
  initFullscreen({
    bezelEl: document.getElementById("bezel"),
    screenEl,
    fullscreenBtn: document.getElementById("fullscreenBtn"),
    fsEscHint: document.getElementById("fsEscHint"),
    fsEscHintOkBtn: document.getElementById("fsEscHintOk"),
    isRunning,
  });

  // "Barebones FreeDOS" notice -- shown over the screen on power-on to
  // explain why C: has no games/apps (the Base package set, PC486_REVIEW.md
  // §19.5) and that JEMMEX is a real V86 memory manager, not a stub. Purely
  // a web-UI convenience like the focus-hint banner above, dismissed by the
  // same signal that banner reacts to: the screen gaining focus. That
  // covers both a real click (the notice is pointer-events:none, so the
  // click reaches the canvas and focuses it in one gesture) and Tab-key
  // navigation, which is every reachable way to start typing here -- the
  // power switch itself is a separate focusable control, so toggling power
  // off and back on always moves focus there first, never leaves it
  // sitting on the screen underneath. Dismissing it once is remembered
  // (localStorage, same "retro8080." site-wide key namespace other machines
  // use) so a returning visitor doesn't see it again every power-on --
  // only a fresh visitor, or one who's cleared site data, gets it back.
  const BOOT_NOTICE_KEY = "retro8080.pc486BootNoticeDismissed";
  const bootNoticeEl = document.getElementById("bootNotice");
  function hideBootNotice() { bootNoticeEl.classList.remove("visible"); }
  // Only the screen actually gaining focus counts as "seen it" and is
  // remembered -- powerOff() also hides it (nothing to type into once
  // powered off) but that's not the visitor dismissing anything, so it
  // must not mark this permanently seen.
  function dismissBootNotice() {
    hideBootNotice();
    try { localStorage.setItem(BOOT_NOTICE_KEY, "1"); } catch {}
  }
  document.addEventListener("focusin", () => {
    if (screenEl.contains(document.activeElement)) dismissBootNotice();
  });

  // ---- floppy drive (this machine's single 3.5" bay) ---------------------
  // A real floppy is a mechanical slot: you can insert or eject one
  // whether the machine is powered on or off (pendingFloppy, populated
  // here, is what a power-on remounts -- see the power section below).
  const floppyBay = document.querySelector('.at-bay[data-drive="0"]');
  function setBayLoaded(bay, name) {
    bay.classList.add("loaded");
    const label = bay.querySelector('[data-role="label"]');
    label.textContent = name;
    label.classList.remove("empty");
    bay.querySelector('[data-role="eject"]').disabled = false;
  }
  function setBayEmpty(bay, emptyLabel) {
    bay.classList.remove("loaded");
    const label = bay.querySelector('[data-role="label"]');
    label.textContent = emptyLabel;
    label.classList.add("empty");
    bay.querySelector('[data-role="eject"]').disabled = true;
  }
  {
    const fileInput = floppyBay.querySelector('[data-role="file"]');
    const ejectBtn = floppyBay.querySelector('[data-role="eject"]');
    const label = floppyBay.querySelector('[data-role="label"]');
    fileInput.addEventListener("change", async () => {
      const f = fileInput.files[0];
      fileInput.value = "";
      if (!f) return;
      const bytes = new Uint8Array(await f.arrayBuffer());
      pendingFloppy = { name: f.name, bytes };
      if (machine) machine.mountFloppy(bytes);
      setBayLoaded(floppyBay, f.name);
    });
    ejectBtn.addEventListener("click", () => {
      // A real swappable drive: if the session actually wrote to this
      // diskette, hand the modified image back before ejecting it --
      // otherwise those writes only ever existed in this browser tab's
      // memory.
      if (machine && machine.floppyDirty()) {
        const img = machine.floppyImage();
        const blob = new Blob([img], { type: "application/octet-stream" });
        const a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = (label.textContent || "disk").replace(/[^\w.-]+/g, "_") + ".img";
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(() => URL.revokeObjectURL(a.href), 4000);
      }
      if (machine) machine.unmountFloppy();
      pendingFloppy = null;
      setBayEmpty(floppyBay, "empty (1.44MB, 3.5″)");
    });
  }

  // ---- CD-ROM drive (atapi_cdrom) -- removable, like the floppy ---------
  // No dirty-image/download path -- a real CD-ROM is read-only media, so
  // there's nothing to hand back on eject the way the floppy flow does.
  //
  // Empty by default, unlike the HDD: C: already ships with FreeDOS
  // installed (see the factory HDD image), so nothing about booting or
  // running needs a disc in this drive. Fetching FreeDOS's own ~400MB
  // install/live CD on every page load regardless was a real, reported
  // cost with no runtime benefit -- "Insert FreeDOS CD..." in the Freeware
  // Disks & Drivers panel fetches that same shipped .iso lazily, only when
  // someone actually wants it in the drive (see PC486_REVIEW.md).
  // ---- freeware disks & drivers ----------------------------------------
  // Fetches the CuteMouse floppy this machine builds (web/disks/ctmouse.img,
  // see disks/build-ctmouse-floppy.sh) and puts it in drive A:, taking the
  // same path a file the user picked would -- so ejecting, writing and the
  // pending-image handling all behave identically. The button stays live: a
  // real drive takes a diskette whenever you hand it one, including over a
  // disk already in the bay.
  {
    const btn = document.getElementById("ctmouseBtn");
    const status = document.getElementById("ctmouseStatus");
    btn.addEventListener("click", async () => {
      status.textContent = "Fetching\u2026";
      try {
        const res = await fetch("disks/ctmouse.img");
        if (!res.ok) throw new Error("HTTP " + res.status);
        const bytes = new Uint8Array(await res.arrayBuffer());
        pendingFloppy = { name: "ctmouse.img", bytes };
        if (machine) machine.mountFloppy(bytes);
        setBayLoaded(floppyBay, "ctmouse.img");
        status.innerHTML =
          "In drive A:. At the prompt: <code>A:</code> then <code>CTMOUSE /P</code> " +
          "-- it stays resident in memory, so you can eject the disk afterwards, but " +
          "it is gone at the next reboot. <code>MOUSETST</code> checks it. " +
          "To load it every boot: <code>COPY CTMOUSE.EXE C:\\</code> and add " +
          "<code>C:\\CTMOUSE /P</code> to <code>AUTOEXEC.BAT</code>.";
      } catch (err) {
        console.error("could not load the CuteMouse driver disk:", err);
        status.textContent = "Could not fetch the driver disk -- see the console.";
      }
    });
  }

  const cdromBay = document.querySelector('.at-bay[data-drive="cdrom"]');
  {
    const fileInput = cdromBay.querySelector('[data-role="file"]');
    const ejectBtn = cdromBay.querySelector('[data-role="eject"]');
    const loadFreedosBtn = document.getElementById("freedosCdBtn");
    const freedosStatus = document.getElementById("freedosCdStatus");
    fileInput.addEventListener("change", async () => {
      const f = fileInput.files[0];
      fileInput.value = "";
      if (!f) return;
      const bytes = new Uint8Array(await f.arrayBuffer());
      pendingCdrom = { name: f.name, bytes };
      if (machine) machine.mountCdrom(bytes);
      setBayLoaded(cdromBay, f.name);
    });
    loadFreedosBtn.addEventListener("click", async () => {
      loadFreedosBtn.disabled = true;
      const originalText = loadFreedosBtn.textContent;
      loadFreedosBtn.textContent = "Loading\u2026";
      freedosStatus.textContent = "Fetching\u2026 (~400MB)";
      try {
        const res = await fetch("disks/freedos-cd.iso");
        if (!res.ok) throw new Error("HTTP " + res.status);
        const bytes = new Uint8Array(await res.arrayBuffer());
        pendingCdrom = { name: "FreeDOS install/live CD", bytes };
        if (machine) machine.mountCdrom(bytes);
        setBayLoaded(cdromBay, pendingCdrom.name);
        freedosStatus.innerHTML =
          "In drive D:. At the prompt: <code>D:</code> then <code>DIR</code> to browse -- " +
          "FreeDOS's official install/live CD (packages, SETUP, extras). " +
          "C: already boots FreeDOS without it; use this to install more packages or " +
          "reinstall. Eject from the CD-ROM bay when done.";
      } catch (err) {
        console.error("could not load the FreeDOS CD:", err);
        freedosStatus.textContent = "Could not fetch the FreeDOS CD -- see the console.";
      } finally {
        loadFreedosBtn.textContent = originalText;
        loadFreedosBtn.disabled = false;
      }
    });
    ejectBtn.addEventListener("click", () => {
      if (machine) machine.ejectCdrom();
      pendingCdrom = null;
      setBayEmpty(cdromBay, "empty (CD-ROM, 5.25″ / 2x)");
    });
  }

  // ---- PC speaker -- muted by default, every page load -----------------
  // Never restored from a saved preference: browsers block audio until a
  // fresh user gesture anyway, and the point of "off by default" is that
  // it stays that way until the visitor explicitly opts back in, not just
  // on first visit.
  //
  // Output is a single, persistent AudioWorkletNode fed by a ring buffer,
  // not a chain of one-shot AudioBufferSourceNodes scheduled back-to-back
  // per animation frame -- that shape is fundamentally fragile: even with
  // perfectly gapless scheduling math, it depends on every rAF frame
  // handing the audio thread its own freshly start()ed node exactly on
  // time, and any main-thread hiccup (a GC pause, a big array copy) leaves
  // the currently-scheduled node's audio running out with nothing queued
  // behind it -- dead silence until the next node starts, which then jumps
  // straight to a nonzero level. That gap-then-jump is an audible click,
  // and enough of them in a row is exactly the "scratchy" artifact
  // reported live (see IBM_PCAT_REVIEW.md). A worklet's process() callback
  // runs continuously on the real-time audio thread regardless of what the
  // main thread is doing; feeding it through a ring buffer means a brief
  // stall just holds the last sample level (silent, no discontinuity)
  // until the main thread catches up and posts more data, rather than
  // clicking. It also needs no nextPlayTime/resync bookkeeping, since
  // there's no scheduling clock to keep in sync.
  const speakerCheckbox = document.getElementById("speakerEnabled");
  speakerCheckbox.checked = false;
  let audioCtx = null, speakerNode = null, lastLevel = false;
  let sbNode = null, lastSbLeft = 0, lastSbRight = 0, lastFmLeft = 0, lastFmRight = 0;
  let audioStats = null;

  // The worklet module's source, registered from a Blob URL rather than a
  // separate fetched file -- keeps the whole speaker path in this one
  // script with nothing extra for the Makefile to stage.
  const kSpeakerWorkletSrc = `
    class PcSpeakerProcessor extends AudioWorkletProcessor {
      constructor() {
        super();
        // ~350ms at 48kHz -- generous headroom to absorb a single main-thread
        // jank spike (a GC pause, the periodic HDD autosave's array copy)
        // without an audible click. This is a transient-absorption ceiling,
        // not the steady-state depth: see targetAvailable below.
        this.ring = new Float32Array(16384);
        this.writeIdx = 0;
        this.readIdx = 0;
        this.available = 0;
        this.lastSample = 0;
        // A jank spike that fills the ring would otherwise latch there
        // forever: under steady 1:1 real-time playback nothing ever drains
        // it back down except an underrun, so one stall used to leave a
        // permanent, ever-present latency behind (measured as an audible
        // ~0.3-0.5s input-to-sound lag once a game had been running a
        // while -- PC486_REVIEW.md). Instead, resync back down to a small
        // target depth: this keeps enough slack to absorb the next spike
        // without a click, but the backlog a spike leaves behind is trimmed
        // away quickly instead of persisting for the rest of the session.
        //
        // This trim MUST run inside process(), not onmessage(): process()
        // is paced by the real audio clock (one call per ~128-sample
        // render quantum, ~2.7ms @48kHz), which is the only thing here with
        // a guaranteed real-time rate. onmessage() has no such guarantee --
        // pump()'s main-thread loop can re-enter far faster than its
        // nominal ~12ms cadence whenever the emulated CPU is idle (each
        // call still posts at least one sample, Math.max(1, ...) in
        // pumpAudio()), and trimming there fires once per *message*
        // instead of once per real audio quantum. With enough of those
        // excess messages, readIdx's forced jumps outrun what process()
        // has actually played, walking it forward through ring positions
        // process() hasn't reached yet -- including stale audio from
        // *before* the current silence that was never overwritten, since a
        // jump only touches readIdx, not the ring contents in between. The
        // audible result was the previous sound looping on its own, long
        // after the emulator had gone silent (see PC486_REVIEW.md's audio
        // investigation). Trimming once per process() call instead bounds
        // the jump to what real playback has actually consumed.
        this.targetAvailable = Math.round(sampleRate * 0.05);  // 50ms
        // One-pole DC blocker. pumpAudio encodes the speaker as ±0.25, which
        // is right for a square-wave beep, but a cone parked on either rail
        // (the FreeDOS prompt: level stuck low, zero edges) is a constant
        // DC bias into Web Audio -- browsers are DC-coupled, unlike a real
        // speaker amp's AC coupling, and that held −0.25 shows up as a
        // quiet periodic thump. This settles a held level to digital
        // silence while letting actual beeps through.
        this.prevIn = 0;
        this.prevOut = 0;
        this.dcR = 0.995;
        this.port.onmessage = (e) => {
          const chunk = e.data;
          for (let i = 0; i < chunk.length; i++) {
            this.ring[this.writeIdx] = chunk[i];
            this.writeIdx = (this.writeIdx + 1) % this.ring.length;
            if (this.available < this.ring.length) {
              this.available++;
            } else {
              // Ring overflowed (main thread fed it faster than real time,
              // e.g. right after a stall's worth of catch-up cycles) --
              // drop the oldest sample rather than the newest, same as an
              // unread hardware FIFO would.
              this.readIdx = (this.readIdx + 1) % this.ring.length;
            }
          }
        };
      }
      process(_inputs, outputs) {
        if (this.available > this.targetAvailable) {
          const drop = this.available - this.targetAvailable;
          this.readIdx = (this.readIdx + drop) % this.ring.length;
          this.available = this.targetAvailable;
        }
        const out = outputs[0][0];
        for (let i = 0; i < out.length; i++) {
          if (this.available > 0) {
            this.lastSample = this.ring[this.readIdx];
            this.readIdx = (this.readIdx + 1) % this.ring.length;
            this.available--;
          }
          // Underrun: hold the last real sample instead of snapping to 0 --
          // a real speaker cone doesn't teleport to rest either, and
          // holding avoids adding its own click on top of the stall.
          const x = this.lastSample;
          const y = x - this.prevIn + this.dcR * this.prevOut;
          this.prevIn = x;
          this.prevOut = y;
          out[i] = y;
        }
        return true;
      }
    }
    registerProcessor("pc-speaker-processor", PcSpeakerProcessor);
  `;

  // Same ring-buffer-behind-a-worklet shape as the PC speaker above, just
  // stereo: two channels fed together so left/right stay sample-locked.
  //
  // Depth is a transient-absorption ceiling, not the steady-state latency:
  // resync-to-target (below, same mechanism as the PC speaker worklet)
  // trims the backlog a jank spike leaves behind back down to targetAvailable
  // within one message, instead of letting it latch at whatever depth the
  // spike drove the ring to for the rest of the session. Before that existed,
  // this number WAS close to the actual steady-state latency once a jank
  // event had occurred (a near-certainty within a few seconds of real
  // browser scheduling jitter) -- a browser gunshot-to-bang report of
  // "about a second" matched the old 32768 depth (683 ms @48kHz) almost
  // exactly. Measured against the real stall this headroom exists for --
  // persistHddIfDirty()'s full-disk-image copy off the wasm heap (the
  // "periodic HDD autosave's array copy" the PC speaker's own comment above
  // names) -- 180-350 ms in a real headless Chromium run against this
  // machine's 504 MB image, EVERY 5 seconds whenever anything on C: was
  // written. persistHddIfDirty() now patches only the sectors a session
  // actually wrote (wd1003.h's dirty_ranges()), so that specific 180-350ms/5s
  // hit is gone in the common case -- typical DOS writes are a handful of
  // 4KB pages, not the whole disk -- but this depth stays as headroom for
  // ordinary GC-pause-scale jank and the rare full-copy fallback (no
  // same-size mirror yet to patch onto): a stall bigger than it underruns
  // into a brief held-last-sample tone rather than the click the ring-buffer
  // architecture itself was built to avoid, and now drains back to target
  // afterward instead of leaving that depth as the new permanent floor.
  const kSbWorkletSrc = `
    class Sb16Processor extends AudioWorkletProcessor {
      constructor() {
        super();
        this.left = new Float32Array(8192);
        this.right = new Float32Array(8192);
        this.writeIdx = 0;
        this.readIdx = 0;
        this.available = 0;
        this.lastLeft = 0;
        this.lastRight = 0;
        // Resync-to-target trim MUST run inside process(), not onmessage() --
        // see the identical reasoning in kSpeakerWorkletSrc above. onmessage()
        // has no real-time guarantee (pump() can re-enter far faster than its
        // nominal ~12ms cadence when the emulated CPU is idle), and trimming
        // there let readIdx's forced jumps outrun real playback, walking it
        // into stale ring positions from a *previous* sound that process()
        // hadn't reached yet and hadn't been overwritten -- heard as that
        // sound looping on its own long after the emulator went silent.
        this.targetAvailable = Math.round(sampleRate * 0.05);  // 50ms, see comment above
        // Health counters for the Performance panel. Underruns are the
        // interesting one: the ring running dry is what "the sound got off"
        // actually is, and nothing on the main thread can observe it.
        this.starved = 0;
        this.trimmed = 0;
        this.statFrames = 0;
        this.port.onmessage = (e) => {
          const { left, right } = e.data;
          for (let i = 0; i < left.length; i++) {
            this.left[this.writeIdx] = left[i];
            this.right[this.writeIdx] = right[i];
            this.writeIdx = (this.writeIdx + 1) % this.left.length;
            if (this.available < this.left.length) {
              this.available++;
            } else {
              this.readIdx = (this.readIdx + 1) % this.left.length;
            }
          }
        };
      }
      process(_inputs, outputs) {
        if (this.available > this.targetAvailable) {
          const drop = this.available - this.targetAvailable;
          this.readIdx = (this.readIdx + drop) % this.left.length;
          this.available = this.targetAvailable;
          this.trimmed += drop;
        }
        const outL = outputs[0][0], outR = outputs[0][1];
        for (let i = 0; i < outL.length; i++) {
          if (this.available > 0) {
            this.lastLeft = this.left[this.readIdx];
            this.lastRight = this.right[this.readIdx];
            this.readIdx = (this.readIdx + 1) % this.left.length;
            this.available--;
          } else {
            this.starved++;
          }
          outL[i] = this.lastLeft;
          outR[i] = this.lastRight;
        }
        this.statFrames += outL.length;
        if (this.statFrames >= sampleRate / 2) {   // twice a second
          this.port.postMessage({
            stats: { depth: this.available, starved: this.starved,
                     trimmed: this.trimmed, secs: this.statFrames / sampleRate },
          });
          this.statFrames = 0;
          this.starved = 0;
          this.trimmed = 0;
        }
        return true;
      }
    }
    registerProcessor("sb16-processor", Sb16Processor);
  `;

  async function ensureAudioStarted() {
    // A context can exist but still be "suspended" -- Safari in particular
    // doesn't always auto-resume on the constructing gesture the way
    // Chrome does, and any browser may suspend an idle/backgrounded
    // context on its own later. Either way this is silent: no error, just
    // no sound, which is why unchecking/rechecking the box (re-entering
    // this function) has to be able to pull an already-created context
    // back out of that state, not just skip past it.
    if (audioCtx) {
      if (audioCtx.state === "suspended") await audioCtx.resume();
      return;
    }
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === "suspended") await audioCtx.resume();
    // AudioWorklet doesn't exist at all outside a secure context (https://,
    // or http://localhost specifically -- not a LAN IP/hostname, even on
    // your own network). The deployed site is always https:// so real
    // visitors never hit this; a LAN-served local preview can. Degrade to
    // silent rather than an uncaught TypeError -- speakerNode/sbNode stay
    // null, and pumpAudio()/pumpSbAudio() already no-op when either is null.
    if (!audioCtx.audioWorklet) {
      console.warn("pc486: AudioWorklet unavailable (not a secure context) -- sound disabled");
      return;
    }
    const blobUrl = URL.createObjectURL(new Blob([kSpeakerWorkletSrc], { type: "application/javascript" }));
    try {
      await audioCtx.audioWorklet.addModule(blobUrl);
    } finally {
      URL.revokeObjectURL(blobUrl);
    }
    speakerNode = new AudioWorkletNode(audioCtx, "pc-speaker-processor", { numberOfOutputs: 1, outputChannelCount: [1] });
    speakerNode.connect(audioCtx.destination);

    const sbBlobUrl = URL.createObjectURL(new Blob([kSbWorkletSrc], { type: "application/javascript" }));
    try {
      await audioCtx.audioWorklet.addModule(sbBlobUrl);
    } finally {
      URL.revokeObjectURL(sbBlobUrl);
    }
    sbNode = new AudioWorkletNode(audioCtx, "sb16-processor", { numberOfOutputs: 1, outputChannelCount: [2] });
    // The ring's own view of its health -- depth, and how often it ran dry.
    // Only the worklet thread can see this; the pump cannot.
    sbNode.port.onmessage = (e) => {
      if (e.data && e.data.stats) audioStats = e.data.stats;
    };
    sbNode.connect(audioCtx.destination);
  }
  speakerCheckbox.addEventListener("change", () => {
    if (speakerCheckbox.checked) {
      ensureAudioStarted();
    } else if (audioCtx) {
      // Stopping the sample pump alone leaves the context "running", so
      // Chrome (and friends) keep the tab's speaker icon lit even though
      // nothing is audible. Suspend matches powerOff()'s own treatment and
      // clears that indicator; ensureAudioStarted() resumes on re-check.
      audioCtx.suspend().catch(() => {});
    }
  });

  // Bezel-corner icons mirror the three checkboxes under the monitor --
  // same state, same change handlers -- so sound / mouse / WASD stay
  // reachable once fullscreen covers the page chrome below the bezel.
  function bindBezelToggle(btn, checkbox) {
    const sync = () => {
      btn.setAttribute("aria-pressed", checkbox.checked ? "true" : "false");
    };
    btn.addEventListener("click", () => {
      checkbox.checked = !checkbox.checked;
      checkbox.dispatchEvent(new Event("change"));
      sync();
    });
    checkbox.addEventListener("change", sync);
    sync();
  }
  bindBezelToggle(document.getElementById("speakerBtn"), speakerCheckbox);
  bindBezelToggle(document.getElementById("mouseCaptureBtn"), mouseCaptureCheckbox);
  bindBezelToggle(document.getElementById("wasdArrowsBtn"), wasdCheckbox);

  // Converts this frame's real (cpu_cycle, level) edge trace --
  // PcSpeaker::drain_edges() via speakerEdges() -- into a sample array and
  // posts it to the worklet's ring buffer. No scheduling clock to maintain
  // here: the worklet plays whatever it's been sent, in order, at its own
  // pace, entirely decoupled from this function's own timing.
  function pumpAudio(frameStartCycle, cyclesThisFrame, dtSeconds) {
    const edges = machine.speakerEdges();  // always drain -- even if muted, so the log can't grow unbounded
    if (!audioCtx || !speakerNode || !speakerCheckbox.checked || cyclesThisFrame <= 0) return;
    const sampleRate = audioCtx.sampleRate;
    // Real elapsed wall-clock time for this frame, not cyclesThisFrame/66MHz --
    // those two only match when TEST_CPU_MULTIPLIER is 1. Deriving duration
    // from the cycle count instead would generate `multiplier`x too many
    // samples for one real frame under a fast-test multiplier. Using real
    // dtSeconds keeps this correct (and harmless -- just pitch-shifted,
    // which nothing here asserts on) at any multiplier.
    const sampleCount = Math.max(1, Math.round(dtSeconds * sampleRate));
    const data = new Float32Array(sampleCount);

    let level = lastLevel, sampleIdx = 0;
    const cycles = edges.cycles, levels = edges.levels;
    // Effective this-frame rate: real 8 MHz normally, `multiplier`x that
    // under the fast-test multiplier -- see sampleCount above.
    const cyclesPerRealSecond = cyclesThisFrame / dtSeconds;
    for (let i = 0; i < cycles.length; i++) {
      let edgeSample = Math.round(((cycles[i] - frameStartCycle) / cyclesPerRealSecond) * sampleRate);
      if (edgeSample < 0) edgeSample = 0;
      if (edgeSample > sampleCount) edgeSample = sampleCount;
      // ±0.25 bipolar square wave; a held rail (idle prompt) is DC that the
      // worklet's DC blocker settles to silence -- see kSpeakerWorkletSrc.
      const v = level ? 0.25 : -0.25;
      for (; sampleIdx < edgeSample; sampleIdx++) data[sampleIdx] = v;
      level = levels[i] !== 0;
    }
    const vTail = level ? 0.25 : -0.25;
    for (; sampleIdx < sampleCount; sampleIdx++) data[sampleIdx] = vTail;
    lastLevel = level;

    speakerNode.port.postMessage(data, [data.buffer]);
  }

  // Same real-cycle-to-wall-time mapping as pumpAudio() above, but for the
  // SB16's discrete digitized samples instead of the speaker's edge trace:
  // each incoming sample is held (sample-and-hold, the same thing a real
  // DAC does between updates) from its own mapped position up to the next
  // sample's, rather than interpolated.
  // The CT1745's attenuators are pure attenuation, and both the Master and
  // the per-source pair power on at 24 (-14 dB), leaving the chain ~28 dB
  // down at defaults -- a real card makes that back up in the analog output
  // amplifier after the mixer. Normalizing by the power-on product models
  // that fixed amplifier gain: default mixer settings play at full scale,
  // and a program that moves a slider still attenuates relative to it.
  const kMixerUnityGain = 0.2 * 0.2;  // five_bit_gain(24)^2, see soundblaster.cpp

  // Sample-and-holds one cpu_cycle-stamped stream into `left`/`right`,
  // scaled by that source's own CT1745 attenuator, and ADDS it -- the card
  // sums FM and digitized audio in the analog domain, so the two streams mix
  // here rather than in the core. Returns the stream's last value so the next
  // frame resumes the hold where this one left off.
  function mixStampedStream(s, left, right, sampleCount, frameStartCycle,
                            cyclesPerRealSecond, sampleRate, startL, startR, gainL, gainR) {
    let idx = 0, curL = startL, curR = startR;
    const gL = gainL / kMixerUnityGain, gR = gainR / kMixerUnityGain;
    const cycles = s.cycles, ls = s.left, rs = s.right;
    for (let i = 0; i < cycles.length; i++) {
      let pos = Math.round(((cycles[i] - frameStartCycle) / cyclesPerRealSecond) * sampleRate);
      if (pos < 0) pos = 0;
      if (pos > sampleCount) pos = sampleCount;
      for (; idx < pos; idx++) { left[idx] += curL * gL; right[idx] += curR * gR; }
      curL = ls[i] / 32768;
      curR = rs[i] / 32768;
    }
    for (; idx < sampleCount; idx++) { left[idx] += curL * gL; right[idx] += curR * gR; }
    return [curL, curR];
  }

  function pumpSbAudio(frameStartCycle, cyclesThisFrame, dtSeconds) {
    // Always drain both -- even if muted, so neither log can grow unbounded.
    const s = machine.sbDrainSamples();
    const fm = machine.fmDrainSamples();
    if (!audioCtx || !sbNode || !speakerCheckbox.checked || cyclesThisFrame <= 0) return;
    const sampleRate = audioCtx.sampleRate;
    const sampleCount = Math.max(1, Math.round(dtSeconds * sampleRate));
    const left = new Float32Array(sampleCount);
    const right = new Float32Array(sampleCount);
    const cyclesPerRealSecond = cyclesThisFrame / dtSeconds;

    [lastSbLeft, lastSbRight] = mixStampedStream(s, left, right, sampleCount, frameStartCycle,
      cyclesPerRealSecond, sampleRate, lastSbLeft, lastSbRight,
      machine.sbGainLeft(), machine.sbGainRight());
    [lastFmLeft, lastFmRight] = mixStampedStream(fm, left, right, sampleCount, frameStartCycle,
      cyclesPerRealSecond, sampleRate, lastFmLeft, lastFmRight,
      machine.fmGainLeft(), machine.fmGainRight());

    if (dbg.on) { dbg.posted += sampleCount; dbg.dsp += s.cycles.length; dbg.fm += fm.cycles.length; }
    sbNode.port.postMessage({ left, right }, [left.buffer, right.buffer]);
  }

  // Names for the opcode forms a DOS game actually spends its time in --
  // enough to read the histogram at a glance without an opcode map open.
  // Anything unlisted shows as its raw byte.
  const kOpNames = {
    "88": "mov rm8,r8", "89": "mov rm,r", "8A": "mov r8,rm8", "8B": "mov r,rm",
    "8D": "lea", "8E": "mov sreg,rm", "8F": "pop rm",
    "00": "add rm8,r8", "01": "add rm,r", "02": "add r8,rm8", "03": "add r,rm",
    "28": "sub rm8,r8", "29": "sub rm,r", "2B": "sub r,rm",
    "30": "xor rm8,r8", "31": "xor rm,r", "33": "xor r,rm",
    "20": "and rm8,r8", "21": "and rm,r", "23": "and r,rm",
    "38": "cmp rm8,r8", "39": "cmp rm,r", "3A": "cmp r8,rm8", "3B": "cmp r,rm",
    "3C": "cmp al,imm8", "3D": "cmp eax,imm",
    "80": "grp1 rm8,imm8", "81": "grp1 rm,imm", "83": "grp1 rm,imm8",
    "84": "test rm8,r8", "85": "test rm,r",
    "C0": "shift rm8,imm", "C1": "shift rm,imm", "D0": "shift rm8,1",
    "D1": "shift rm,1", "D3": "shift rm,cl",
    "F6": "grp3 rm8", "F7": "grp3 rm", "FE": "inc/dec rm8", "FF": "grp5 rm",
    "50": "push r", "58": "pop r", "68": "push imm", "6A": "push imm8",
    "E8": "call rel", "E9": "jmp rel", "EB": "jmp short", "C3": "ret",
    "74": "je", "75": "jne", "72": "jb", "73": "jae", "7C": "jl", "7D": "jge",
    "7E": "jle", "7F": "jg", "76": "jbe", "77": "ja",
    "A4": "movsb", "A5": "movsd", "AA": "stosb", "AB": "stosd",
    "AC": "lodsb", "AD": "lodsd", "B0": "mov al,imm", "B8": "mov eax,imm",
    "C6": "mov rm8,imm", "C7": "mov rm,imm",
    "26": "pfx es", "2E": "pfx cs", "36": "pfx ss", "3E": "pfx ds",
    "64": "pfx fs", "65": "pfx gs", "66": "pfx opsize", "67": "pfx addrsize",
    "F2": "pfx repnz", "F3": "pfx rep",
    "0fA4": "shld rm,r,imm", "0fA5": "shld rm,r,cl",
    "0fAC": "shrd rm,r,imm", "0fAD": "shrd rm,r,cl",
    "0fAF": "imul r,rm", "0fB6": "movzx r,rm8", "0fB7": "movzx r,rm16",
    "0fBE": "movsx r,rm8", "0fBF": "movsx r,rm16", "0f84": "je near",
    "0f85": "jne near", "0f45": "cmovne", "0f44": "cmove",
  };
  function opName(tok) {
    const key = tok.startsWith("0f") ? tok : tok.toUpperCase();
    return kOpNames[key] || kOpNames[tok] || tok;
  }

  // ?perf asks for the instrumented build: a separate binary carrying the
  // emulator's own counters (the panel's Tier 2). The shipped one has none
  // of them -- they sit on the hottest paths there are. Only one of the two
  // is ever fetched, and if the instrumented build is missing we fall back
  // so the panel still gets its host-side half instead of the page failing.
  const perfRequested = new URLSearchParams(location.search).has("perf");
  function loadScript(src) {
    return new Promise((resolve, reject) => {
      const el = document.createElement("script");
      el.src = src;
      el.onload = () => resolve();
      el.onerror = () => reject(new Error("could not load " + src));
      document.head.appendChild(el);
    });
  }
  async function loadEmulatorModule() {
    let src = "pc486.js";
    if (perfRequested) {
      // Ask whether the instrumented build was deployed BEFORE committing to
      // it, so exactly one module script is ever appended -- appending a
      // second after a failed one leaves the page with two half-initialized
      // emscripten modules and no machine at all.
      try {
        const head = await fetch("pc486-perf.js", { method: "HEAD" });
        if (head.ok) src = "pc486-perf.js";
        else console.warn("instrumented build not deployed -- host metrics only");
      } catch (err) {
        console.warn("instrumented build not reachable -- host metrics only:", err);
      }
    }
    await loadScript(src);
  }

  // Per-second accumulators behind the Performance panel. dbg.on is set only
  // while the panel is open, so a normal visit does no bookkeeping -- the
  // cost of measuring stays out of the thing being measured.
  const dbg = { on: false, emuMs: 0, renderMs: 0, frames: 0, dropped: 0, pumps: 0,
                posted: 0, dsp: 0, fm: 0 };

  // A Task Manager-style trace of the last minute: filled area for the main
  // thread's share of one core, line for the emulated clock against its real
  // 66 MHz. Both are percentages on the same axis, so one chart shows "is it
  // busy" and "is it keeping up" together -- which is the pair that matters,
  // since the clock only falls once the thread runs out of room.
  function drawPerfChart(c, canvas, history) {
    const w = canvas.width, h = canvas.height;
    c.clearRect(0, 0, w, h);
    c.fillStyle = "#0b0f0b";
    c.fillRect(0, 0, w, h);

    c.strokeStyle = "#1e2c1e";
    c.lineWidth = 1;
    for (let i = 1; i < 4; i++) {          // 25/50/75%
      const y = Math.round(h * i / 4) + 0.5;
      c.beginPath(); c.moveTo(0, y); c.lineTo(w, y); c.stroke();
    }
    for (let i = 1; i < 6; i++) {          // every 10s across a 60s window
      const x = Math.round(w * i / 6) + 0.5;
      c.beginPath(); c.moveTo(x, 0); c.lineTo(x, h); c.stroke();
    }

    const n = 60;                           // fixed window, so it scrolls
    const xAt = (i) => (i / (n - 1)) * w;
    const yAt = (pct) => h - Math.max(0, Math.min(120, pct)) / 120 * h;
    const first = n - history.length;

    if (history.length > 1) {
      c.beginPath();
      c.moveTo(xAt(first), h);
      history.forEach((p, i) => c.lineTo(xAt(first + i), yAt(p.cpu)));
      c.lineTo(xAt(first + history.length - 1), h);
      c.closePath();
      c.fillStyle = "rgba(64, 160, 255, 0.28)";
      c.fill();
      c.beginPath();
      history.forEach((p, i) => {
        const x = xAt(first + i), y = yAt(p.cpu);
        if (i === 0) c.moveTo(x, y); else c.lineTo(x, y);
      });
      c.strokeStyle = "#5ab0ff";
      c.lineWidth = 1.5;
      c.stroke();

      c.beginPath();
      history.forEach((p, i) => {
        const x = xAt(first + i), y = yAt(p.clock);
        if (i === 0) c.moveTo(x, y); else c.lineTo(x, y);
      });
      c.strokeStyle = "#6f6";
      c.lineWidth = 1.5;
      c.stroke();
    }

    // 100% is where the clock should sit and where the thread runs out.
    const full = Math.round(yAt(100)) + 0.5;
    c.strokeStyle = "#4a5a4a";
    c.setLineDash([3, 3]);
    c.beginPath(); c.moveTo(0, full); c.lineTo(w, full); c.stroke();
    c.setLineDash([]);

    c.font = "11px ui-monospace, Menlo, Consolas, monospace";
    c.fillStyle = "#5ab0ff";
    c.fillText("486 cpu", 6, 13);
    c.fillStyle = "#6f6";
    c.fillText("clock", 56, 13);
    c.fillStyle = "#6a7a6a";
    c.fillText("60s", w - 26, h - 5);
  }

  // The host's side of the same minute: the share of one core this page is
  // using, and the frame rate it is managing. Separate from the 486's chart
  // because these are the browser's numbers, not the machine's -- and
  // because a fullscreen stall shows here as draw cost and falling fps while
  // the machine's own chart barely moves.
  // Sticky fps chart scale -- see the comment at its use below.
  let fpsScale = 60, fpsShrinkFrames = 0;
  const kFpsShrinkFrames = 90;

  function drawHostChart(c, canvas, history) {
    const w = canvas.width, h = canvas.height;
    c.fillStyle = "#0b0f0b";
    c.fillRect(0, 0, w, h);

    c.strokeStyle = "#1e2c1e";
    c.lineWidth = 1;
    for (let i = 1; i < 4; i++) {
      const y = Math.round(h * i / 4) + 0.5;
      c.beginPath(); c.moveTo(0, y); c.lineTo(w, y); c.stroke();
    }
    for (let i = 1; i < 6; i++) {
      const x = Math.round(w * i / 6) + 0.5;
      c.beginPath(); c.moveTo(x, 0); c.lineTo(x, h); c.stroke();
    }

    const n = 60;
    const xAt = (i) => (i / (n - 1)) * w;
    const first = n - history.length;
    // fps has no natural percentage, so it gets its own scale off the
    // fastest rate actually seen -- a 60Hz display would otherwise sit at
    // half height forever and read as a problem. The scale grows at once but
    // shrinks only after the reading has stayed low for a while: recomputing
    // it per frame made it flip between buckets as a spike aged out of the
    // window, which redraws the whole graph at a new scale and reads as two
    // charts alternating.
    let wanted = 60;
    for (const p of history) if (p.fps > wanted) wanted = p.fps;
    wanted = Math.ceil(wanted / 30) * 30;
    if (wanted >= fpsScale) {
      fpsScale = wanted;
      fpsShrinkFrames = 0;
    } else if (++fpsShrinkFrames >= kFpsShrinkFrames) {
      fpsScale = wanted;
      fpsShrinkFrames = 0;
    }
    const fpsMax = fpsScale;

    if (history.length > 1) {
      c.beginPath();
      c.moveTo(xAt(first), h);
      history.forEach((p, i) => c.lineTo(xAt(first + i), h - Math.min(100, p.host) / 120 * h));
      c.lineTo(xAt(first + history.length - 1), h);
      c.closePath();
      c.fillStyle = "rgba(255, 176, 64, 0.26)";
      c.fill();
      c.beginPath();
      history.forEach((p, i) => {
        const x = xAt(first + i), y = h - Math.min(100, p.host) / 120 * h;
        if (i === 0) c.moveTo(x, y); else c.lineTo(x, y);
      });
      c.strokeStyle = "#ffb040";
      c.lineWidth = 1.5;
      c.stroke();

      c.beginPath();
      history.forEach((p, i) => {
        const x = xAt(first + i), y = h - Math.min(1, p.fps / fpsMax) * h * (100 / 120);
        if (i === 0) c.moveTo(x, y); else c.lineTo(x, y);
      });
      c.strokeStyle = "#8ad";
      c.lineWidth = 1.5;
      c.stroke();
    }

    const full = Math.round(h - 100 / 120 * h) + 0.5;
    c.strokeStyle = "#4a5a4a";
    c.setLineDash([3, 3]);
    c.beginPath(); c.moveTo(0, full); c.lineTo(w, full); c.stroke();
    c.setLineDash([]);

    c.font = "11px ui-monospace, Menlo, Consolas, monospace";
    c.fillStyle = "#ffb040";
    c.fillText("host core", 6, 13);
    c.fillStyle = "#8ad";
    c.fillText("fps (max " + fpsMax + ")", 72, 13);
    c.fillStyle = "#6a7a6a";
    c.fillText("60s", w - 26, h - 5);
  }

  // ---- performance panel -------------------------------------------
  // Shown only when the wasm module reports a debug build (web/Makefile's
  // DEBUG_PERF=1); a shipped machine has neither the counters nor the panel.
  // Everything lands in the page rather than the console deliberately:
  // reading it needs no DevTools, and DevTools attached plus a log line a
  // second spends the very main-thread budget being measured -- which is how
  // the first round of numbers came out lower than the machine really ran.
  function startPerfPanel() {
    const card = document.getElementById("perfCard");
    const out = document.getElementById("perfReadout");
    card.hidden = false;
    dbg.on = true;
    // Tier 2 is the emulator's own counters, and only the instrumented
    // binary has them. Saying so on the page matters: absent counters read
    // as genuine zeros otherwise, which is exactly how a measurement gets
    // misread.
    const tier2 = typeof machine.perfBuild === "function" && machine.perfBuild();
    document.getElementById("perfTier").textContent = tier2
      ? "Instrumented build (pc486-perf.wasm): host metrics + the emulator's own counters."
      : "Shipped build: host metrics only. Reload with ?perf after 'make perf-build' for " +
        "the emulator's internal counters.";
    const chart = document.getElementById("perfChart");
    const cctx = chart.getContext("2d");
    const chart2 = document.getElementById("perfChart2");
    const cctx2 = chart2.getContext("2d");
    const history = [];   // {cpu, clock} per second, newest last
    let c0 = machine.totalCycles(), h0 = machine.haltCycles();
    let i0 = machine.idleCycles(), t0 = performance.now();
    // The panel opens before the ROMs load and before power-on, so the first
    // sample spans a window the machine was not really running in. A
    // cumulative average that included it would sit permanently wrong -- and
    // wrong in a way that looks plausible, since the pump can never grant
    // more than 66 MHz of wall time and a reading above it is the giveaway.
    let first = true;
    const recent = [];
    // Discard whatever accumulated before the panel opened -- only the
    // instrumented build has counters to discard.
    if (tier2) machine.perfStats();
    setInterval(() => {
      if (!machine) return;
      const secs = (performance.now() - t0) / 1000;
      const cyc = Number(machine.totalCycles() - c0);
      const halt = machine.haltCycles() - h0;
      const idlePoll = machine.idleCycles() - i0;
      const stats = tier2 ? machine.perfStats() : "";
      c0 = machine.totalCycles();
      h0 = machine.haltCycles();
      i0 = machine.idleCycles();
      t0 = performance.now();
      if (secs <= 0 || cyc <= 0) return;   // powered off, or no time elapsed
      const mhz = cyc / secs / 1e6;
      if (first) { first = false; return; }
      recent.push(mhz);
      if (recent.length > 30) recent.shift();   // last 30s, so a dip decays out
      const avg = recent.reduce((a, b) => a + b, 0) / recent.length;
      // Windowed like the average: a session-wide minimum just reports the
      // boot or a level load forever, which says nothing about how the
      // machine is running now.
      const worst = Math.min(...recent);
      const per = {};
      for (const pair of stats.split(" ")) {
        const [k, v] = pair.split("=");
        per[k] = Number(v) / secs;
      }
      const m = (n) => (n / 1e6).toFixed(2) + "M/s";
      // Ranked opcode counts -- what the interpreter actually spends itself
      // on. Frequency, not time: timing each instruction would cost more
      // than running it.
      let hot = "";
      const parts = tier2 ? machine.perfHotOpcodes(8).split(" ") : [];
      const totalOps = Number((parts.pop() || "total=0").split("=")[1]) || 1;
      for (const pair of parts) {
        if (!pair) continue;
        const eq = pair.lastIndexOf("=");
        const tok = pair.slice(0, eq), n = Number(pair.slice(eq + 1));
        hot += "  " + (n / totalOps * 100).toFixed(1).padStart(5) + "%  " +
               tok.padEnd(5) + opName(tok) + "\n";
      }
      const emuMs = dbg.emuMs / secs, renderMs = dbg.renderMs / secs;
      const fps = dbg.frames / secs, dropped = dbg.dropped / secs;
      const busy = emuMs + renderMs;
      const posted = dbg.posted / secs, dspRate = dbg.dsp / secs, fmRate = dbg.fm / secs;
      dbg.emuMs = dbg.renderMs = dbg.dropped = 0;
      dbg.frames = dbg.pumps = 0;
      dbg.posted = dbg.dsp = dbg.fm = 0;

      // The ring's own numbers, reported by the worklet twice a second. A
      // starved ring is what "the sound got off" is: the emulator can be
      // keeping perfect time and the audio still break up, because the ring
      // is fed from the main thread and a stall there empties it.
      let audio = "audio   off\n";
      if (audioCtx && sbNode && speakerCheckbox.checked) {
        const sr = audioCtx.sampleRate;
        const st = audioStats;
        const depthMs = st ? (st.depth / sr) * 1000 : 0;
        const starvedMs = st && st.secs ? (st.starved / sr) * 1000 / st.secs : 0;
        const trimMs = st && st.secs ? (st.trimmed / sr) * 1000 / st.secs : 0;
        const outLat = audioCtx.outputLatency || audioCtx.baseLatency || 0;
        audio =
          "audio   ring " + depthMs.toFixed(0) + " ms of 50 target   " +
          audioCtx.state + " " + (sr / 1000).toFixed(1) + " kHz\n" +
          "        starved " + starvedMs.toFixed(1) + " ms/s   trimmed " +
          trimMs.toFixed(1) + " ms/s   latency " + (outLat * 1000).toFixed(0) + " ms\n" +
          "        fed " + (posted / 1000).toFixed(1) + "k/s   dsp " +
          (dspRate / 1000).toFixed(1) + "k/s   fm " + (fmRate / 1000).toFixed(1) + "k/s\n";
      }
      // "CPU" here is the share of ONE core this page's main thread is
      // using -- the browser exposes no system-wide figure, and claiming one
      // would be inventing it. Emulation and drawing share that thread, so
      // it is also the number that decides whether the pump starts dropping.
      // Clamp: a sample window can attribute more than `secs` of emu+draw
      // when a long runCycles chunk straddles the tick (busy/10 > 100), but
      // a single thread cannot honestly exceed one core.
      const cpuPct = Math.min(100, busy / 10);
      const cores = navigator.hardwareConcurrency || 0;
      // Guest RAM lives inside the wasm heap, so its size is the emulator's
      // real memory footprint. performance.memory is Chrome-only.
      const heapMB = machine.heapBytes() / 1048576;
      const jsMem = performance.memory
        ? (performance.memory.usedJSHeapSize / 1048576).toFixed(0) + " / " +
          (performance.memory.jsHeapSizeLimit / 1048576).toFixed(0) + " MB JS heap"
        : "JS heap n/a";

      // The 486's own CPU usage: the share of its cycles spent doing work
      // rather than halted waiting for an interrupt. Bare DOS busy-waits at
      // the prompt instead of halting, so 100% here is the honest, period
      // answer -- an idle driver (FreeDOS's FDAPM, DOS 6's POWER) is what
      // makes it drop, exactly as on the real machine.
      const idlePct = cyc > 0 ? Math.min(100, ((halt + idlePoll) / cyc) * 100) : 0;
      const guestPct = 100 - idlePct;
      const targetHz = machine.cpuHz();
      const targetMhz = targetHz / 1e6;
      history.push({ cpu: guestPct, clock: mhz / targetMhz * 100, host: cpuPct, fps: fps });
      if (history.length > 60) history.shift();
      drawPerfChart(cctx, chart, history);
      drawHostChart(cctx2, chart2, history);

      out.innerHTML =
        "clock   " + mhz.toFixed(1) + " MHz of " + targetMhz.toFixed(1) + "  (" + (mhz / targetMhz * 100).toFixed(0) + "%)\n" +
        "        avg " + avg.toFixed(1) + " (30s)   min " + worst.toFixed(1) + "\n" +
        "dropped " + m(dropped) + " cyc  " + (dropped / targetHz * 100).toFixed(1) + "% of clock\n" +
        "\n" +
        "486 cpu " + guestPct.toFixed(0) + "% busy   " + idlePct.toFixed(0) + "% idle\n" +
        "        idle = halted or spinning in a DOS wait loop\n" +
        "\n" +
        "<b>host</b>    " + cpuPct.toFixed(0) + "% of one core" +
        (cores ? "  (" + cores + " cores)" : "") + "\n" +
        "  emul  " + emuMs.toFixed(0) + " ms/s\n" +
        "  draw  " + renderMs.toFixed(0) + " ms/s   " + fps.toFixed(1) + " fps\n" +
        "  heap  " + heapMB.toFixed(0) + " MB   " + jsMem + "\n" +
        "\n" + audio +
        (tier2
          ? "\ninstrs  " + m(per.instrs) + "   " +
            (per.instrs ? (cyc / secs / per.instrs).toFixed(2) : "0") + " cyc/instr\n" +
            "tlb miss " + m(per.tlb_miss) + "\n" +
            "fetch slow " + m(per.fetch_slow) + "\n" +
            "mmio    " + m(per.mmio) + "\n" +
            "service " + m(per.services) + "\n" +
            "\nhot instructions (share of all executed)\n" + hot
          : "");
    }, 1000);
  }

  // ---- main loop ---------------------------------------------------
  // Two loops, deliberately split:
  //
  //   pump()  advances the emulated machine, in chunks bounded by REAL
  //           wall-clock time, yielding to the event loop between them.
  //   frame() (requestAnimationFrame) only draws the screen and the LEDs.
  //
  // They are separate because runCycles() is synchronous: for its entire
  // duration the main thread dispatches nothing -- no keydown, no click,
  // not even the next rAF callback. Running a whole frame's worth of cycles
  // in one call therefore makes input latency equal to however long that
  // call takes, and when the host can't quite sustain 66 MHz the loop
  // settles into back-to-back multi-hundred-millisecond calls with no gap
  // at all between them, which is what made keystrokes take seconds to
  // register or vanish entirely. See PC486_REVIEW.md §8.
  const ctx = screenEl.getContext("2d");
  const hddLed = document.getElementById("hddLed");
  let cycleCredit = 0, lastT = null;

  // Longest any single runCycles() call may hold the main thread. Not a
  // speed control: at real 66 MHz this host needs ~15 ms of wall clock per
  // 60 Hz frame, so a 12 ms chunk simply means "about one frame of emulated
  // time, delivered in two pieces with a yield in the middle" rather than
  // any reduction in emulated speed.
  const kChunkMs = 12;
  // Measured host throughput in cycles per real millisecond, used to turn
  // kChunkMs into a cycle count. Seeded at the real 66 MHz rate and then
  // tracked with a slow EWMA, so the chunk size follows the actual machine
  // this page is running on instead of a hardcoded guess.
  let cyclesPerMs = 66000;

  // A macrotask yield, not a microtask: queueMicrotask/Promise.resolve()
  // would run the next chunk within the SAME event-loop turn and dispatch
  // no input at all. MessageChannel is the standard zero-delay macrotask
  // (setTimeout(0) is clamped to ~4 ms, which would cap emulated speed).
  const pumpChannel = new MessageChannel();
  let pumpScheduled = false;
  pumpChannel.port1.onmessage = () => { pumpScheduled = false; pump(); };
  function schedulePump() {
    if (pumpScheduled) return;
    pumpScheduled = true;
    pumpChannel.port2.postMessage(0);
  }

  // pump() reschedules itself via a zero-delay macrotask (deliberately, so
  // CPU throughput isn't capped by setTimeout's ~4ms clamp -- see
  // schedulePump()'s own comment) -- but that means when the emulated CPU
  // is idle (HLT, waiting on the next timer interrupt) with nothing to run,
  // pump() can re-enter thousands of times per real second doing no CPU
  // work at all. Calling pumpAudio()/pumpSbAudio() on every one of those
  // spins used to flood both worklets with thousands of near-empty
  // messages a second; each one's sampleCount is floored to at least one
  // sample (Math.max(1, ...) in both functions, so a genuinely tiny real
  // dt still gets *some* coverage rather than a gap), and at that rate the
  // worklet's own real-time-paced resync trim couldn't keep the ring's
  // read/write pointers from drifting past positions process() hadn't
  // reached yet -- including stale audio from a *previous* sound sitting
  // in ring slots the flood hadn't caught up to overwriting. Heard as that
  // sound looping on its own long after the emulator had gone silent (see
  // PC486_REVIEW.md's audio investigation). Coalescing here -- accumulating
  // cycles/dt across spins and only actually pumping audio once enough
  // real time has passed to be worth a message -- fixes it at the source:
  // a normal ~12ms-cadence call already exceeds the threshold and flushes
  // immediately (unchanged from before), only the pathological idle-spin
  // case gets batched.
  const kMinAudioPumpDt = 0.001;  // 1ms -- far below anything perceptible
  let audioPumpStartCycle = null, audioPumpCycles = 0, audioPumpDt = 0;
  function pumpAudioCoalesced(chunkStartCycle, cyclesThisChunk, dtSeconds) {
    if (audioPumpStartCycle === null) audioPumpStartCycle = chunkStartCycle;
    audioPumpCycles += cyclesThisChunk;
    audioPumpDt += dtSeconds;
    if (audioPumpDt < kMinAudioPumpDt) return;
    pumpAudio(audioPumpStartCycle, audioPumpCycles, audioPumpDt);
    pumpSbAudio(audioPumpStartCycle, audioPumpCycles, audioPumpDt);
    audioPumpStartCycle = null;
    audioPumpCycles = 0;
    audioPumpDt = 0;
  }

  function clearScreenToBlack() {
    screenEl.width = kTextRenderWidth;
    screenEl.height = kTextRenderHeight;
    screenEl.style.aspectRatio = kTextRenderWidth + " / " + kTextRenderHeight;
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, screenEl.width, screenEl.height);
  }
  const kTextRenderWidth = 640, kTextRenderHeight = 350;  // matches ega_render.h's text-mode default

  function pump() {
    if (!poweredOn || !machine) return;  // power switched off mid-loop -- stop, don't reschedule
    const t = performance.now();
    if (lastT === null) lastT = t;
    let dtSeconds = (t - lastT) / 1000;
    lastT = t;
    dtSeconds = Math.min(dtSeconds, 0.25);  // clamp a backgrounded-tab gap -- no runaway catch-up burst

    // Real rate for the selected Turbo state -- 66 MHz on / 33 MHz off
    // (DX2 clock doubling). Never sped above that for a real visitor
    // (CLAUDE.md). TEST_CPU_MULTIPLIER is 1 outside `?test=1&fast=1`.
    cycleCredit += dtSeconds * cpuHz * TEST_CPU_MULTIPLIER;
    let cyclesThisChunk = Math.floor(cycleCredit);

    // Bound this one call's wall-clock cost. Anything over the chunk budget
    // is DROPPED rather than banked: a host that cannot sustain the
    // requested rate would never work a backlog off, and each attempt to
    // try is exactly the long, input-starving call this bound exists to
    // prevent. The visible consequence is that emulated time runs slow on a
    // host that can't keep up -- which is a departure from the real
    // machine, and the deliberate one: a 486 that runs 10% slow is closer
    // to the real article than a 486 whose keyboard stops answering.
    const chunkBudget = Math.max(1, Math.floor(cyclesPerMs * kChunkMs));
    if (cyclesThisChunk > chunkBudget) {
      if (dbg.on) dbg.dropped += cycleCredit - chunkBudget;
      cyclesThisChunk = chunkBudget;
      cycleCredit = 0;
    } else {
      cycleCredit -= cyclesThisChunk;
    }

    const chunkStartCycle = machine.totalCycles();
    if (cyclesThisChunk > 0) {
      const t0 = performance.now();
      machine.runCycles(cyclesThisChunk);
      const elapsed = performance.now() - t0;
      // Only re-measure off a chunk long enough to time meaningfully;
      // performance.now()'s resolution makes a sub-millisecond sample noise.
      if (elapsed >= 1) cyclesPerMs += 0.25 * (cyclesThisChunk / elapsed - cyclesPerMs);
      if (dbg.on) { dbg.emuMs += elapsed; dbg.pumps++; }
    }

    pumpAudioCoalesced(chunkStartCycle, cyclesThisChunk, dtSeconds);
    schedulePump();
  }

  function frame(t) {
    if (!poweredOn || !machine) return;  // power switched off mid-loop -- stop, don't reschedule
    const frameT0 = dbg.on ? performance.now() : 0;
    const blinkOn = Math.floor(t / 266) % 2 === 0;  // ~1.9Hz block-cursor blink
    const rgba = machine.renderFrame(blinkOn);
    // Resolution varies by mode (640x400 text, 320x200 CGA-compatible and
    // VGA 256-color graphics, 640x350 native 16-color EGA, up to 640x400
    // in the card's SVGA modes -- see ega_render.h) -- resize the canvas's own pixel
    // buffer to match whenever it changes, and let it fill its native
    // aspect ratio rather than stretching a lower-res mode into the text
    // mode's box (no real hardware basis to prefer one distortion over
    // another, so: don't introduce one).
    const frameW = machine.renderWidth(), frameH = machine.renderHeight();
    if (screenEl.width !== frameW || screenEl.height !== frameH) {
      screenEl.width = frameW;
      screenEl.height = frameH;
      screenEl.style.aspectRatio = frameW + " / " + frameH;
    }
    const img = ctx.createImageData(frameW, frameH);
    img.data.set(rgba);
    ctx.putImageData(img, 0, 0);

    floppyBay.querySelector('[data-role="led"]').classList.toggle(
      "on", machine.floppyPresent() && machine.floppyMotorOn());
    cdromBay.querySelector('[data-role="led"]').classList.toggle("on", machine.cdromBusy());
    hddLed.classList.toggle("on", machine.hddBusy());

    if (dbg.on) { dbg.renderMs += performance.now() - frameT0; dbg.frames++; }
    requestAnimationFrame(frame);
  }

  // ---- hard disk persistence (IndexedDB) ---------------------------------
  // A real fixed disk keeps its contents when the machine is off; this
  // emulator's own Machine is fully discarded on power-off (see the power
  // switch section below), so without this C: would silently revert to
  // whatever it was mount()ed with every single power-on. One record in
  // one object store -- there's only ever one C: drive to remember.
  const HDD_DB_NAME = "pc486-hdd", HDD_STORE = "hdd", HDD_KEY = "c-drive";
  // Pristine factory FreeDOS bytes, stashed on first download so a
  // factory-delta C: can reconstruct across reloads without touching the
  // network (Cache API silently refuses a 504MB put on some hosts).
  const HDD_FACTORY_KEY = "factory-base";
  function openHddDb() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(HDD_DB_NAME, 1);
      req.onupgradeneeded = () => req.result.createObjectStore(HDD_STORE);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }
  // IDB value is either a full Uint8Array (blank / upload / legacy) or a
  // factory-delta record: { v:1, base:"factory", patches:[{offset, bytes}] }.
  // Deltas are what a normal FreeDOS session writes (FDAUTO, a few files) --
  // putting the whole 504MB image every dirty tick froze the main thread for
  // minutes in Playwright and was the wrong default for visitors too.
  function isFactoryDeltaRecord(raw) {
    return !!(raw && raw.v === 1 && raw.base === "factory" && Array.isArray(raw.patches));
  }
  async function loadSavedHddRecord() {
    try {
      const db = await openHddDb();
      const rec = await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readonly");
        const req = tx.objectStore(HDD_STORE).get(HDD_KEY);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
      });
      if (rec instanceof Blob) return await gunzipBlob(rec);
      if (rec) return rec;  // raw bytes, or a factory-delta record, from an older build
      // A save written in 512KB chunks by the build before this one.
      const meta = await getLegacyChunkMeta();
      if (meta && meta.v === 1 && meta.chunks > 0) {
        const out = new Uint8Array(meta.length);
        for (let i = 0; i < meta.chunks; i++) {
          const chunk = await new Promise((resolve, reject) => {
            const tx = db.transaction(HDD_STORE, "readonly");
            const req = tx.objectStore(HDD_STORE).get(HDD_KEY + ":" + i);
            req.onsuccess = () => resolve(req.result || null);
            req.onerror = () => reject(req.error);
          });
          if (!chunk) return null;  // incomplete -- treat as no save
          out.set(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk), i * meta.chunkSize);
        }
        return out;
      }
      return null;
    } catch (err) {
      console.error("could not read saved hard disk, using factory default:", err);
      return null;
    }
  }
  async function loadFactoryFromIdb() {
    try {
      const db = await openHddDb();
      const meta = await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readonly");
        const req = tx.objectStore(HDD_STORE).get(FACTORY_META_KEY);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
      });
      if (meta && meta.v === 1 && meta.chunks > 0) {
        const out = new Uint8Array(meta.length);
        for (let i = 0; i < meta.chunks; i++) {
          const chunk = await new Promise((resolve, reject) => {
            const tx = db.transaction(HDD_STORE, "readonly");
            const req = tx.objectStore(HDD_STORE).get(HDD_FACTORY_KEY + ":" + i);
            req.onsuccess = () => resolve(req.result || null);
            req.onerror = () => reject(req.error);
          });
          if (!chunk) return null;
          const u8 = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
          out.set(u8, i * meta.chunkSize);
        }
        return out;
      }
      // Legacy single-blob / Uint8Array stash from an earlier session.
      return await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readonly");
        const req = tx.objectStore(HDD_STORE).get(HDD_FACTORY_KEY);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
      });
    } catch (_) {
      return null;
    }
  }
  let factoryIdbChain = Promise.resolve();
  let factoryIdbCurrent = false;
  // Chunk the factory stash so a single IndexedDB put never freezes the
  // main thread for hundreds of ms (smoke.spec.ts's longtask budget is
  // 150ms; a one-shot 504MB put blew past that during realtime boot).
  const FACTORY_CHUNK = 512 * 1024;
  const FACTORY_META_KEY = "factory-base-meta";
  function stashFactoryInIdb(bytes, fp) {
    if (factoryIdbCurrent || !bytes) return factoryIdbChain;
    factoryIdbChain = factoryIdbChain.then(async () => {
      if (factoryIdbCurrent) return;
      let u8;
      if (bytes instanceof Uint8Array) u8 = bytes;
      else if (bytes instanceof ArrayBuffer) u8 = new Uint8Array(bytes);
      else if (bytes instanceof Blob) u8 = new Uint8Array(await bytes.arrayBuffer());
      else u8 = new Uint8Array(bytes);
      const db = await openHddDb();
      const n = Math.ceil(u8.length / FACTORY_CHUNK) || 1;
      for (let i = 0; i < n; i++) {
        const chunk = u8.slice(i * FACTORY_CHUNK, Math.min((i + 1) * FACTORY_CHUNK, u8.length));
        await new Promise((resolve, reject) => {
          const tx = db.transaction(HDD_STORE, "readwrite");
          tx.objectStore(HDD_STORE).put(chunk, HDD_FACTORY_KEY + ":" + i);
          tx.oncomplete = resolve;
          tx.onerror = () => reject(tx.error);
        });
        // Yield so rAF / input / the smoke longtask observer see a gap.
        await new Promise((r) => setTimeout(r, 0));
      }
      await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readwrite");
        const store = tx.objectStore(HDD_STORE);
        // fp: the fingerprint ensureFactoryHdd() last confirmed these bytes
        // against (null/unknown if it never got a successful HEAD this
        // session) -- see factoryFingerprint() above.
        store.put({ v: 1, chunks: n, length: u8.length, chunkSize: FACTORY_CHUNK, fp: fp || null }, FACTORY_META_KEY);
        store.delete(HDD_FACTORY_KEY);  // legacy single-blob key, if any
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
      factoryIdbCurrent = true;
    }).catch((err) => {
      console.error("could not stash factory hard disk:", err);
    });
    return factoryIdbChain;
  }
  // C: is stored gzipped as a single Blob under HDD_KEY, matching
  // ibmpc-at's one-entry shape. Raw, this image is 504MB -- far too big for
  // one structured clone -- but it is mostly zeros, so gzip takes it to a few
  // MB in about a second, and a Blob is stored by reference rather than
  // deep-copied (measured: 10ms, against 31ms for one array and 36ms for the
  // 512KB chunking this replaces).
  async function gzipBytes(u8) {
    const cs = new CompressionStream("gzip");
    const stream = new Blob([u8]).stream().pipeThrough(cs);
    return await new Response(stream).blob();
  }
  async function gunzipBlob(blob) {
    const ds = new DecompressionStream("gzip");
    const buf = await new Response(blob.stream().pipeThrough(ds)).arrayBuffer();
    return new Uint8Array(buf);
  }
  async function saveHdd(record) {
    try {
      const u8 = record instanceof Uint8Array ? record : new Uint8Array(record);
      const blob = await gzipBytes(u8);
      const db = await openHddDb();
      await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readwrite");
        tx.objectStore(HDD_STORE).put(blob, HDD_KEY);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
      await dropLegacyHddChunks();
    } catch (err) {
      console.error("could not save hard disk changes:", err);
    }
  }
  // Chunk records written by the build that stored C: uncompressed.
  const HDD_CHUNK_META = "hdd-meta";
  function getLegacyChunkMeta() {
    return openHddDb().then((db) => new Promise((resolve, reject) => {
      const tx = db.transaction(HDD_STORE, "readonly");
      const req = tx.objectStore(HDD_STORE).get(HDD_CHUNK_META);
      req.onsuccess = () => resolve(req.result || null);
      req.onerror = () => reject(req.error);
    })).catch(() => null);
  }
  async function dropLegacyHddChunks() {
    const meta = await getLegacyChunkMeta();
    if (!meta || meta.v !== 1) return;
    const db = await openHddDb();
    await new Promise((resolve, reject) => {
      const tx = db.transaction(HDD_STORE, "readwrite");
      const store = tx.objectStore(HDD_STORE);
      for (let i = 0; i < meta.chunks; i++) store.delete(HDD_KEY + ":" + i);
      store.delete(HDD_CHUNK_META);
      tx.oncomplete = resolve;
      tx.onerror = () => reject(tx.error);
    });
  }
  async function clearSavedHdd() {
    try {
      const db = await openHddDb();
      await dropLegacyHddChunks();
      await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readwrite");
        // Leave HDD_FACTORY_KEY alone -- a "Reset to factory" still wants
        // the pristine image locally so the next power-on is not a download.
        tx.objectStore(HDD_STORE).delete(HDD_KEY);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
    } catch (err) {
      console.error("could not clear saved hard disk:", err);
    }
  }

  // ---- power switch (off by default) -------------------------------------
  // Flipping power off cuts power to everything -- RAM (and so every bit of
  // running state) is gone, exactly like unplugging it, while a diskette or
  // CD physically stays seated in its drive regardless. Modeled the same
  // way here: powering off discards the whole Machine instance; powering
  // back on builds a fresh one and re-mounts whatever floppy/CD-ROM images
  // were still "in the drive" (remembered in JS, not the discarded Machine)
  // when power was cut. Unlike ibmpc-at's genuine 5170 (no front-panel reset
  // button on real hardware), this is a period clone-era tower case with a
  // real momentary Reset button -- see resetBtn below -- separate from the
  // Power LED/switch on the tower strip.
  const powerSwitch = document.getElementById("powerSwitch");
  const powerLed = document.getElementById("powerLed");
  const resetBtn = document.getElementById("resetBtn");
  const turboBtn = document.getElementById("turboBtn");
  const turboLed = document.getElementById("turboLed");
  // Turbo models a real DX2 clock-doubling switch: on = 66 MHz internal,
  // off = 33 MHz bus rate. The PIT crystal and device wall-clock pacing
  // stay correct (Machine::set_turbo); only CPU instruction throughput
  // drops. The seven-segment readout tracks the selected rate. Default
  // on -- matching a tower shipped with Turbo engaged.
  let cpuHz = 66000000;
  // Segment maps for digits this panel shows (3 and 6 only).
  const kSevenSegOn = {
    3: { a: 1, b: 1, c: 1, d: 1, g: 1 },
    6: { a: 1, c: 1, d: 1, e: 1, f: 1, g: 1 },
  };
  function setClockDisplay(mhz) {
    const tens = Math.floor(mhz / 10) % 10;
    const ones = mhz % 10;
    const digits = document.querySelectorAll("#clockDisplay .sevenseg");
    [tens, ones].forEach((n, i) => {
      const on = kSevenSegOn[n] || {};
      digits[i].querySelectorAll("i").forEach((seg) => {
        seg.classList.toggle("on", !!on[seg.classList[0]]);
      });
    });
  }
  function applyTurbo(on) {
    turboBtn.setAttribute("aria-pressed", on ? "true" : "false");
    turboLed.classList.toggle("turbo-on", on);
    cpuHz = on ? 66000000 : 33000000;
    setClockDisplay(on ? 66 : 33);
    if (machine) machine.setTurbo(on);
  }
  turboBtn.addEventListener("click", () => {
    applyTurbo(turboBtn.getAttribute("aria-pressed") !== "true");
  });
  let poweredOn = false;
  let firmware = null;  // {Module, bios, vga, hdd} once fetched -- fetched once, reused every power-on
  // 1024 cyl x 16 head x 63 sec/track x 512 bytes -- this machine's one
  // fixed C: geometry (wd1003.cpp), known without needing the actual
  // factory FreeDOS bytes downloaded yet.
  const kHddImageBytes = 528482304;
  let pendingFloppy = null;  // {name, bytes} -- "what's physically in the drive" (this system's one bay)
  let pendingCdrom = null;   // {name, bytes} -- same idea, for the CD-ROM bay

  resetBtn.addEventListener("click", () => {
    // A real reset button pulses the RESET line to the CPU/chipset only --
    // RAM and CMOS both survive, unlike the power switch above. No-op while
    // powered off, matching a real machine (nothing to reset without power).
    if (machine) machine.reset();
  });

  // What C: actually mounts next power-on: a saved image from IndexedDB
  // (whatever it last held -- factory FreeDOS with changes, a blank drive
  // mid-install, or a real OS the visitor installed themselves) if one
  // exists, otherwise the pristine fetched factory image. `hddLabel`
  // exists purely to describe that choice in the status line below.
  let savedHdd = null;   // Uint8Array | null
  // True when IndexedDB already holds the current `savedHdd` mirror.
  let hddIdbCurrent = false;
  // "factory" = IDB holds a delta against the shipped FreeDOS image (Cache
  // API keeps that image across reloads so we never re-fetch it). "full" =
  // IDB holds the entire Uint8Array (blank drive, user upload, legacy).
  let hddSaveKind = null;  // "factory" | "full" | null
  // offset → bytes for the factory-delta record; rebuilt on load, merged
  // on every dirty persist. Cleared when the visitor blanks/uploads/resets.
  let hddLabel = "factory FreeDOS (default)";
  // Lazily populated -- only fetched when a session actually needs the
  // pristine factory image (no IndexedDB C: yet, or "Reset to factory").
  // Starting the fetch eagerly used to download ~7MB gzip on every reload
  // even when savedHdd already covered the mount (see ensureFactoryHdd).
  // Survives reloads via the Cache API so a factory-delta IDB record can
  // reconstruct C: without touching the network again.
  let factoryHddPromise = null;
  function factoryHddUrl() {
    return new URL("disks/freedos-hdd.img", location.href).href;
  }
  // Cheap identity for whatever freedos-hdd.img the server is currently
  // shipping, so a stash from a build that has since been replaced can be
  // told apart from a current one -- without downloading the 504MB body.
  // ETag if the server sends one, else Last-Modified, plus Content-Length.
  // Memoized: only one HEAD per page load, no matter how many callers ask.
  let factoryFingerprintPromise = null;
  function factoryFingerprint() {
    if (!factoryFingerprintPromise) {
      factoryFingerprintPromise = (async () => {
        try {
          const r = await fetch(factoryHddUrl(), { method: "HEAD", cache: "no-store" });
          if (!r.ok) return null;
          const id = r.headers.get("ETag") || r.headers.get("Last-Modified");
          return id ? id + "|" + (r.headers.get("Content-Length") || "") : null;
        } catch (err) {
          // Offline / CORS / server down -- fail open, not closed: a stale
          // image beats a machine that won't boot at all.
          console.error("could not check factory hard disk freshness, using any cached copy:", err);
          return null;
        }
      })();
    }
    return factoryFingerprintPromise;
  }
  async function getFactoryMeta() {
    try {
      const db = await openHddDb();
      return await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readonly");
        const req = tx.objectStore(HDD_STORE).get(FACTORY_META_KEY);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
      });
    } catch (_) {
      return null;
    }
  }
  // Drops every trace of a stale stash -- the chunked IDB bytes (if any),
  // the legacy single-blob key, the meta record, and the Cache API entry --
  // so ensureFactoryHdd() falls through to a real network fetch below.
  async function purgeFactoryStash() {
    try {
      const db = await openHddDb();
      const meta = await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readonly");
        const req = tx.objectStore(HDD_STORE).get(FACTORY_META_KEY);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
      });
      await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readwrite");
        const store = tx.objectStore(HDD_STORE);
        const chunks = meta && meta.v === 1 && meta.chunks > 0 ? meta.chunks : 0;
        for (let i = 0; i < chunks; i++) store.delete(HDD_FACTORY_KEY + ":" + i);
        store.delete(HDD_FACTORY_KEY);  // legacy single-blob key, if any
        store.delete(FACTORY_META_KEY);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
    } catch (err) {
      console.error("could not clear stale factory hard disk stash:", err);
    }
    factoryIdbCurrent = false;
  }
  // A tiny record of what's now cached, written right after a genuine
  // network fetch -- stashFactoryInIdb() later overwrites this with the
  // real chunked byte-stash (same fp) if that ever runs (see its comment).
  // chunks:0 tells loadFactoryFromIdb() there's no byte-stash to read yet.
  async function recordFactoryFingerprint(fp, length) {
    try {
      const db = await openHddDb();
      await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readwrite");
        tx.objectStore(HDD_STORE).put(
          { v: 1, chunks: 0, length, chunkSize: FACTORY_CHUNK, fp }, FACTORY_META_KEY);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
    } catch (err) {
      console.error("could not record factory hard disk fingerprint:", err);
    }
  }
  // Fingerprint last confirmed by a HEAD this session (null if never
  // checked or the HEAD failed) -- stashFactoryInIdb() reads this so a test-
  // triggered full byte-stash records the same identity ensureFactoryHdd()
  // already validated. Set true when a stale stash was found and purged;
  // the boot flow below reads it to also drop a now-meaningless
  // factory-delta save (its patches are offsets into a base that's gone).
  let currentFactoryFingerprint = null;
  let factoryStashInvalidated = false;
  // Drops every local copy of the factory image -- stash, Cache API entry,
  // memoised promise, in-memory bytes -- so the fetch that follows is an
  // unconditional trip to the network.
  function refetchFactoryHdd() {
    factoryHddPromise = null;
    factoryIdbCurrent = false;
    factoryStashInvalidated = false;
    currentFactoryFingerprint = null;
    if (firmware) firmware.hdd = null;
    return (async () => {
      await purgeFactoryStash();
      return ensureFactoryHdd();
    })();
  }

  function ensureFactoryHdd() {
    if (firmware && firmware.hdd) return Promise.resolve(firmware.hdd);
    if (!factoryHddPromise) {
      factoryHddPromise = (async () => {
        // Check the shipped image's identity before trusting anything
        // already stashed. A HEAD failure fails open (see factoryFingerprint
        // above) -- only a confirmed mismatch triggers a purge.
        const liveFp = await factoryFingerprint();
        if (liveFp !== null) {
          const meta = await getFactoryMeta();
          // A stash with no recorded fingerprint (written before this fix
          // existed) is unknown, not a match -- it must not pass silently.
          const stashFp = meta && typeof meta.fp === "string" ? meta.fp : null;
          if (stashFp !== liveFp) {
            await purgeFactoryStash();
            factoryStashInvalidated = true;
          }
        }
        // Prefer the IndexedDB stash from a prior visit -- avoids re-fetching
        // 504MB when C: is a factory-delta record (see persistHddIfDirty).
        const fromIdb = factoryStashInvalidated ? null : await loadFactoryFromIdb();
        if (fromIdb) {
          factoryIdbCurrent = true;
          let bytes;
          if (fromIdb instanceof Blob) bytes = await fromIdb.arrayBuffer();
          else if (fromIdb instanceof ArrayBuffer) bytes = fromIdb;
          else bytes = fromIdb.buffer.slice(fromIdb.byteOffset, fromIdb.byteOffset + fromIdb.byteLength);
          if (firmware) firmware.hdd = bytes;
          currentFactoryFingerprint = liveFp;
          return bytes;
        }
        // Not kept in Cache storage: that was a second ~504MB copy of bytes
        // this page barely re-reads. C: is saved whole and self-contained, so
        // a returning visitor never needs the shipped image, and Reset
        // deliberately re-fetches it rather than trusting any local copy.
        const r = await fetch("disks/freedos-hdd.img");
        if (!r.ok) throw new Error("freedos-hdd.img HTTP " + r.status);
        const bytes = await r.arrayBuffer();
        const fetchedFresh = true;
        if (firmware) firmware.hdd = bytes;
        currentFactoryFingerprint = liveFp;
        if (fetchedFresh && liveFp !== null) recordFactoryFingerprint(liveFp, bytes.byteLength);
        // Do not stash into IndexedDB here: a 504MB structured-clone (even
        // chunked) is a main-thread longtask under suite load and trips
        // smoke.spec.ts's 150ms budget during realtime boot. Stashing is
        // kicked off only by whenFactoryStashed() (the no-re-fetch spec).
        return bytes;
      })();
    }
    return factoryHddPromise;
  }
  function applyFactoryPatches(base, patches) {
    const img = base instanceof Uint8Array ? base : new Uint8Array(base);
    for (const p of patches) {
      const b = p.bytes instanceof Uint8Array ? p.bytes : new Uint8Array(p.bytes);
      img.set(b, p.offset | 0);
    }
    return img;
  }
  const hddStatus = document.getElementById("hddStatus");
  const hddResetBtn = document.getElementById("hddResetBtn");
  const hddBlankBtn = document.getElementById("hddBlankBtn");
  const hddDownloadBtn = document.getElementById("hddDownloadBtn");
  const hddUploadInput = document.getElementById("hddUploadInput");
  function refreshHddControls() {
    hddStatus.textContent = "Using: " + hddLabel;
    // A real fixed disk can't be swapped while the machine is running --
    // every one of these actions only ever affects the *next* power-on.
    hddResetBtn.disabled = !firmware || poweredOn;
    hddBlankBtn.disabled = !firmware || poweredOn;
    hddDownloadBtn.disabled = !firmware;  // download works even while running -- it's read-only
    hddUploadInput.disabled = !firmware || poweredOn;
    document.getElementById("hddUploadBtn").disabled = !firmware || poweredOn;
  }
  hddResetBtn.addEventListener("click", () => {
    savedHdd = null;
    hddIdbCurrent = false;
    hddSaveKind = null;
    hddLabel = "factory FreeDOS (default) -- takes effect next power-on";
    refreshHddControls();
    clearSavedHdd();
    // "Reset to factory" means the image the server has right now: no
    // fingerprint to trust, nothing local to fall back on. The stash stays
    // an optimisation for ordinary page loads only -- a factory-delta C:
    // would otherwise re-download 504MB on every visit.
    refetchFactoryHdd();
  });
  hddBlankBtn.addEventListener("click", () => {
    if (!firmware) return;
    savedHdd = new Uint8Array(kHddImageBytes);  // all zero -- unformatted, like a drive fresh from the factory floor
    hddIdbCurrent = false;
    hddSaveKind = "full";
    hddLabel = "blank drive, unformatted (FDISK/FORMAT and install your own OS) -- takes effect next power-on";
    refreshHddControls();
    saveHdd(savedHdd).then(() => { hddIdbCurrent = true; });
  });
  // A real file on the visitor's own disk, independent of this browser's
  // storage -- the same "save modified media" idea the floppy eject flow
  // already offers, just for C: (which isn't ejectable, so it needs its
  // own explicit control instead of piggybacking on a drive-swap gesture).
  hddDownloadBtn.addEventListener("click", async () => {
    if (!firmware) return;
    // Whatever is *actually* current: the live, possibly-just-written
    // image if the machine is running, else whatever's staged for the
    // next power-on, else the pristine factory image.
    if (!(poweredOn && machine) && !savedHdd) await ensureFactoryHdd();
    const bytes = (poweredOn && machine) ? machine.hddImage() : (savedHdd || new Uint8Array(firmware.hdd));
    const blob = new Blob([bytes], { type: "application/octet-stream" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "pc486-hdd.img";
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  });
  hddUploadInput.addEventListener("change", async () => {
    const f = hddUploadInput.files[0];
    hddUploadInput.value = "";
    if (!f || !firmware) return;
    const bytes = new Uint8Array(await f.arrayBuffer());
    // This system's WD1003 geometry (1024 cyl/16 head/63 sec, see
    // wd1003.cpp -- the real pre-EIDE INT13h CHS ceiling, 504MB) is fixed
    // in CMOS, not derived from the image. An image of the wrong size
    // would still fail safely (wd1003.cpp's own bounds check reports a
    // genuine IDNF error rather than silently doing nothing), but refusing
    // it up front gives a clearer reason than a mysterious disk error deep
    // into a boot.
    if (bytes.byteLength !== kHddImageBytes) {
      alert("That file is " + bytes.byteLength + " bytes; this machine's hard disk " +
            "must be exactly " + kHddImageBytes + " bytes (1024 cyl / 16 head / " +
            "63 sec/track, 504MB). Not mounted.");
      return;
    }
    savedHdd = bytes;
    hddIdbCurrent = false;
    hddSaveKind = "full";
    hddLabel = "uploaded image (" + f.name + ") -- takes effect next power-on";
    refreshHddControls();
    saveHdd(savedHdd).then(() => { hddIdbCurrent = true; });
  });

  async function powerOn() {
    if (poweredOn || !firmware) return;
    if (!machine) {
      // Free the previous power cycle's machine before building the next
      // one. An embind handle owns a C++ object that outlives the JS
      // reference, so dropping `machine` at powerOff() freed nothing and
      // every power cycle leaked another ~528MB of wasm heap (C:'s image;
      // +419MB more whenever a CD-ROM disc happens to be loaded too) -- the
      // second power-on then died trying to grow past it. Deleted here
      // rather than in powerOff() so the handle
      // stays callable while the machine is off, which is how "power off
      // really does stop the cycle counter" is observed (tests/boot.spec.ts).
      if (lastMachine) {
        lastMachine.delete();
        lastMachine = null;
      }
      machine = new firmware.Module.Machine();
      applyTurbo(turboBtn.getAttribute("aria-pressed") === "true");
      if (perfRequested) startPerfPanel();
      machine.loadRom(0x100000 - firmware.bios.byteLength, new Uint8Array(firmware.bios));
      machine.loadRom(0xC0000, new Uint8Array(firmware.vga));
      // Factory image is fetched on demand -- a session that already has
      // C: in IndexedDB never downloads it (see ensureFactoryHdd). Keep the
      // Uint8Array we mount as `savedHdd` so the first dirty persist can
      // patch it (and write a small factory-delta to IDB) instead of
      // copying all 504MB out of wasm.
      if (!savedHdd) {
        await ensureFactoryHdd();
        savedHdd = new Uint8Array(firmware.hdd);
        // Self-contained: once anything is written, C: is stored whole and
        // depends on nothing else. A delta keyed to the shipped image is
        // smaller, but it dies whenever that image changes -- and losing
        // installed software is a far worse trade than a larger write.
        hddSaveKind = "full";
        hddIdbCurrent = false;
      }
      machine.mountHdd(savedHdd);
      if (pendingFloppy) {
        machine.mountFloppy(pendingFloppy.bytes);
        setBayLoaded(floppyBay, pendingFloppy.name);
      }
      // Empty by default -- see the CD-ROM section above for why this
      // drive isn't pre-loaded the way the HDD is. `pendingCdrom.bytes` is
      // already a real Uint8Array by the time it lands here (both the file
      // input and "Insert FreeDOS CD..." construct one from the fetched/read
      // ArrayBuffer), which matters because mountCdrom() goes through
      // embind's convertJSArrayToNumberVector -- that reads `.length`, and
      // a bare ArrayBuffer only has `byteLength`, so passing one directly
      // converts to an empty vector and the drive comes up with no disc in
      // it, silently. Same wrapping the HDD path above already does.
      if (pendingCdrom) {
        machine.mountCdrom(pendingCdrom.bytes);
        setBayLoaded(cdromBay, pendingCdrom.name);
      }
    }
    poweredOn = true;
    powerLed.classList.add("power-on");
    lastT = null;
    requestAnimationFrame(frame);  // rendering only
    schedulePump();                // the machine's own clock -- see the main loop above
    refreshHddControls();
    refreshFkeyControls();
    updateFocusHint();  // e.g. the auto power-on at boot never focuses the screen itself
    let noticeDismissed = false;
    try { noticeDismissed = localStorage.getItem(BOOT_NOTICE_KEY) === "1"; } catch {}
    if (!noticeDismissed) bootNoticeEl.classList.add("visible");
    if (new URLSearchParams(location.search).get("test") === "1") {
      window.__test = {
        machine, sendKey, screenEl, mapKey,
        get audioState() { return audioCtx ? audioCtx.state : null; },
        get heldKeysSize() { return heldKeys.size; },
        // Flush C: to IndexedDB and resolve when the put finishes -- tests
        // that reload must await this, or a factory-delta / full-image save
        // races the navigation and the next boot falls back to factory.
        persistHdd: () => persistHddIfDirty(),
        // Always snapshot C: even when clean. Persistence specs can't rely
        // on FreeDOS having dirtied the image during a fast boot.
        forcePersistHdd: () => persistHddSnapshot(),
        whenHddSaved: () => hddPersistChain,
        // Kick off (and await) the chunked factory-base IndexedDB stash.
        // Not started during boot -- that path tripped the realtime smoke
        // longtask budget under suite load. Specs that assert a reload
        // never re-fetches freedos-hdd.img must await this first.
        whenFactoryStashed: () => {
          if (firmware && firmware.hdd) stashFactoryInIdb(firmware.hdd, currentFactoryFingerprint);
          return factoryIdbChain;
        },
      };
    }
  }

  // Mirrors C: to IndexedDB if (and only if) it's actually been written to
  // since the last mirror -- called both periodically while running (see
  // the setInterval below) and once more, unconditionally safe to call
  // again, at powerOff(). A real fixed disk never needs this at all: a
  // sector write is durable the instant it hits the platter, no separate
  // "save" step exists on real hardware. This only exists because this
  // emulator's own C: lives in a JS Uint8Array that's gone the moment the
  // tab is (mountHdd()'d fresh from IndexedDB/the factory image on every
  // powerOn() -- see there) -- so it has to be copied out to the one
  // place that actually survives that, on some real cadence, not just
  // once at a clean power-off nobody reliably triggers by hand.
  //
  // Returns a promise that settles when the IndexedDB put completes (or
  // immediately when there is nothing dirty). Callers that are about to
  // tear the page down (powerOff, a test about to reload) must await it:
  // a first-session full-image put is ~504MB and easily loses a bare
  // reload race, which is what left CI reading "factory FreeDOS" again.
  let hddPersistChain = Promise.resolve();
  function queueHddSave(record) {
    hddLabel = "saved state (changes from this session)";
    refreshHddControls();
    hddPersistChain = hddPersistChain.then(async () => {
      await saveHdd(record);
      hddIdbCurrent = true;
    });
    return hddPersistChain;
  }
  function persistHddIfDirty() {
    if (!machine || !machine.hddDirty()) return hddPersistChain;
    // Patch only the sectors this session actually wrote into the kept
    // mirror, instead of re-copying the whole 504MB image off the wasm
    // heap -- see wd1003.h's dirty_ranges() comment and the audio-worklet
    // comment above this file's speaker code. Falls back to a full copy
    // only when there's no same-size mirror yet (should not happen after
    // powerOn always seeds savedHdd).
    const patches = machine.hddDirtyPatches();
    if (savedHdd && savedHdd.length === kHddImageBytes) {
      for (const { offset, bytes } of patches) savedHdd.set(bytes, offset);
    } else {
      savedHdd = machine.hddImage();
      hddSaveKind = "full";
    }
    machine.clearHddDirty();
    hddIdbCurrent = false;
    return queueHddSave(savedHdd);
  }
  // Test helper: put the live C: image in IndexedDB even when hddDirty() is
  // false (a fast ?test=1&fast=1 boot often never dirties the factory image).
  function persistHddSnapshot() {
    if (!machine) return hddPersistChain;
    if (machine.hddDirty()) return persistHddIfDirty();
    if (savedHdd && savedHdd.length === kHddImageBytes && hddIdbCurrent) {
      hddLabel = "saved state (changes from this session)";
      refreshHddControls();
      return hddPersistChain;
    }
    if (!savedHdd || savedHdd.length !== kHddImageBytes) {
      savedHdd = machine.hddImage();
      hddSaveKind = hddSaveKind || "full";
    }
    hddIdbCurrent = false;
    return queueHddSave(savedHdd);
  }

  async function powerOff() {
    if (!poweredOn) return;
    await persistHddIfDirty();
    poweredOn = false;  // pump()/frame() see this on their next tick and stop rescheduling
    lastMachine = machine;  // freed at the next powerOn() -- see there
    machine = null;      // real hardware: RAM is gone the instant power is cut
    powerLed.classList.remove("power-on");
    floppyBay.querySelector('[data-role="led"]').classList.remove("on");
    cdromBay.querySelector('[data-role="led"]').classList.remove("on");
    hddLed.classList.remove("on");
    clearScreenToBlack();
    if (audioCtx) { audioCtx.suspend().catch(() => {}); }
    refreshHddControls();
    refreshFkeyControls();
    updateFocusHint();  // nothing to type into once powered off -- hide it
    hideBootNotice();
  }

  // ---- function/extended-key panel -- a real AT keyboard's F-keys and
  // extended block, for anyone without a physical key to press (a Mac
  // keyboard has no Insert/PrintScreen/ScrollLock/Pause key at all, and no
  // discrete forward-Delete on laptops). A real keyboard sends nothing to a
  // powered-off machine, so these only work while running.
  //
  // #bezel's own [data-key] (escBtn) rides the same click-tap logic below
  // for a different reason: it's not a key a Mac keyboard lacks, it's a key
  // the *browser* lacks a way to deliver at all while fullscreen -- the
  // Fullscreen API treats Esc as its own reserved exit gesture and never
  // dispatches it to the page (confirmed live), so a physical Esc press
  // can't reach the guest no matter what keyboard you have. Injecting the
  // scancode straight from a click sidesteps the native key event entirely.
  const fkeyButtons = Array.from(document.querySelectorAll('#fkeyRow [data-key], #extraKeyRow [data-key], #bezel [data-key]'));
  const ctrlAltDelBtn = document.getElementById('ctrlAltDelBtn');
  function refreshFkeyControls() {
    const enabled = poweredOn && !!machine;
    for (const b of fkeyButtons) b.disabled = !enabled;
    ctrlAltDelBtn.disabled = !enabled;
  }
  for (const btn of fkeyButtons) {
    btn.addEventListener('click', () => {
      const key = btn.dataset.key;
      sendKey(key, false);
      // A real key tap has a real make-then-release gap; a synchronous
      // back-to-back make+break can land inside the same JS turn as the
      // machine's own real-time-paced instruction loop (requestAnimationFrame),
      // which never gets a chance to run between them since JS is single-
      // threaded -- the guest can end up never seeing the make code before
      // the break overwrites it. 50ms mirrors a real, if fast, keystroke.
      setTimeout(() => sendKey(key, true), 50);
    });
  }
  ctrlAltDelBtn.addEventListener('click', () => {
    if (!machine) return;
    // The classic warm-boot combo: Ctrl make, Alt make, then Del make --
    // using the ORIGINAL non-extended Delete scancode (0x53, the numpad
    // Del/period key from the 84-key keyboard that predates the 101-key
    // extended block), which is what the historical Ctrl-Alt-Del check
    // (present in this machine's real Bochs-legacy BIOS, matching genuine
    // x86 BIOS convention) looks for -- NOT SET1.Delete, which is the
    // newer extended [0xE0, 0x53] forward-Delete key. Gaps matter: sending
    // the makes back to back clobbered everything but Del (IBM_PCAT_REVIEW
    // §31). Under FreeDOS+JEMMEX (V86) that BIOS INT 9 check does not
    // currently take effect, so after the authentic sequence we also pulse
    // CPU+chipset reset -- same durable outcome as the front-panel Reset
    // -- until the V86 keyboard path handles CAD on its own.
    injectScancodeSequence([
      0x1D,          // Ctrl make
      0x38,          // Alt make
      0x53,          // Del make (classic non-extended)
      0x53 | 0x80,   // Del break
      0x38 | 0x80,   // Alt break
      0x1D | 0x80,   // Ctrl break
    ], 50);
    setTimeout(() => { if (machine) machine.reset(); }, 350);
  });

  powerSwitch.checked = false;  // starts unchecked -- switched on programmatically the instant
                                 // firmware finishes loading (see below), not by the user's own click
  powerSwitch.disabled = true;  // enabled once firmware has actually finished fetching -- its own
                                 // disabled state is the "still loading" signal, no status text needed
  clearScreenToBlack();
  refreshFkeyControls();  // start disabled while machine is off
  powerSwitch.addEventListener("change", () => {
    if (powerSwitch.checked) powerOn();
    else void powerOff();
  });

  // Autosave C: every few seconds while running, not only at an explicit
  // power-off -- the machine now boots itself on page load (see the
  // firmware-fetch block below) and most visitors never think to flip the
  // switch off before just closing the tab or hitting reload, which used
  // to silently discard every write since the last clean power-off (this
  // was a real bug: a whole game install lost because nothing ever called
  // powerOff()). 5s is arbitrary -- frequent enough that a mid-session
  // close loses at most a few seconds of writes, infrequent enough that
  // idle sessions (hddDirty() false) do nothing.
  setInterval(() => { void persistHddIfDirty(); }, 5000);

  // Even with the autosave above, navigating away (closing the tab,
  // following a link, a browser-gesture back/forward navigation) can still
  // land in the few-seconds gap since the last tick. Ask first, the same
  // way a real "unsaved changes" prompt would, as a last defense.
  // (Known limitation, not fixable from here: some browsers' gesture-based
  // navigation -- e.g. a trackpad swipe -- can bypass beforeunload
  // entirely, which is exactly the scenario "Download image" above exists
  // for as a durable, browser-independent backup.)
  window.addEventListener("beforeunload", (e) => {
    if (poweredOn && machine && machine.hddDirty()) {
      e.preventDefault();
      e.returnValue = "";
    }
  });

  // ---- fetch firmware once, up front ------------------------------------
  // Not modeling anything physical -- purely the web delivery mechanism --
  // so there's no reason to gate it behind the power switch: fetch starts
  // immediately, and flipping power on is instant once it's done.
  //
  // The factory HDD image is deliberately NOT in this list -- a visitor
  // with C: already in IndexedDB (the overwhelmingly common case after the
  // first visit, and anyone who uploaded their own image) never touches
  // those bytes. ensureFactoryHdd() fetches on demand for the cold-start
  // and "Reset to factory FreeDOS" paths only. The CD-ROM's ~400MB
  // install/live CD stays out for the same reason -- see the CD-ROM drive
  // section above / "Insert FreeDOS CD...".
  (async () => {
    await loadEmulatorModule();
    const [Module, savedRecord, bios, vga] = await Promise.all([
      Pc486({}),
      loadSavedHddRecord(),
      fetch("roms/BIOS-bochs-legacy").then((r) => r.arrayBuffer()),
      fetch("roms/VGABIOS-lgpl-latest.bin").then((r) => r.arrayBuffer()),
    ]);
    firmware = { Module, bios, vga, hdd: null };
    if (isFactoryDeltaRecord(savedRecord)) {
      // Reconstruct C: from the cached factory image + the small dirty
      // patch list. Cache API hit → no network fetch of freedos-hdd.img.
      await ensureFactoryHdd();
      // A delta written by an older build. Replay it once against the shipped
      // image and keep the result whole, so this record shape disappears.
      savedHdd = applyFactoryPatches(new Uint8Array(firmware.hdd), savedRecord.patches);
      hddSaveKind = "full";
      hddLabel = "saved state (from a previous visit)";
      hddIdbCurrent = false;
      void queueHddSave(savedHdd);
    } else if (savedRecord) {
      savedHdd = savedRecord instanceof Uint8Array ? savedRecord : new Uint8Array(savedRecord);
      hddSaveKind = "full";
      hddLabel = "saved state (from a previous visit)";
      hddIdbCurrent = true;
    } else {
      await ensureFactoryHdd();
    }

    powerSwitch.disabled = false;
    refreshHddControls();
    // Boot straight to a running machine once firmware is ready, rather
    // than making the visitor find and click the power switch themselves.
    powerSwitch.checked = true;
    await powerOn();
  })().catch((err) => {
    // no on-page error surface -- the power switch simply never enables;
    // the real failure detail goes to the console for diagnosis.
    console.error(err);
  });
})();
