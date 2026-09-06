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
  };

  let machine = null;
  function sendKey(code, isBreak) {
    const entry = SET1[code];
    if (entry === undefined || !machine) return;
    const bytes = Array.isArray(entry) ? entry.slice() : [entry];
    const last = bytes.length - 1;
    bytes[last] = isBreak ? (bytes[last] | 0x80) : bytes[last];
    for (const b of bytes) machine.injectScancode(b);
  }
  const screenEl = document.getElementById("screen");
  screenEl.addEventListener("keydown", (e) => { sendKey(e.code, false); e.preventDefault(); });
  screenEl.addEventListener("keyup", (e) => { sendKey(e.code, true); e.preventDefault(); });
  screenEl.addEventListener("click", () => screenEl.focus());

  // ---- floppy drives ------------------------------------------------
  const bays = Array.from(document.querySelectorAll(".drive-bay"));
  const driveDefaultLabel = (d) => (d === 0 ? "empty (1.2MB)" : "empty (360KB)");
  for (const bay of bays) {
    const drive = parseInt(bay.dataset.drive, 10);
    const fileInput = bay.querySelector('[data-role="file"]');
    const label = bay.querySelector('[data-role="label"]');
    const ejectBtn = bay.querySelector('[data-role="eject"]');
    fileInput.addEventListener("change", async () => {
      const f = fileInput.files[0];
      fileInput.value = "";
      if (!f || !machine) return;
      const bytes = new Uint8Array(await f.arrayBuffer());
      machine.mountFloppy(drive, bytes);
      label.textContent = f.name;
      label.classList.remove("empty");
      ejectBtn.disabled = false;
    });
    ejectBtn.addEventListener("click", () => {
      if (!machine) return;
      // A real 88-DCDD-style swappable drive: if the session actually
      // wrote to this diskette, hand the modified image back before
      // ejecting it -- otherwise those writes only ever existed in this
      // browser tab's memory.
      if (machine.floppyDirty(drive)) {
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
      machine.unmountFloppy(drive);
      label.textContent = driveDefaultLabel(drive);
      label.classList.add("empty");
      ejectBtn.disabled = true;
    });
  }

  // ---- PC speaker -- muted by default, every page load -----------------
  // Never restored from a saved preference: browsers block audio until a
  // fresh user gesture anyway, and the point of "off by default" is that
  // it stays that way until the visitor explicitly opts back in, not just
  // on first visit.
  const speakerCheckbox = document.getElementById("speakerEnabled");
  speakerCheckbox.checked = false;
  let audioCtx = null, nextPlayTime = 0, lastLevel = false;
  speakerCheckbox.addEventListener("change", () => {
    if (speakerCheckbox.checked && !audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      nextPlayTime = audioCtx.currentTime + 0.05;
    }
  });

  // Builds one audio buffer covering exactly the cycles this animation
  // frame just ran, from the real (cpu_cycle, level) edge trace --
  // PcSpeaker::drain_edges() via speakerEdges() -- rather than committing
  // to any waveform assumption of its own. Schedules gapless playback
  // starting at `nextPlayTime`; the CPU's own cycles are already paced to
  // real elapsed wall-clock time (see frame() below), so consecutive
  // frames' audio durations naturally stay in sync with no separate
  // cross-referencing needed.
  function pumpAudio(frameStartCycle, cyclesThisFrame) {
    const edges = machine.speakerEdges();  // always drain -- even if muted, so the log can't grow unbounded
    if (!audioCtx || !speakerCheckbox.checked || cyclesThisFrame <= 0) return;
    const sampleRate = audioCtx.sampleRate;
    const durationSeconds = cyclesThisFrame / 8000000;
    const sampleCount = Math.max(1, Math.round(durationSeconds * sampleRate));
    const buffer = audioCtx.createBuffer(1, sampleCount, sampleRate);
    const data = buffer.getChannelData(0);

    let level = lastLevel, sampleIdx = 0;
    const cycles = edges.cycles, levels = edges.levels;
    for (let i = 0; i < cycles.length; i++) {
      let edgeSample = Math.round(((cycles[i] - frameStartCycle) / 8000000) * sampleRate);
      if (edgeSample < 0) edgeSample = 0;
      if (edgeSample > sampleCount) edgeSample = sampleCount;
      const v = level ? 0.25 : -0.25;
      for (; sampleIdx < edgeSample; sampleIdx++) data[sampleIdx] = v;
      level = levels[i] !== 0;
    }
    const vTail = level ? 0.25 : -0.25;
    for (; sampleIdx < sampleCount; sampleIdx++) data[sampleIdx] = vTail;
    lastLevel = level;

    const src = audioCtx.createBufferSource();
    src.buffer = buffer;
    src.connect(audioCtx.destination);
    const now = audioCtx.currentTime;
    if (nextPlayTime < now) nextPlayTime = now + 0.02;  // fell behind (backgrounded tab) -- resync with slack
    src.start(nextPlayTime);
    nextPlayTime += durationSeconds;
  }

  // ---- main loop ---------------------------------------------------
  const ctx = screenEl.getContext("2d");
  const bootStatus = document.getElementById("bootStatus");
  const hddLed = document.getElementById("hddLed");
  let cycleCredit = 0, lastT = null;

  function frame(t) {
    if (lastT === null) lastT = t;
    let dtSeconds = (t - lastT) / 1000;
    lastT = t;
    dtSeconds = Math.min(dtSeconds, 0.25);  // clamp a backgrounded-tab gap -- no runaway catch-up burst

    cycleCredit += dtSeconds * 8000000;  // real, fixed 8 MHz -- never sped up, per CLAUDE.md
    const cyclesThisFrame = Math.floor(cycleCredit);
    cycleCredit -= cyclesThisFrame;
    const frameStartCycle = machine.totalCycles();
    if (cyclesThisFrame > 0) machine.runCycles(cyclesThisFrame);

    pumpAudio(frameStartCycle, cyclesThisFrame);

    const blinkOn = Math.floor(t / 266) % 2 === 0;  // ~1.9Hz block-cursor blink
    const rgba = machine.renderFrame(blinkOn);
    const img = ctx.createImageData(machine.renderWidth(), machine.renderHeight());
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

  // ---- boot: fetch firmware + the shipped HDD image, then go -----------
  (async () => {
    const Module = await IbmPcAt({});
    bootStatus.textContent = "Fetching BIOS/VGA BIOS/HDD image…";
    const [bios, vga, hdd] = await Promise.all([
      fetch("roms/BIOS-bochs-legacy").then((r) => r.arrayBuffer()),
      fetch("roms/VGABIOS-lgpl-latest.bin").then((r) => r.arrayBuffer()),
      fetch("disks/freedos-hdd.img").then((r) => r.arrayBuffer()),
    ]);
    machine = new Module.Machine();
    // BIOS-bochs-legacy is a 64KB image landing at the top of the address
    // space, exactly where the real 80286 reset vector (F000:FFF0 ->
    // physical 0xFFFF0) expects it.
    machine.loadRom(0x100000 - bios.byteLength, new Uint8Array(bios));
    machine.loadRom(0xC0000, new Uint8Array(vga));
    machine.mountHdd(new Uint8Array(hdd));
    bootStatus.textContent = "Booting…";
    setTimeout(() => { bootStatus.style.visibility = "hidden"; }, 3000);
    requestAnimationFrame(frame);

    // Test/debug hook, matching the ?test=1 / window.__test convention
    // already established in altair8800/web/app.js and cg-oac-6502/web/app.js
    // -- lets a Playwright suite (Phase 8) drive the machine and read its
    // state directly instead of only through pixel comparisons.
    if (new URLSearchParams(location.search).get("test") === "1") {
      window.__test = { machine, sendKey, screenEl };
    }
  })().catch((err) => {
    bootStatus.textContent = "Failed to start: " + err;
    console.error(err);
  });
})();
