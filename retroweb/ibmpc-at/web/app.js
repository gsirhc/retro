"use strict";
(() => {
  // ---- page theme (Win95 / mid-90s Mosaic web / Modern / Dark Modern) ---
  // Shared with every other page on the site via the retro8080.theme
  // localStorage key -- a theme picked here or on the landing page carries
  // across. "moderndark" is Modern's layout with data-mode="dark" bolted on.
  const pageTheme = document.getElementById("pageTheme");
  const root = document.documentElement;
  const THEME_VALUES = ["win", "web94", "modern", "moderndark"];
  const applyTheme = (v) => {
    if (!THEME_VALUES.includes(v)) v = "win";
    if (v === "moderndark") { root.dataset.theme = "modern"; root.dataset.mode = "dark"; }
    else { root.dataset.theme = v; delete root.dataset.mode; }
    return v;
  };
  let storedTheme; try { storedTheme = localStorage.getItem("retro8080.theme"); } catch {}
  pageTheme.value = applyTheme(
    new URLSearchParams(location.search).get("theme") || storedTheme || root.dataset.theme || "win");

  // Automated-test-only CPU speed multiplier: `?test=1&fast=1`. A real visitor
  // has no control that reaches this -- it exists solely so the Playwright
  // suite (whose real cost is a genuine ~45s 8 MHz POST + FreeDOS boot, not
  // just a device-transfer wait) doesn't pay that in full on every test.
  // `?test=1` alone still runs the real, wall-clock-paced 8 MHz clock -- the
  // suite's shared boot() helper opts most tests into `fast=1` explicitly,
  // and a couple of smoke tests deliberately don't, to verify the real-speed
  // contract itself still holds. See CLAUDE.md "Current sanctioned
  // overrides" (automated-test CPU clock multiplier).
  const testParams = new URLSearchParams(location.search);
  const TEST_CPU_MULTIPLIER =
    testParams.get("test") === "1" && testParams.get("fast") === "1" ? 20 : 1;
  pageTheme.addEventListener("change", () => {
    applyTheme(pageTheme.value);
    try { localStorage.setItem("retro8080.theme", pageTheme.value); } catch {}
  });

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
  function injectScancodeSequence(codes) {
    let i = 0;
    (function step() {
      if (!machine || i >= codes.length) return;
      machine.injectScancode(codes[i++]);
      if (i < codes.length) setTimeout(step, 20);
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
  const screenEl = document.getElementById("screen");
  screenEl.addEventListener("keydown", (e) => { sendKey(e.code, false); e.preventDefault(); });
  screenEl.addEventListener("keyup", (e) => { sendKey(e.code, true); e.preventDefault(); });
  screenEl.addEventListener("click", () => screenEl.focus());

  // ---- fullscreen -----------------------------------------------------
  // Expands the bezel (CRT frame + vignette + power LED), not the bare
  // canvas -- see the CSS comment by .bezel:fullscreen for why. A pure
  // web-UI convenience with no real hardware to be faithful to.
  const bezelEl = document.getElementById("bezel");
  const fullscreenBtn = document.getElementById("fullscreenBtn");
  function isFullscreen() {
    return (document.fullscreenElement || document.webkitFullscreenElement) === bezelEl;
  }
  function enterFullscreen() {
    (bezelEl.requestFullscreen || bezelEl.webkitRequestFullscreen).call(bezelEl);
  }
  // One-time "your Esc key won't reach DOS" hint, shown *before* fullscreen
  // actually engages -- not shown again once seen, tracked the same way
  // the theme picker remembers its own choice (localStorage, wrapped in
  // try/catch: private browsing can throw on either call). The version
  // string is the cache-bust: bump it whenever the hint's content changes
  // meaningfully, and everyone who saw an older version sees it again,
  // since their stored value no longer matches.
  const FS_ESC_HINT_VERSION = "2";
  const fsEscHint = document.getElementById("fsEscHint");
  function fsEscHintSeen() {
    try { return localStorage.getItem("retro8080.fsEscHintSeen") === FS_ESC_HINT_VERSION; } catch { return false; }
  }
  // Closing the dialog always means "go fullscreen now, and don't ask
  // again" -- whether that's the "Got it" button or the browser's own
  // native Escape-cancels-a-dialog behavior (a plain page dialog, not the
  // fullscreen problem this hint is about; Escape closing it here is
  // completely normal). Wiring both to the dialog's own "close" event
  // instead of just the button's click covers either path identically.
  document.getElementById("fsEscHintOk").addEventListener("click", () => fsEscHint.close());
  fsEscHint.addEventListener("close", () => {
    try { localStorage.setItem("retro8080.fsEscHintSeen", FS_ESC_HINT_VERSION); } catch {}
    enterFullscreen();
    if (poweredOn) screenEl.focus();
  });
  function updateFullscreenBtn() {
    const label = isFullscreen() ? "Exit fullscreen" : "Fullscreen";
    fullscreenBtn.title = label;
    fullscreenBtn.setAttribute("aria-label", label);
    // Re-grab keyboard focus on the way both in and out -- fullscreen
    // transitions move focus to the bezel itself, and a real keyboard has
    // no such thing as "the front panel has focus" (see screenEl's own
    // click handler above). Skipped while the hint dialog is open so it
    // doesn't fight the dialog's own focused "Got it" button.
    if (poweredOn && !fsEscHint.open) screenEl.focus();
  }
  fullscreenBtn.addEventListener("click", () => {
    if (isFullscreen()) {
      (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    } else if (fsEscHintSeen()) {
      enterFullscreen();
    } else {
      // Show the hint first and wait for it to be dismissed -- see the
      // dialog's own "close" listener above for what happens next. Not
      // requestFullscreen()'d here: a first-time visitor should read this
      // before the screen jumps, not have it appear after the fact.
      fsEscHint.showModal();
    }
  });
  document.addEventListener("fullscreenchange", updateFullscreenBtn);
  document.addEventListener("webkitfullscreenchange", updateFullscreenBtn);
  // Known limitation, not fixable from here: browsers reserve the real Esc
  // key to exit fullscreen and never dispatch it to the page at all while
  // doing so (confirmed live -- DOS gets nothing, it's not merely "also"
  // exiting fullscreen). There's no way for page script to claim it back
  // from the Fullscreen API. The Esc button in the bezel's corner (see
  // #bezel [data-key] below) is the workaround: it injects the scancode
  // directly, bypassing the native key event this problem lives in.

  // ---- floppy drives ------------------------------------------------
  // A real floppy is a mechanical slot: you can insert or eject one
  // whether the machine is powered on or off (pendingFloppy, populated
  // here, is what a power-on remounts -- see the power section below).
  const bays = Array.from(document.querySelectorAll(".at-bay"));
  const driveDefaultLabel = (d) => (d === 0 ? "empty (1.2MB, 5.25″)" : "empty (360KB, 5.25″)");
  function setBayLoaded(bay, name) {
    bay.classList.add("loaded");
    const label = bay.querySelector('[data-role="label"]');
    label.textContent = name;
    label.classList.remove("empty");
    bay.querySelector('[data-role="eject"]').disabled = false;
  }
  function setBayEmpty(bay, drive) {
    bay.classList.remove("loaded");
    const label = bay.querySelector('[data-role="label"]');
    label.textContent = driveDefaultLabel(drive);
    label.classList.add("empty");
    bay.querySelector('[data-role="eject"]').disabled = true;
  }
  for (const bay of bays) {
    const drive = parseInt(bay.dataset.drive, 10);
    const fileInput = bay.querySelector('[data-role="file"]');
    const ejectBtn = bay.querySelector('[data-role="eject"]');
    const label = bay.querySelector('[data-role="label"]');
    fileInput.addEventListener("change", async () => {
      const f = fileInput.files[0];
      fileInput.value = "";
      if (!f) return;
      const bytes = new Uint8Array(await f.arrayBuffer());
      pendingFloppy[drive] = { name: f.name, bytes };
      if (machine) machine.mountFloppy(drive, bytes);
      setBayLoaded(bay, f.name);
    });
    ejectBtn.addEventListener("click", () => {
      // A real 88-DCDD-style swappable drive: if the session actually
      // wrote to this diskette, hand the modified image back before
      // ejecting it -- otherwise those writes only ever existed in this
      // browser tab's memory.
      if (machine && machine.floppyDirty(drive)) {
        const img = machine.floppyImage(drive);
        const blob = new Blob([img], { type: "application/octet-stream" });
        const a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = (label.textContent || "disk").replace(/[^\w.-]+/g, "_") + ".img";
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(() => URL.revokeObjectURL(a.href), 4000);
      }
      if (machine) machine.unmountFloppy(drive);
      pendingFloppy[drive] = null;
      setBayEmpty(bay, drive);
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
  // per animation frame (an earlier version of this code did that). That
  // approach turned out to be fundamentally fragile: even with perfectly
  // gapless scheduling math, it depends on every rAF frame handing the
  // audio thread its own freshly start()ed node exactly on time, and any
  // main-thread hiccup (a GC pause, a big array copy) leaves the
  // previously-scheduled node's audio simply running out with nothing
  // queued behind it -- dead silence until the next node starts, which
  // then jumps straight to a nonzero level. That gap-then-jump is an
  // audible click, and enough of them in a row is exactly the "scratchy"
  // artifact reported live (see IBM_PCAT_REVIEW.md). A worklet's process()
  // callback runs continuously on the real-time audio thread regardless of
  // what the main thread is doing; feeding it through a ring buffer means
  // a brief stall just holds the last sample level (silent, no discontinuity)
  // until the main thread catches up and posts more data, rather than
  // clicking. It also drops the whole nextPlayTime/resync bookkeeping the
  // old approach needed, since there's no scheduling clock to keep in sync.
  const speakerCheckbox = document.getElementById("speakerEnabled");
  speakerCheckbox.checked = false;
  let audioCtx = null, speakerNode = null, lastLevel = false;

  // The worklet module's source, registered from a Blob URL rather than a
  // separate fetched file -- keeps the whole speaker path in this one
  // script with nothing extra for the Makefile to stage.
  const kSpeakerWorkletSrc = `
    class PcSpeakerProcessor extends AudioWorkletProcessor {
      constructor() {
        super();
        // ~350ms at 48kHz -- generous headroom against main-thread jank
        // (GC pauses, the periodic HDD autosave's array copy) without
        // making genuine underrun-driven latency noticeable.
        this.ring = new Float32Array(16384);
        this.writeIdx = 0;
        this.readIdx = 0;
        this.available = 0;
        this.lastSample = 0;
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
          out[i] = this.lastSample;
        }
        return true;
      }
    }
    registerProcessor("pc-speaker-processor", PcSpeakerProcessor);
  `;

  async function ensureAudioStarted() {
    if (audioCtx) return;
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const blobUrl = URL.createObjectURL(new Blob([kSpeakerWorkletSrc], { type: "application/javascript" }));
    try {
      await audioCtx.audioWorklet.addModule(blobUrl);
    } finally {
      URL.revokeObjectURL(blobUrl);
    }
    speakerNode = new AudioWorkletNode(audioCtx, "pc-speaker-processor", { numberOfOutputs: 1, outputChannelCount: [1] });
    speakerNode.connect(audioCtx.destination);
  }
  speakerCheckbox.addEventListener("change", () => {
    if (speakerCheckbox.checked) ensureAudioStarted();
  });

  // Converts this frame's real (cpu_cycle, level) edge trace --
  // PcSpeaker::drain_edges() via speakerEdges() -- into a sample array and
  // posts it to the worklet's ring buffer. No scheduling clock to maintain
  // here: the worklet plays whatever it's been sent, in order, at its own
  // pace, entirely decoupled from this function's own timing.
  function pumpAudio(frameStartCycle, cyclesThisFrame, dtSeconds) {
    const edges = machine.speakerEdges();  // always drain -- even if muted, so the log can't grow unbounded
    if (!audioCtx || !speakerNode || !speakerCheckbox.checked || cyclesThisFrame <= 0) return;
    const sampleRate = audioCtx.sampleRate;
    // Real elapsed wall-clock time for this frame, not cyclesThisFrame/8MHz --
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
      const v = level ? 0.25 : -0.25;
      for (; sampleIdx < edgeSample; sampleIdx++) data[sampleIdx] = v;
      level = levels[i] !== 0;
    }
    const vTail = level ? 0.25 : -0.25;
    for (; sampleIdx < sampleCount; sampleIdx++) data[sampleIdx] = vTail;
    lastLevel = level;

    speakerNode.port.postMessage(data, [data.buffer]);
  }

  // ---- main loop ---------------------------------------------------
  const ctx = screenEl.getContext("2d");
  const hddLed = document.getElementById("hddLed");
  let cycleCredit = 0, lastT = null;

  function clearScreenToBlack() {
    screenEl.width = kTextRenderWidth;
    screenEl.height = kTextRenderHeight;
    screenEl.style.aspectRatio = kTextRenderWidth + " / " + kTextRenderHeight;
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, screenEl.width, screenEl.height);
  }
  const kTextRenderWidth = 640, kTextRenderHeight = 350;  // matches ega_render.h's text-mode default

  function frame(t) {
    if (!poweredOn || !machine) return;  // power switched off mid-loop -- stop, don't reschedule
    if (lastT === null) lastT = t;
    let dtSeconds = (t - lastT) / 1000;
    lastT = t;
    dtSeconds = Math.min(dtSeconds, 0.25);  // clamp a backgrounded-tab gap -- no runaway catch-up burst

    // Real, fixed 8 MHz -- never sped up for a real visitor, per CLAUDE.md.
    // TEST_CPU_MULTIPLIER is 1 outside `?test=1&fast=1`; see its own comment.
    cycleCredit += dtSeconds * 8000000 * TEST_CPU_MULTIPLIER;
    const cyclesThisFrame = Math.floor(cycleCredit);
    cycleCredit -= cyclesThisFrame;
    const frameStartCycle = machine.totalCycles();
    if (cyclesThisFrame > 0) machine.runCycles(cyclesThisFrame);

    pumpAudio(frameStartCycle, cyclesThisFrame, dtSeconds);

    const blinkOn = Math.floor(t / 266) % 2 === 0;  // ~1.9Hz block-cursor blink
    const rgba = machine.renderFrame(blinkOn);
    // Resolution varies by mode (640x350 text, 320x200 CGA-compatible
    // graphics -- see ega_render.h) -- resize the canvas's own pixel
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

    for (const bay of bays) {
      const drive = parseInt(bay.dataset.drive, 10);
      const led = bay.querySelector('[data-role="led"]');
      led.classList.toggle("on", machine.floppyPresent(drive) && machine.floppyMotorOn(drive));
    }
    hddLed.classList.toggle("on", machine.hddBusy());

    requestAnimationFrame(frame);
  }

  // ---- hard disk persistence (IndexedDB) ---------------------------------
  // A real fixed disk keeps its contents when the machine is off; this
  // emulator's own Machine is fully discarded on power-off (see the power
  // switch section below), so without this C: would silently revert to
  // whatever it was mount()ed with every single power-on. One record in
  // one object store -- there's only ever one C: drive to remember.
  const HDD_DB_NAME = "ibmpcat-hdd", HDD_STORE = "hdd", HDD_KEY = "c-drive";
  function openHddDb() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(HDD_DB_NAME, 1);
      req.onupgradeneeded = () => req.result.createObjectStore(HDD_STORE);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }
  async function loadSavedHdd() {
    try {
      const db = await openHddDb();
      return await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readonly");
        const req = tx.objectStore(HDD_STORE).get(HDD_KEY);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
      });
    } catch (err) {
      console.error("could not read saved hard disk, using factory default:", err);
      return null;
    }
  }
  async function saveHdd(bytes) {
    try {
      const db = await openHddDb();
      await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readwrite");
        tx.objectStore(HDD_STORE).put(bytes, HDD_KEY);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
    } catch (err) {
      console.error("could not save hard disk changes:", err);
    }
  }
  async function clearSavedHdd() {
    try {
      const db = await openHddDb();
      await new Promise((resolve, reject) => {
        const tx = db.transaction(HDD_STORE, "readwrite");
        tx.objectStore(HDD_STORE).delete(HDD_KEY);
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
    } catch (err) {
      console.error("could not clear saved hard disk:", err);
    }
  }

  // ---- power switch (off by default) -------------------------------------
  // A real AT: flipping power off cuts power to everything -- RAM (and so
  // every bit of running state) is gone, exactly like unplugging it, while
  // a diskette physically stays seated in its drive regardless. Modeled
  // the same way here: powering off discards the whole Machine instance;
  // powering back on builds a fresh one and re-mounts whatever floppy
  // images were still "in the drive" (remembered in JS, not the discarded
  // Machine) when power was cut. No reset button -- the genuine 5170 never
  // had a front-panel one (a later clone-era convention); the real
  // machine's only user-facing control here is this power switch (in
  // reality mounted on the case's side/rear, not the front bezel, but kept
  // here as a labelled web-UI concession).
  const powerSwitch = document.getElementById("powerSwitch");
  const powerLed = document.getElementById("powerLed");
  let poweredOn = false;
  let firmware = null;  // {Module, bios, vga, hdd} once fetched -- fetched once, reused every power-on
  const pendingFloppy = [null, null];  // {name, bytes} per drive -- "what's physically in the drive"

  // What C: actually mounts next power-on: a saved image from IndexedDB
  // (whatever it last held -- factory FreeDOS with changes, a blank drive
  // mid-install, or a real OS the visitor installed themselves) if one
  // exists, otherwise the pristine fetched factory image. `hddLabel`
  // exists purely to describe that choice in the status line below.
  let savedHdd = null;   // Uint8Array | null
  let hddLabel = "factory FreeDOS (default)";
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
    hddLabel = "factory FreeDOS (default) -- takes effect next power-on";
    refreshHddControls();
    clearSavedHdd();
  });
  hddBlankBtn.addEventListener("click", () => {
    if (!firmware) return;
    savedHdd = new Uint8Array(firmware.hdd.byteLength);  // all zero -- unformatted, like a drive fresh from the factory floor
    hddLabel = "blank drive, unformatted (FDISK/FORMAT and install your own OS) -- takes effect next power-on";
    refreshHddControls();
    saveHdd(savedHdd);
  });
  // A real file on the visitor's own disk, independent of this browser's
  // storage -- the same "save modified media" idea the floppy eject flow
  // already offers, just for C: (which isn't ejectable, so it needs its
  // own explicit control instead of piggybacking on a drive-swap gesture).
  hddDownloadBtn.addEventListener("click", () => {
    if (!firmware) return;
    // Whatever is *actually* current: the live, possibly-just-written
    // image if the machine is running, else whatever's staged for the
    // next power-on, else the pristine factory image.
    const bytes = (poweredOn && machine) ? machine.hddImage() : (savedHdd || new Uint8Array(firmware.hdd));
    const blob = new Blob([bytes], { type: "application/octet-stream" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "ibmpcat-hdd.img";
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
    // This system's WD1003 geometry (733 cyl/5 head/17 sec, see wd1003.cpp)
    // is fixed in CMOS, not derived from the image the way the floppy
    // controller now derives its own geometry from media size (see
    // IBM_PCAT_REVIEW.md §27) -- a real fixed disk doesn't change shape
    // depending on what's written to it. An image of the wrong size would
    // still fail safely (wd1003.cpp's own bounds check reports a genuine
    // IDNF error rather than silently doing nothing), but refusing it
    // up front gives a clearer reason than a mysterious disk error deep
    // into a boot.
    if (bytes.byteLength !== firmware.hdd.byteLength) {
      alert("That file is " + bytes.byteLength + " bytes; this machine's hard disk " +
            "must be exactly " + firmware.hdd.byteLength + " bytes (733 cyl / 5 head / " +
            "17 sec/track). Not mounted.");
      return;
    }
    savedHdd = bytes;
    hddLabel = "uploaded image (" + f.name + ") -- takes effect next power-on";
    refreshHddControls();
    saveHdd(savedHdd);
  });

  function remountPendingFloppies() {
    for (let d = 0; d < 2; d++) {
      const p = pendingFloppy[d];
      if (!p) continue;
      machine.mountFloppy(d, p.bytes);
      setBayLoaded(bays[d], p.name);
    }
  }

  function powerOn() {
    if (poweredOn || !firmware) return;
    if (!machine) {
      machine = new firmware.Module.Machine();
      machine.loadRom(0x100000 - firmware.bios.byteLength, new Uint8Array(firmware.bios));
      machine.loadRom(0xC0000, new Uint8Array(firmware.vga));
      machine.mountHdd(savedHdd || new Uint8Array(firmware.hdd));
      remountPendingFloppies();
    }
    poweredOn = true;
    powerLed.classList.add("power-on");
    lastT = null;
    requestAnimationFrame(frame);
    refreshHddControls();
    refreshFkeyControls();
    if (new URLSearchParams(location.search).get("test") === "1") {
      window.__test = { machine, sendKey, screenEl };
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
  function persistHddIfDirty() {
    if (!machine || !machine.hddDirty()) return;
    savedHdd = machine.hddImage();
    machine.clearHddDirty();
    hddLabel = "saved state (changes from this session)";
    refreshHddControls();
    saveHdd(savedHdd);
  }

  function powerOff() {
    if (!poweredOn) return;
    persistHddIfDirty();
    poweredOn = false;  // frame() sees this on its next tick and stops rescheduling itself
    machine = null;      // real hardware: RAM is gone the instant power is cut
    powerLed.classList.remove("power-on");
    for (const bay of bays) bay.querySelector('[data-role="led"]').classList.remove("on");
    hddLed.classList.remove("on");
    clearScreenToBlack();
    if (audioCtx) { audioCtx.suspend().catch(() => {}); }
    refreshHddControls();
    refreshFkeyControls();
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
    // newer extended [0xE0, 0x53] forward-Delete key. No new C++ needed:
    // the real BIOS's own keyboard ISR already implements the warm-boot
    // check, exactly like genuine hardware. All 6 bytes go through
    // injectScancodeSequence() so each one gets its own real gap -- sending
    // even the 3 makes back to back clobbered everything but the last
    // (Del), so the BIOS only ever saw a lone Del with no Ctrl/Alt held
    // and never recognized the combo. See IBM_PCAT_REVIEW.md.
    injectScancodeSequence([
      0x1D,          // Ctrl make
      0x38,          // Alt make
      0x53,          // Del make (classic non-extended)
      0x53 | 0x80,   // Del break
      0x38 | 0x80,   // Alt break
      0x1D | 0x80,   // Ctrl break
    ]);
  });

  powerSwitch.checked = false;  // starts unchecked -- switched on programmatically the instant
                                 // firmware finishes loading (see below), not by the user's own click
  powerSwitch.disabled = true;  // enabled once firmware has actually finished fetching -- its own
                                 // disabled state is the "still loading" signal, no status text needed
  clearScreenToBlack();
  refreshFkeyControls();  // start disabled while machine is off
  powerSwitch.addEventListener("change", () => { if (powerSwitch.checked) powerOn(); else powerOff(); });

  // Autosave C: every few seconds while running, not only at an explicit
  // power-off -- the machine now boots itself on page load (see the
  // firmware-fetch block below) and most visitors never think to flip the
  // switch off before just closing the tab or hitting reload, which used
  // to silently discard every write since the last clean power-off (this
  // was a real bug: a whole game install lost because nothing ever called
  // powerOff()). 5s is arbitrary -- frequent enough that a mid-session
  // close loses at most a few seconds of writes, infrequent enough that
  // idle sessions (hddDirty() false) do nothing.
  setInterval(persistHddIfDirty, 5000);

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

  // ---- fetch firmware + the shipped HDD image once, up front ------------
  // Not modeling anything physical -- purely the web delivery mechanism --
  // so there's no reason to gate it behind the power switch: fetch starts
  // immediately, and flipping power on is instant once it's done.
  (async () => {
    const [Module, savedHddResult, bios, vga, hdd] = await Promise.all([
      IbmPcAt({}),
      loadSavedHdd(),
      fetch("roms/BIOS-bochs-legacy").then((r) => r.arrayBuffer()),
      fetch("roms/VGABIOS-lgpl-latest.bin").then((r) => r.arrayBuffer()),
      fetch("disks/freedos-hdd.img").then((r) => r.arrayBuffer()),
    ]);
    firmware = { Module, bios, vga, hdd };
    if (savedHddResult) {
      savedHdd = savedHddResult;
      hddLabel = "saved state (from a previous visit)";
    }
    powerSwitch.disabled = false;
    refreshHddControls();
    // Boot straight to a running machine once firmware is ready, rather
    // than making the visitor find and click the power switch themselves.
    powerSwitch.checked = true;
    powerOn();
  })().catch((err) => {
    // no on-page error surface -- the power switch simply never enables;
    // the real failure detail goes to the console for diagnosis.
    console.error(err);
  });
})();
