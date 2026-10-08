"use strict";
(() => {
  // ---- page theme ----
  // shared retro8080.theme localStorage key
  initThemePicker();

  // "Last built" is the wasm's mtime from the server
  (async () => {
    const el = document.getElementById("buildDate");
    for (const url of ["ibmpcat.wasm", "ibmpcat.js", "app.js"]) {
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

  // Test-only CPU multiplier (?test=1&fast=1). Bare ?test=1 stays real speed.
  const testParams = new URLSearchParams(location.search);
  const TEST_CPU_MULTIPLIER =
    testParams.get("test") === "1" && testParams.get("fast") === "1" ? 20 : 1;

  // ---- keyboard: key -> AT Set 1 scan code ----
  // number = one byte, array = 0xE0-prefixed. Break = make | 0x80 on the last byte.
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
    // Fixed sequences, no simple break-bit form. Pause has no break code (Set 1).
    PrintScreen: { make: [0xE0, 0x2A, 0xE0, 0x37], break: [0xE0, 0xB7, 0xE0, 0xAA] },
    Pause: { make: [0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5], break: [] },
  };

  let machine = null;

  // The 8042 has a single output byte, so each byte of a multi-byte sequence
  // needs a gap or the guest never reads the previous one.
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
      // Print Screen / Pause: fixed sequences
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

  // poweredOn is declared later, so pass it as a predicate
  const isRunning = () => poweredOn;
  const updateFocusHint = initFocusHint(screenEl, isRunning);
  // escBtn rides the [data-key] handling below
  initFullscreen({
    bezelEl: document.getElementById("bezel"),
    screenEl,
    fullscreenBtn: document.getElementById("fullscreenBtn"),
    fsEscHint: document.getElementById("fsEscHint"),
    fsEscHintOkBtn: document.getElementById("fsEscHintOk"),
    isRunning,
  });

  // Nestable load overlay. Two rAFs let the spinner paint before a sync mount.
  const loadOverlayEl = document.getElementById("loadOverlay");
  const loadOverlayLabel = document.getElementById("loadOverlayLabel");
  let loadBusyDepth = 0;
  function beginLoad(msg) {
    loadBusyDepth++;
    if (msg) loadOverlayLabel.textContent = msg;
    loadOverlayEl.classList.add("visible");
    loadOverlayEl.setAttribute("aria-hidden", "false");
  }
  function endLoad() {
    loadBusyDepth = Math.max(0, loadBusyDepth - 1);
    if (loadBusyDepth === 0) {
      loadOverlayEl.classList.remove("visible");
      loadOverlayEl.setAttribute("aria-hidden", "true");
    }
  }
  function paintLoadOverlay() {
    return new Promise((resolve) => {
      requestAnimationFrame(() => requestAnimationFrame(resolve));
    });
  }
  async function withLoad(msg, fn) {
    beginLoad(msg);
    try {
      await paintLoadOverlay();
      return await fn();
    } finally {
      endLoad();
    }
  }

  // ---- floppy drives ----
  // insert/eject works powered off too; pendingFloppy is remounted on power-on
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
      await withLoad("Loading floppy\u2026", async () => {
        const bytes = new Uint8Array(await f.arrayBuffer());
        pendingFloppy[drive] = { name: f.name, bytes };
        if (machine) machine.mountFloppy(drive, bytes);
        setBayLoaded(bay, f.name);
      });
    });
    ejectBtn.addEventListener("click", () => {
      // hand back modified images before ejecting
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

  // ---- PC speaker (muted every page load) ----
  // AudioWorklet ring buffer: a main-thread stall holds the last sample instead of
  // leaving a gap that clicks.
  const speakerCheckbox = document.getElementById("speakerEnabled");
  speakerCheckbox.checked = false;
  let audioCtx = null, speakerNode = null, lastLevel = false;

  // worklet source loaded from a Blob URL to keep it in this script
  const kSpeakerWorkletSrc = `
    class PcSpeakerProcessor extends AudioWorkletProcessor {
      constructor() {
        super();
        // ~350ms at 48kHz of jank headroom
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
              // overflow: drop oldest, like an unread FIFO
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
          // underrun: hold last sample to avoid a click
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

  // Turns this frame's (cpu_cycle, level) edges into samples for the worklet ring.
  function pumpAudio(frameStartCycle, cyclesThisFrame, dtSeconds) {
    const edges = machine.speakerEdges();  // always drain, even if muted
    if (!audioCtx || !speakerNode || !speakerCheckbox.checked || cyclesThisFrame <= 0) return;
    const sampleRate = audioCtx.sampleRate;
    // real dtSeconds, not cycles/8MHz, so the fast-test multiplier doesn't overproduce samples
    const sampleCount = Math.max(1, Math.round(dtSeconds * sampleRate));
    const data = new Float32Array(sampleCount);

    let level = lastLevel, sampleIdx = 0;
    const cycles = edges.cycles, levels = edges.levels;
    // effective cycles/sec this frame
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

  // ---- main loop ----
  const ctx = screenEl.getContext("2d");
  const hddLed = document.getElementById("hddLed");
  let cycleCredit = 0, lastT = null;

  function clearScreenToBlack() {
    setFrameSize(kTextRenderWidth, kTextRenderHeight);
    frameCtx.fillStyle = "#000";
    frameCtx.fillRect(0, 0, frameCanvas.width, frameCanvas.height);
    presentFrame();
  }
  const kTextRenderWidth = 640, kTextRenderHeight = 350;  // ega_render.h text-mode default

  // The guest frame lands in frameCanvas at native resolution, is scaled by a whole factor per
  // axis (nearest) into #screen, then smooth-scaled to the CSS box, so guest pixels stay uniform.
  // The 5154 Enhanced Color Display is a 4:3 tube in every mode, so 640x350 and 320x200 stretch tall.
  const frameCanvas = document.createElement("canvas");
  const frameCtx = frameCanvas.getContext("2d");
  const kMaxScreenScale = 4;  // past 4x the final smoothing pass is invisible
  let screenScaleX = 0, screenScaleY = 0;

  function setFrameSize(w, h) {
    if (frameCanvas.width === w && frameCanvas.height === h && screenScaleX) return;
    frameCanvas.width = w;
    frameCanvas.height = h;
    screenScaleX = screenScaleY = 0;
    fitScreen();
  }

  function fitScreen() {
    const dpr = window.devicePixelRatio || 1;
    const axis = (box, n) => Math.min(kMaxScreenScale, Math.max(1, Math.ceil(box * dpr / n - 0.01)));
    const sx = axis(screenEl.clientWidth, frameCanvas.width);
    const sy = axis(screenEl.clientHeight, frameCanvas.height);
    if (sx === screenScaleX && sy === screenScaleY) return;
    screenScaleX = sx;
    screenScaleY = sy;
    screenEl.width = frameCanvas.width * sx;
    screenEl.height = frameCanvas.height * sy;
    ctx.imageSmoothingEnabled = false;
    presentFrame();
  }

  function presentFrame() {
    ctx.drawImage(frameCanvas, 0, 0, screenEl.width, screenEl.height);
  }

  const screenResizeObserver = new ResizeObserver(fitScreen);
  try {
    screenResizeObserver.observe(screenEl, { box: "device-pixel-content-box" });
  } catch {
    screenResizeObserver.observe(screenEl);
    // Without device-pixel-content-box a monitor move changes no CSS size, so watch the ratio.
    (function watchDpr() {
      matchMedia("(resolution: " + (window.devicePixelRatio || 1) + "dppx)")
        .addEventListener("change", () => { fitScreen(); watchDpr(); }, { once: true });
    })();
  }

  function frame(t) {
    if (!poweredOn || !machine) return;  // powered off mid-loop, stop rescheduling
    if (lastT === null) lastT = t;
    let dtSeconds = (t - lastT) / 1000;
    lastT = t;
    dtSeconds = Math.min(dtSeconds, 0.25);  // clamp backgrounded-tab gap

    // real 8 MHz; TEST_CPU_MULTIPLIER is 1 outside ?test=1&fast=1
    cycleCredit += dtSeconds * 8000000 * TEST_CPU_MULTIPLIER;
    let cyclesThisFrame = Math.floor(cycleCredit);
    cycleCredit -= cyclesThisFrame;
    const frameStartCycle = machine.totalCycles();
    if (TEST_CPU_MULTIPLIER === 1) {
      if (cyclesThisFrame > 0) machine.runCycles(cyclesThisFrame);
    } else {
      // fast test mode: stop at a wall-clock budget and drop the rest, or input starves behind 250ms frames
      const deadline = performance.now() + 12;
      while (cyclesThisFrame > 0 && performance.now() < deadline) {
        const n = Math.min(cyclesThisFrame, 200000);
        machine.runCycles(n);
        cyclesThisFrame -= n;
      }
      cycleCredit = 0;
    }

    pumpAudio(frameStartCycle, machine.totalCycles() - frameStartCycle, dtSeconds);

    const rgba = machine.renderFrame();
    // Resolution varies by mode (640x350 text, 320x200 CGA graphics, ega_render.h); the 4:3 box stays put.
    const frameW = machine.renderWidth(), frameH = machine.renderHeight();
    setFrameSize(frameW, frameH);
    const img = frameCtx.createImageData(frameW, frameH);
    img.data.set(rgba);
    frameCtx.putImageData(img, 0, 0);
    presentFrame();

    for (const bay of bays) {
      const drive = parseInt(bay.dataset.drive, 10);
      const led = bay.querySelector('[data-role="led"]');
      led.classList.toggle("on", machine.floppyPresent(drive) && machine.floppyMotorOn(drive));
    }
    hddLed.classList.toggle("on", machine.hddBusy());

    requestAnimationFrame(frame);
  }

  // ---- hard disk persistence (IndexedDB) ----
  // the Machine is discarded on power-off, so C: is stored here (one record)
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

  // ---- power switch ----
  // power-off discards the Machine; power-on remounts floppies from pendingFloppy.
  // No reset button on the 5170.
  const powerSwitch = document.getElementById("powerSwitch");
  const powerLed = document.getElementById("powerLed");
  let poweredOn = false;
  let firmware = null;  // {Module, bios, videoBios, hdd}, fetched once
  const pendingFloppy = [null, null];  // {name, bytes} per drive

  // next power-on mounts the saved IndexedDB image if any, else the factory image
  let savedHdd = null;   // Uint8Array | null
  let hddLabel = "factory FreeDOS (default)";
  const hddStatus = document.getElementById("hddStatus");
  const hddResetBtn = document.getElementById("hddResetBtn");
  const hddBlankBtn = document.getElementById("hddBlankBtn");
  const hddDownloadBtn = document.getElementById("hddDownloadBtn");
  const hddUploadInput = document.getElementById("hddUploadInput");
  function refreshHddControls() {
    hddStatus.textContent = "Using: " + hddLabel;
    // C: only changes at the next power-on
    hddResetBtn.disabled = !firmware || poweredOn;
    hddBlankBtn.disabled = !firmware || poweredOn;
    hddDownloadBtn.disabled = !firmware;  // read-only, works while running
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
    savedHdd = new Uint8Array(firmware.hdd.byteLength);  // all zero, unformatted
    hddLabel = "blank drive, unformatted (FDISK/FORMAT and install your own OS) -- takes effect next power-on";
    refreshHddControls();
    saveHdd(savedHdd);
  });
  // download C: as a file
  hddDownloadBtn.addEventListener("click", () => {
    if (!firmware) return;
    // live image if running, else staged, else factory
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
    await withLoad("Loading hard disk\u2026", async () => {
      const bytes = new Uint8Array(await f.arrayBuffer());
      // WD1003 geometry (733/5/17) is fixed in CMOS, so reject wrong-size images
      if (bytes.byteLength !== firmware.hdd.byteLength) {
        alert("That file is " + bytes.byteLength + " bytes; this machine's hard disk " +
              "must be exactly " + firmware.hdd.byteLength + " bytes (733 cyl / 5 head / " +
              "17 sec/track). Not mounted.");
        return;
      }
      savedHdd = bytes;
      hddLabel = "uploaded image (" + f.name + ") -- takes effect next power-on";
      refreshHddControls();
      await saveHdd(savedHdd);
    });
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
      machine.loadRom(0xC0000, new Uint8Array(firmware.videoBios));
      machine.mountHdd(savedHdd || new Uint8Array(firmware.hdd));
      remountPendingFloppies();
    }
    poweredOn = true;
    powerLed.classList.add("power-on");
    lastT = null;
    requestAnimationFrame(frame);
    refreshHddControls();
    refreshFkeyControls();
    updateFocusHint();
    if (new URLSearchParams(location.search).get("test") === "1") {
      window.__test = {
        machine, sendKey, screenEl, frameCanvas,
        get loadOverlayVisible() { return loadOverlayEl.classList.contains("visible"); },
        get loadOverlayText() { return loadOverlayLabel.textContent; },
        beginLoad, endLoad,
      };
    }
  }

  // Mirrors C: to IndexedDB if written since the last mirror.
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
    poweredOn = false;
    machine = null;      // RAM is gone on power cut
    powerLed.classList.remove("power-on");
    for (const bay of bays) bay.querySelector('[data-role="led"]').classList.remove("on");
    hddLed.classList.remove("on");
    clearScreenToBlack();
    if (audioCtx) { audioCtx.suspend().catch(() => {}); }
    refreshHddControls();
    refreshFkeyControls();
    updateFocusHint();
  }

  // ---- function/extended-key panel ----
  // For keys a Mac keyboard lacks. escBtn is here because fullscreen swallows Esc.
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
      // make and break need a gap or the guest misses the make
      setTimeout(() => sendKey(key, true), 50);
    });
  }
  ctrlAltDelBtn.addEventListener('click', () => {
    if (!machine) return;
    // Ctrl-Alt-Del warm boot: BIOS checks the non-extended Del (0x53), not SET1.Delete.
    // Each byte is spaced or only the last survives.
    injectScancodeSequence([
      0x1D,          // Ctrl make
      0x38,          // Alt make
      0x53,          // Del make (classic non-extended)
      0x53 | 0x80,   // Del break
      0x38 | 0x80,   // Alt break
      0x1D | 0x80,   // Ctrl break
    ]);
  });

  powerSwitch.checked = false;
  powerSwitch.disabled = true;
  clearScreenToBlack();
  refreshFkeyControls();  // start disabled while machine is off
  powerSwitch.addEventListener("change", () => { if (powerSwitch.checked) powerOn(); else powerOff(); });

  // autosave so closing the tab doesn't lose writes
  setInterval(persistHddIfDirty, 5000);

  // last-ditch prompt; gesture navigation can bypass beforeunload
  window.addEventListener("beforeunload", (e) => {
    if (poweredOn && machine && machine.hddDirty()) {
      e.preventDefault();
      e.returnValue = "";
    }
  });

  // ---- fetch firmware + HDD image ----
  // web delivery only, not gated by the power switch
  (async () => {
    beginLoad("Loading\u2026");
    try {
      await paintLoadOverlay();
      const [Module, savedHddResult, bios, videoBios, hdd] = await Promise.all([
        IbmPcAt({}),
        loadSavedHdd(),
        fetch("roms/BIOS-bochs-legacy").then((r) => r.arrayBuffer()),
        fetch("roms/egabios.bin").then((r) => r.arrayBuffer()),
        fetch("disks/freedos-hdd.img").then((r) => r.arrayBuffer()),
      ]);
      firmware = { Module, bios, videoBios, hdd };
      if (savedHddResult) {
        savedHdd = savedHddResult;
        hddLabel = "saved state (from a previous visit)";
      }

      powerSwitch.disabled = false;
      refreshHddControls();
      // boot once firmware is ready
      powerSwitch.checked = true;
      powerOn();
    } finally {
      endLoad();
    }
  })().catch((err) => {
    // no on-page error; the power switch never enables
    console.error(err);
  });
})();
