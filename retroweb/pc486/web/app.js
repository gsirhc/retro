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

  // Opt-in diagnostic capture, not part of the machine: `?audiotrace`
  // records one row per audio post -- how long the chunk was in wall time
  // and in guest cycles, how many samples were handed over, how much FM and
  // digitized audio arrived, and the ring's depth and health at that moment.
  // A synthetic main-thread load does not reproduce what a real DOS game's
  // redraw does to this path (PC486_REVIEW.md section 31), so the point of
  // this is to replay a real session's pattern in the test harness. Needs
  // sound enabled, since nothing is posted otherwise.
  if (testParams.has("audiotrace")) {
    window.__audio = {
      start: (n) => { audioTraceMax = n || 40000; audioTrace = []; return "recording"; },
      stop: () => { const n = audioTrace ? audioTrace.length : 0; audioTraceMax = 0; return n; },
      // A few numbers to paste, rather than a file to send.
      summary: () => {
        if (!audioTrace || audioTrace.length === 0) return "no rows -- is sound enabled?";
        const q = (vals, f) => {
          const v = vals.slice().sort((a, b) => a - b);
          return v[Math.floor(f * (v.length - 1))];
        };
        const dt = audioTrace.map((r) => r.dtMs);
        const guestOverWall = audioTrace.map((r) => r.cycMs / r.dtMs);
        const audioOverWall = audioTrace.map((r) => r.fmMs / r.dtMs);
        // The worklet reports twice a second, so the first rows have no
        // ring figure of their own yet.
        const ring = audioTrace.map((r) => r.ringMs).filter((v) => v !== null);
        const f = (x) => Number(x.toFixed(3));
        return {
          rows: audioTrace.length,
          postMs: { p05: f(q(dt, 0.05)), p50: f(q(dt, 0.5)), p95: f(q(dt, 0.95)), max: f(q(dt, 1)) },
          guestPerWall: { p05: f(q(guestOverWall, 0.05)), p50: f(q(guestOverWall, 0.5)), p95: f(q(guestOverWall, 0.95)) },
          fmPerWall: { p05: f(q(audioOverWall, 0.05)), p50: f(q(audioOverWall, 0.5)), p95: f(q(audioOverWall, 0.95)) },
          ringMs: ring.length
            ? { min: f(q(ring, 0)), p50: f(q(ring, 0.5)), max: f(q(ring, 1)) }
            : null,
          starvedTotal: audioTrace[audioTrace.length - 1].starved,
        };
      },
      save: () => {
        const blob = new Blob([JSON.stringify(audioTrace || [])], { type: "application/json" });
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = "audiotrace.json";
        a.click();
        URL.revokeObjectURL(url);
      },
    };
  }

  // ---- UI state (panels, sound, mouse) --------------------------------
  // Small prefs in localStorage under one key. Defaults stay opt-in / open
  // for a first visit; once the visitor changes something, it comes back
  // next time. Keymap bindings live in their own key (below) -- denser and
  // already separately versioned.
  const UI_STATE_KEY = "retro8080.pc486.ui";
  const kUiStateDefaults = {
    sound: false,
    mouse: false,
    fkeysOpen: true,
    keymapOpen: true,
  };
  function loadUiState() {
    try {
      const raw = localStorage.getItem(UI_STATE_KEY);
      if (!raw) return Object.assign({}, kUiStateDefaults);
      const parsed = JSON.parse(raw);
      if (!parsed || typeof parsed !== "object") return Object.assign({}, kUiStateDefaults);
      return {
        sound: !!parsed.sound,
        mouse: !!parsed.mouse,
        fkeysOpen: parsed.fkeysOpen !== false,
        keymapOpen: parsed.keymapOpen !== false,
      };
    } catch {
      return Object.assign({}, kUiStateDefaults);
    }
  }
  function persistUiState() {
    try { localStorage.setItem(UI_STATE_KEY, JSON.stringify(uiState)); } catch {}
  }
  const uiState = loadUiState();

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
  // One queue for every sequence, so two keys' bytes never interleave: a
  // real keyboard sends one scan code whole before the next, and an E0
  // separated from its code reaches DOS as a different key.
  const scancodeQueue = [];
  let scancodePumping = false;
  function injectScancodeSequence(codes, gapMs = 20) {
    for (const code of codes) scancodeQueue.push({ code, gapMs });
    if (!scancodePumping) pumpScancodes();
  }
  function pumpScancodes() {
    if (!machine) scancodeQueue.length = 0;
    if (scancodeQueue.length === 0) { scancodePumping = false; return; }
    scancodePumping = true;
    const { code, gapMs } = scancodeQueue.shift();
    machine.injectScancode(code);
    if (scancodeQueue.length) setTimeout(pumpScancodes, gapMs);
    else scancodePumping = false;
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
  // Physical key -> guest codes attributed to that press. Guest keys are
  // refcounted so a shared chord modifier (WASD's Alt strafe on A and D)
  // stays down while any source that needs it is still held.
  const physicalHeld = new Map();
  const guestKeyRefs = new Map();
  function pressGuestKey(code) {
    const n = (guestKeyRefs.get(code) || 0) + 1;
    guestKeyRefs.set(code, n);
    if (n === 1) {
      heldKeys.add(code);
      sendKey(code, false);
    }
  }
  function releaseGuestKey(code) {
    const n = (guestKeyRefs.get(code) || 0) - 1;
    if (n <= 0) {
      guestKeyRefs.delete(code);
      if (heldKeys.has(code)) {
        heldKeys.delete(code);
        sendKey(code, true);
      }
    } else {
      guestKeyRefs.set(code, n);
    }
  }
  function releaseAllHeldKeys() {
    for (const code of heldKeys) sendKey(code, true);
    heldKeys.clear();
    physicalHeld.clear();
    guestKeyRefs.clear();
  }

  // ---- Key Mapper ----------------------------------------------------
  // Browser-edge remapping: one physical KeyboardEvent.code -> one or more
  // guest codes. The guest still sees genuine Set-1 scancodes; nothing in
  // the emulated keyboard changes. Doom 1.2 and its contemporaries predate
  // WASD (arrows + Ctrl/Alt/Shift), so the WASD preset translates the modern
  // habit here -- A/D send Alt+arrow so they strafe rather than turn.
  const KEYMAP_STORAGE_KEY = "retro8080.pc486.keymap";
  const kWasdPresetRows = [
    { from: "KeyW", to: ["ArrowUp"], preset: "wasd" },
    { from: "KeyS", to: ["ArrowDown"], preset: "wasd" },
    { from: "KeyA", to: ["AltLeft", "ArrowLeft"], preset: "wasd" },
    { from: "KeyD", to: ["AltLeft", "ArrowRight"], preset: "wasd" },
  ];
  const kShiftCtrlPresetRows = [
    { from: "ShiftLeft", to: ["ControlLeft"], preset: "shiftCtrl" },
  ];
  const kModifierCodes = new Set([
    "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
    "AltLeft", "AltRight", "MetaLeft", "MetaRight",
  ]);
  let keymapRows = [];
  let keymapNextId = 1;
  const keymapTableBody = document.getElementById("keymapTableBody");
  const keymapCaptureStatus = document.getElementById("keymapCaptureStatus");
  const keymapAllBtn = document.getElementById("keymapAllBtn");
  const keymapToggleAllBtn = document.getElementById("keymapToggleAll");

  function mapKey(code) {
    for (let i = 0; i < keymapRows.length; i++) {
      const row = keymapRows[i];
      if (row.enabled && row.from === code) return row.to.slice();
    }
    return [code];
  }

  function anyMappingEnabled() {
    return keymapRows.some((r) => r.enabled);
  }

  function labelKeyCode(code) {
    const labels = {
      ArrowUp: "Up", ArrowDown: "Down", ArrowLeft: "Left", ArrowRight: "Right",
      ShiftLeft: "Left Shift", ShiftRight: "Right Shift",
      ControlLeft: "Left Ctrl", ControlRight: "Right Ctrl",
      AltLeft: "Left Alt", AltRight: "Right Alt",
      MetaLeft: "Left Meta", MetaRight: "Right Meta",
      Escape: "Esc", Backspace: "Backspace", Enter: "Enter", Tab: "Tab",
      Space: "Space", CapsLock: "Caps Lock",
      Insert: "Insert", Delete: "Delete", Home: "Home", End: "End",
      PageUp: "Page Up", PageDown: "Page Down",
      PrintScreen: "Print Screen", ScrollLock: "Scroll Lock",
      Pause: "Pause", NumLock: "Num Lock",
    };
    if (labels[code]) return labels[code];
    if (code.startsWith("Key") && code.length === 4) return code.slice(3);
    if (code.startsWith("Digit") && code.length === 6) return code.slice(5);
    if (code.startsWith("F") && /^F\d{1,2}$/.test(code)) return code;
    return code;
  }
  function labelKeyChord(codes) {
    return codes.map(labelKeyCode).join("+");
  }

  function persistKeymap() {
    try {
      localStorage.setItem(KEYMAP_STORAGE_KEY, JSON.stringify(keymapRows.map((r) => ({
        id: r.id, from: r.from, to: r.to, enabled: r.enabled, preset: r.preset || undefined,
      }))));
    } catch {}
  }

  function loadKeymap() {
    try {
      const raw = localStorage.getItem(KEYMAP_STORAGE_KEY);
      if (!raw) return;
      const parsed = JSON.parse(raw);
      if (!Array.isArray(parsed)) return;
      const rows = [];
      for (const item of parsed) {
        if (!item || typeof item.from !== "string") continue;
        if (!Array.isArray(item.to) || item.to.length === 0) continue;
        if (!item.to.every((c) => typeof c === "string")) continue;
        const id = Number.isFinite(item.id) ? item.id : keymapNextId++;
        if (id >= keymapNextId) keymapNextId = id + 1;
        rows.push({
          id,
          from: item.from,
          to: item.to.slice(),
          enabled: item.enabled !== false,
          preset: item.preset === "wasd" || item.preset === "shiftCtrl" ? item.preset : undefined,
        });
      }
      keymapRows = rows;
    } catch {
      keymapRows = [];
    }
  }

  function isWasdPresetActive() {
    return kWasdPresetRows.every((p) => {
      const row = keymapRows.find((r) => r.from === p.from && r.preset === "wasd");
      return row && row.enabled && row.to.join("\0") === p.to.join("\0");
    });
  }

  function syncKeymapBezel() {
    const has = keymapRows.length > 0;
    const on = anyMappingEnabled();
    keymapAllBtn.disabled = !has;
    keymapAllBtn.setAttribute("aria-pressed", on ? "true" : "false");
  }
  function syncToggleAllBtn() {
    const empty = keymapRows.length === 0;
    keymapToggleAllBtn.disabled = empty;
    keymapToggleAllBtn.textContent = anyMappingEnabled() ? "Disable All" : "Enable All";
    syncKeymapBezel();
  }
  function setAllMappingsEnabled(on) {
    if (keymapRows.length === 0) return;
    for (const row of keymapRows) row.enabled = !!on;
    releaseAllHeldKeys();
    persistKeymap();
    // Flip checkboxes in place -- a full table rebuild on every bezel click
    // made KEYS feel different from the speaker / mouse toggles.
    keymapTableBody.querySelectorAll('input[type="checkbox"]').forEach((c) => {
      c.checked = !!on;
    });
    syncToggleAllBtn();
  }

  function appendMappingRow(opts) {
    const { fromLabel, toLabel, enabled, ariaLabel, onToggle, onDelete } = opts;
    const tr = document.createElement("tr");
    const tdFrom = document.createElement("td");
    tdFrom.textContent = fromLabel;
    const tdTo = document.createElement("td");
    tdTo.textContent = toLabel;
    const tdOn = document.createElement("td");
    const check = document.createElement("input");
    check.type = "checkbox";
    check.checked = enabled;
    check.setAttribute("aria-label", ariaLabel);
    check.addEventListener("change", () => onToggle(check.checked));
    tdOn.appendChild(check);
    const tdAct = document.createElement("td");
    tdAct.className = "keymap-actions";
    const del = document.createElement("button");
    del.type = "button";
    del.textContent = "Delete";
    del.addEventListener("click", onDelete);
    tdAct.appendChild(del);
    tr.append(tdFrom, tdTo, tdOn, tdAct);
    keymapTableBody.appendChild(tr);
  }

  function renderKeymapTable() {
    keymapTableBody.replaceChildren();
    if (keymapRows.length === 0) {
      const tr = document.createElement("tr");
      tr.className = "keymap-empty";
      const td = document.createElement("td");
      td.colSpan = 4;
      td.textContent = "No mappings — presets above, or Add mapping.";
      tr.appendChild(td);
      keymapTableBody.appendChild(tr);
      syncToggleAllBtn();
      return;
    }
    // WASD's four underlying rows render as one table line.
    const wasdRows = keymapRows.filter((r) => r.preset === "wasd");
    const otherRows = keymapRows.filter((r) => r.preset !== "wasd");
    if (wasdRows.length > 0) {
      appendMappingRow({
        fromLabel: "WASD",
        toLabel: "Arrows (A/D strafe)",
        enabled: wasdRows.every((r) => r.enabled),
        ariaLabel: "Enable WASD to Arrows",
        onToggle: (on) => {
          for (const row of wasdRows) row.enabled = on;
          releaseAllHeldKeys();
          persistKeymap();
          syncToggleAllBtn();
        },
        onDelete: () => {
          const ids = new Set(wasdRows.map((r) => r.id));
          keymapRows = keymapRows.filter((r) => !ids.has(r.id));
          releaseAllHeldKeys();
          persistKeymap();
          renderKeymapTable();
        },
      });
    }
    for (const row of otherRows) {
      appendMappingRow({
        fromLabel: labelKeyCode(row.from),
        toLabel: labelKeyChord(row.to),
        enabled: row.enabled,
        ariaLabel: `Enable mapping ${labelKeyCode(row.from)}`,
        onToggle: (on) => {
          row.enabled = on;
          releaseAllHeldKeys();
          persistKeymap();
          syncToggleAllBtn();
        },
        onDelete: () => {
          keymapRows = keymapRows.filter((r) => r.id !== row.id);
          releaseAllHeldKeys();
          persistKeymap();
          renderKeymapTable();
        },
      });
    }
    syncToggleAllBtn();
  }

  function upsertMapping(from, to, preset) {
    const existing = keymapRows.findIndex((r) => r.from === from);
    const row = {
      id: existing >= 0 ? keymapRows[existing].id : keymapNextId++,
      from,
      to: to.slice(),
      enabled: true,
      preset,
    };
    if (existing >= 0) keymapRows[existing] = row;
    else keymapRows.push(row);
  }

  function applyPresetRows(presetRows) {
    for (const p of presetRows) upsertMapping(p.from, p.to, p.preset);
    releaseAllHeldKeys();
    persistKeymap();
    renderKeymapTable();
  }

  function setWasdPresetEnabled(on) {
    if (on) {
      applyPresetRows(kWasdPresetRows);
      return;
    }
    let changed = false;
    for (const p of kWasdPresetRows) {
      const row = keymapRows.find((r) => r.from === p.from && r.preset === "wasd");
      if (row && row.enabled) { row.enabled = false; changed = true; }
    }
    if (changed) {
      releaseAllHeldKeys();
      persistKeymap();
      renderKeymapTable();
    } else {
      syncToggleAllBtn();
    }
  }

  function clearKeymap() {
    keymapRows = [];
    releaseAllHeldKeys();
    persistKeymap();
    renderKeymapTable();
  }

  // Capture flow for Add mapping: From (one key), then To (one key; modifiers
  // held with that key become a guest chord).
  let capturePhase = null; // null | "from" | "to"
  let captureFrom = null;
  let capturePendingModifier = null;
  function setCaptureStatus(text) {
    keymapCaptureStatus.textContent = text || "";
  }
  function cancelCapture() {
    capturePhase = null;
    captureFrom = null;
    capturePendingModifier = null;
    setCaptureStatus("");
    document.getElementById("keymapAddBtn").textContent = "Add mapping";
  }
  function chordFromEvent(e) {
    const chord = [];
    if (e.altKey && e.code !== "AltLeft" && e.code !== "AltRight") chord.push("AltLeft");
    if (e.ctrlKey && e.code !== "ControlLeft" && e.code !== "ControlRight") chord.push("ControlLeft");
    if (e.shiftKey && e.code !== "ShiftLeft" && e.code !== "ShiftRight") chord.push("ShiftLeft");
    chord.push(e.code);
    return chord;
  }
  function onCaptureKeyDown(e) {
    if (!capturePhase) return;
    e.preventDefault();
    e.stopPropagation();
    if (e.repeat) return;
    if (e.code === "Escape") {
      cancelCapture();
      return;
    }
    if (capturePhase === "from") {
      captureFrom = e.code;
      capturePhase = "to";
      capturePendingModifier = null;
      setCaptureStatus(`From ${labelKeyCode(captureFrom)} — now press the guest key (modifiers held become a chord). Esc cancels.`);
      return;
    }
    // Bare modifiers are valid targets (Left Shift → Ctrl). Defer until
    // keyup so Alt+Arrow can still form a chord on the non-modifier press.
    if (kModifierCodes.has(e.code)) {
      capturePendingModifier = e.code;
      return;
    }
    const to = chordFromEvent(e);
    capturePendingModifier = null;
    upsertMapping(captureFrom, to, undefined);
    releaseAllHeldKeys();
    persistKeymap();
    renderKeymapTable();
    cancelCapture();
  }
  function onCaptureKeyUp(e) {
    if (!capturePhase) return;
    e.preventDefault();
    e.stopPropagation();
    if (capturePhase === "to" && capturePendingModifier && e.code === capturePendingModifier) {
      const to = [capturePendingModifier];
      capturePendingModifier = null;
      upsertMapping(captureFrom, to, undefined);
      releaseAllHeldKeys();
      persistKeymap();
      renderKeymapTable();
      cancelCapture();
    }
  }
  document.addEventListener("keydown", onCaptureKeyDown, true);
  document.addEventListener("keyup", onCaptureKeyUp, true);

  document.getElementById("keymapAddBtn").addEventListener("click", () => {
    if (capturePhase) {
      cancelCapture();
      return;
    }
    capturePhase = "from";
    captureFrom = null;
    capturePendingModifier = null;
    document.getElementById("keymapAddBtn").textContent = "Cancel";
    setCaptureStatus("Press the physical key to remap. Esc cancels.");
  });
  document.getElementById("keymapPresetWasd").addEventListener("click", () => {
    cancelCapture();
    applyPresetRows(kWasdPresetRows);
  });
  document.getElementById("keymapPresetShiftCtrl").addEventListener("click", () => {
    cancelCapture();
    applyPresetRows(kShiftCtrlPresetRows);
  });
  keymapToggleAllBtn.addEventListener("click", () => {
    cancelCapture();
    setAllMappingsEnabled(!anyMappingEnabled());
  });

  loadKeymap();
  renderKeymapTable();

  function applyPanelOpen(toggleId, panelId, open) {
    const btn = document.getElementById(toggleId);
    const panel = document.getElementById(panelId);
    btn.setAttribute("aria-expanded", open ? "true" : "false");
    panel.classList.toggle("collapsed", !open);
  }
  function wirePanelCollapse(toggleId, panelId, stateKey) {
    const btn = document.getElementById(toggleId);
    applyPanelOpen(toggleId, panelId, uiState[stateKey]);
    btn.addEventListener("click", () => {
      const open = btn.getAttribute("aria-expanded") !== "true";
      applyPanelOpen(toggleId, panelId, open);
      uiState[stateKey] = open;
      persistUiState();
    });
  }
  wirePanelCollapse("fkeysToggle", "fkeysCard", "fkeysOpen");
  wirePanelCollapse("keymapToggle", "keymapCard", "keymapOpen");

  const screenEl = document.getElementById("screen");
  screenEl.addEventListener("keydown", (e) => {
    if (capturePhase) return;
    if (e.repeat) {
      e.preventDefault();
      return;
    }
    if (physicalHeld.has(e.code)) {
      e.preventDefault();
      return;
    }
    const codes = mapKey(e.code);
    physicalHeld.set(e.code, codes);
    for (const code of codes) pressGuestKey(code);
    e.preventDefault();
  });
  screenEl.addEventListener("keyup", (e) => {
    if (capturePhase) return;
    const codes = physicalHeld.get(e.code) || mapKey(e.code);
    physicalHeld.delete(e.code);
    for (let i = codes.length - 1; i >= 0; i--) releaseGuestKey(codes[i]);
    e.preventDefault();
  });
  screenEl.addEventListener("click", () => {
    screenEl.focus();
    if (mouseCaptureCheckbox.checked && document.pointerLockElement !== screenEl) screenEl.requestPointerLock();
    // The natural first gesture a visitor makes, and the one the "restored
    // checkmark may need one click" comment above the checkbox promises --
    // a context created (or left suspended by the browser) without this
    // never resumes on its own, so a sound preference restored from
    // localStorage would otherwise stay silent until the box is toggled.
    if (speakerCheckbox.checked) ensureAudioStarted();
  });
  window.addEventListener("blur", releaseAllHeldKeys);
  document.addEventListener("visibilitychange", () => { if (document.hidden) releaseAllHeldKeys(); });

  // ---- PS/2 mouse (8042 AUX port) -----------------------------------
  // Off by default for a first visit; remembered in UI state once chosen.
  // Pointer Lock still needs an explicit canvas click to engage.
  const mouseCaptureCheckbox = document.getElementById("mouseCaptureEnabled");
  mouseCaptureCheckbox.checked = uiState.mouse;
  mouseCaptureCheckbox.addEventListener("change", () => {
    uiState.mouse = mouseCaptureCheckbox.checked;
    persistUiState();
  });
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

  // Screen overlay while a large image is downloaded or read into memory.
  // Nestable: overlapping HDD + media loads keep it up until the last one
  // finishes. Two rAFs after show give the spinner a chance to paint before
  // a sync wasm mount freezes the main thread.
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
      await withLoad("Loading floppy\u2026", async () => {
        const bytes = new Uint8Array(await f.arrayBuffer());
        pendingFloppy = { name: f.name, bytes };
        if (machine) machine.mountFloppy(bytes);
        setBayLoaded(floppyBay, f.name);
      });
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
        await withLoad("Loading floppy\u2026", async () => {
          const res = await fetch("disks/ctmouse.img");
          if (!res.ok) throw new Error("HTTP " + res.status);
          const bytes = new Uint8Array(await res.arrayBuffer());
          pendingFloppy = { name: "ctmouse.img", bytes };
          if (machine) machine.mountFloppy(bytes);
          setBayLoaded(floppyBay, "ctmouse.img");
        });
        status.innerHTML =
          "CuteMouse is a freeware DOS mouse driver. To load it every boot: " +
          "<code>COPY CTMOUSE.EXE C:\\</code> then add " +
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
      const files = Array.from(fileInput.files);
      fileInput.value = "";
      if (!files.length) return;
      // A mixed-mode disc (data + CD-DA audio tracks) is a CUE sheet plus
      // its companion BIN -- the picker's `multiple` attribute lets both be
      // selected in one go. Anything else falls back to the plain single-ISO
      // path, unchanged.
      const cueFile = files.find((f) => /\.cue$/i.test(f.name));
      if (cueFile) {
        const binFile = files.find((f) => f !== cueFile);
        if (!binFile) {
          alert("Select the .cue file together with its .bin file.");
          return;
        }
        await withLoad("Loading CD-ROM\u2026", async () => {
          const cueText = await cueFile.text();
          const bin = new Uint8Array(await binFile.arrayBuffer());
          pendingCdrom = { name: cueFile.name, cueText, bin };
          if (machine) machine.mountCdromCue(cueText, bin);
          setBayLoaded(cdromBay, cueFile.name);
        });
        return;
      }
      const f = files[0];
      await withLoad("Loading CD-ROM\u2026", async () => {
        const bytes = new Uint8Array(await f.arrayBuffer());
        pendingCdrom = { name: f.name, bytes };
        if (machine) machine.mountCdrom(bytes);
        setBayLoaded(cdromBay, f.name);
      });
    });
    loadFreedosBtn.addEventListener("click", async () => {
      loadFreedosBtn.disabled = true;
      const originalText = loadFreedosBtn.textContent;
      loadFreedosBtn.textContent = "Loading\u2026";
      freedosStatus.textContent = "Fetching\u2026 (~400MB)";
      try {
        await withLoad("Loading CD-ROM\u2026", async () => {
          const res = await fetch("disks/freedos-cd.iso");
          if (!res.ok) throw new Error("HTTP " + res.status);
          const bytes = new Uint8Array(await res.arrayBuffer());
          pendingCdrom = { name: "FreeDOS install/live CD", bytes };
          if (machine) machine.mountCdrom(bytes);
          setBayLoaded(cdromBay, pendingCdrom.name);
        });
        freedosStatus.innerHTML =
          "In drive D:. At the prompt, type <code>D:</code> then <code>DIR</code> to browse. " +
          "This is FreeDOS's official install/live CD. " +
          "C: already boots without it; use this to install more packages or " +
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
  const sbVolume = document.getElementById("sbVolume");
  const sbVolumeReadout = document.getElementById("sbVolumeReadout");
  // A real wheel stays where it was left, so the position persists.
  // Enable Sound is remembered in UI state too; browsers still block
  // Autoplay until a gesture, so a restored checkmark may need one click
  // before audio actually starts.
  const SB_VOLUME_KEY = "retro8080.pc486SbVolume";
  speakerCheckbox.checked = uiState.sound;
  let audioCtx = null, speakerNode = null, lastLevel = false;
  // The SB16's backplate volume wheel: an analog pot after the card's output
  // amplifier (CT1740/CT1750 kept the thumbwheel earlier cards had), so it
  // sits outside everything the guest can see -- the CT1745's own Master and
  // FM attenuators are already modelled in pumpSbAudio's gains. The
  // motherboard speaker is a separate part on a real tower and does not pass
  // through the card, so speakerNode is deliberately not routed through this.
  let sbGainNode = null;
  // Square law, a reasonable stand-in for a pot's audio taper: 50 is unity
  // (kMixerUnityGain's modelled amplifier gain) and 100 is +12 dB.
  const kWheelMaxGain = 4.0;
  function wheelGain(pos) { const f = pos / 100; return f * f * kWheelMaxGain; }
  let sbNode = null;
  let audioStats = null;
  // Fractional sample carried between posts (see pumpSbAudio) and the ring
  // depth the pump aims to hold, a little under the worklet's own 50ms trim
  // ceiling so the two don't fight each other.
  let spkSampleCarry = 0;
  // Guest cycles per real second, averaged over ~0.2s of posts. The worklet
  // steps its play position by this, so it must be the rate actually being
  // achieved: nominal is right on the shipped page but wrong under the
  // fast-test multiplier, where the host cannot reach 20x. Averaging is safe
  // here in a way it was not on the main thread (PC486_REVIEW.md section 31):
  // the worklet closes a loop around it on the lead it actually observes, so
  // a slightly wrong rate self-corrects instead of draining the cushion.
  // Accumulated over a window rather than averaged per post: the mean of
  // per-post cycles/dt ratios sits above the true aggregate rate whenever
  // those ratios are spread out (and under load they are), and that bias is
  // larger than the correction the audio thread is allowed to apply.
  let guestHzEma = 0, guestCycAcc = 0, guestDtAcc = 0;
  // `?audiotrace` capture buffer -- see the window.__audio hook below. Null
  // unless a visitor started one, so the pump pays one null check for it.
  let audioTrace = null, audioTraceMax = 0;
  let sbCapture = null, sbCaptureCyc = null;

  // CT1745 mixer tone controls (SBPG chapter 4): registers 44h/45h (Treble
  // L/R) and 46h/47h (Bass L/R), 4 bits in the value's high nibble like the
  // other level registers, default 8<<4 -- 0 to 7 is -14 dB to 0 dB and 8 to
  // 15 is 0 dB to +14 dB, both in 2 dB steps, so 7 and 8 are both flat. This
  // sits in the mixer, upstream of the card's own output amplifier and the
  // backplate volume wheel (sbGainNode) -- unlike output_gain_*/fm_gain_*,
  // which the core already applies before samples reach here.
  let sbBassLeft = null, sbBassRight = null, sbTrebleLeft = null, sbTrebleRight = null;
  let lastTrebleLeftReg = -1, lastTrebleRightReg = -1, lastBassLeftReg = -1, lastBassRightReg = -1;
  // Shelf corner frequencies: SBPG documents the register range (±14 dB) but
  // not the real CT1745 tone circuit's corner frequencies anywhere we could
  // find -- these two numbers are an UNCITED ESTIMATE of a period analog
  // tone control, not a hardware-sourced value like the rest of this file.
  const kBassShelfHz = 100;
  const kTrebleShelfHz = 5000;
  function mixerLevelDb(reg) {
    const level = (reg >> 4) & 0x0f;
    return level <= 7 ? level * 2 - 14 : (level - 8) * 2;
  }

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
  // machine's old 504 MB image, EVERY 5 seconds whenever anything on C: was
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
    // Placement happens HERE, against this thread's own sample clock, not on
    // the main thread against performance.now(). The main thread hands over
    // the card's samples exactly as the emulator stamped them -- a guest CPU
    // cycle per sample -- and nothing else. That matters because the main
    // thread measures elapsed time in ~1ms windows while runCycles() blocks
    // it for up to 12ms at a stretch: when the guest is busy no post happens
    // during a chunk and several fire at once afterwards, so a
    // cycles-per-second figure derived there swings wildly, and every sample
    // position divided by it swung with it. The audio clock has no such
    // problem -- it advances one sample per sample, exactly -- so a cycle
    // stamp converts to an output position with nothing measured at all.
    class Sb16Processor extends AudioWorkletProcessor {
      constructor() {
        super();
        // Pending stamped samples per stream, as flat rings: a cycle stamp
        // and the sample it belongs to. Three streams because the card sums
        // FM, digitized audio, and CD-DA in the analog domain, each behind
        // its own CT1745 attenuator (see soundblaster.h) -- they arrive
        // separately and mix here.
        const kCap = 1 << 16;   // ~1.3s of FM at 49.7kHz
        this.cap = kCap;
        this.sb = { cyc: new Float64Array(kCap), l: new Int16Array(kCap), r: new Int16Array(kCap),
                    head: 0, tail: 0, lastL: 0, lastR: 0 };
        this.fm = { cyc: new Float64Array(kCap), l: new Int16Array(kCap), r: new Int16Array(kCap),
                    head: 0, tail: 0, lastL: 0, lastR: 0 };
        this.cd = { cyc: new Float64Array(kCap), l: new Int16Array(kCap), r: new Int16Array(kCap),
                    head: 0, tail: 0, lastL: 0, lastR: 0 };
        this.gain = { sbL: 1, sbR: 1, fmL: 1, fmR: 1, cdL: 1, cdR: 1 };
        // Guest cycles per real second. The emulator is paced to this by
        // construction (the pump grants exactly this much credit per real
        // second), so it is exact rather than measured -- which is the whole
        // point of doing the placement here.
        this.cpuHz = 66000000;
        this.playCycle = null;     // guest cycle the next output sample sits at
        this.rateTrim = 1;         // tiny correction, see below
        // How far ahead of the play position the newest stamp should sit.
        // Same job as a ring's depth: it absorbs a late batch without the
        // output having to stall, so it is also the longest main-thread stall
        // that passes unheard. 80ms costs 80ms of output latency and covers
        // the GC-pause and disk-persist scale of jank this page actually
        // sees; measured dropouts at 40ms were stalls longer than that.
        this.targetLeadSec = 0.08;
        // Above this the surplus is dropped rather than queued: a guest that
        // outruns real time (the fast-test multiplier, or a catch-up burst)
        // would otherwise push latency up without bound. The old ring did the
        // same thing by trimming to its target.
        this.maxLeadSec = 0.12;
        this.starved = 0;
        this.trimmed = 0;
        this.statFrames = 0;
        this.capture = null;
        this.captureMax = 0;
        this.port.onmessage = (e) => {
          const d = e.data;
          if (d.gain) this.gain = d.gain;
          if (d.cpuHz) this.cpuHz = d.cpuHz;
          if (d.sbCyc) this.push(this.sb, d.sbCyc, d.sbL, d.sbR);
          if (d.fmCyc) this.push(this.fm, d.fmCyc, d.fmL, d.fmR);
          if (d.cdCyc) this.push(this.cd, d.cdCyc, d.cdL, d.cdR);
          // Diagnostic capture of what actually reaches the device, for
          // web/tests/fmquality.spec.ts. Off unless asked for.
          if (d.startCapture) { this.capture = []; this.captureCyc = []; this.captureMax = d.startCapture; }
        };
      }

      push(q, cyc, l, r) {
        for (let i = 0; i < cyc.length; i++) {
          q.cyc[q.head] = cyc[i];
          q.l[q.head] = l[i];
          q.r[q.head] = r[i];
          q.head = (q.head + 1) % this.cap;
          if (q.head === q.tail) {         // full: drop the oldest
            q.tail = (q.tail + 1) % this.cap;
            this.trimmed++;
          }
        }
      }

      // Advances one stream to that cycle, holding the most recent sample at or
      // before it -- a sample-and-hold, which is what the card's own DAC does
      // between updates.
      advance(q, cycle) {
        while (q.tail !== q.head && q.cyc[q.tail] <= cycle) {
          q.lastL = q.l[q.tail];
          q.lastR = q.r[q.tail];
          q.tail = (q.tail + 1) % this.cap;
        }
      }

      newestCycle() {
        let newest = null;
        for (const q of [this.sb, this.fm, this.cd]) {
          if (q.tail === q.head) continue;
          const last = (q.head - 1 + this.cap) % this.cap;
          if (newest === null || q.cyc[last] > newest) newest = q.cyc[last];
        }
        return newest;
      }

      process(_inputs, outputs) {
        const outL = outputs[0][0], outR = outputs[0][1];
        const newest = this.newestCycle();
        if (this.playCycle === null) {
          if (newest === null) { this.silence(outL, outR); return true; }
          // Start a cushion behind the newest stamp rather than at it.
          this.playCycle = newest - this.targetLeadSec * this.cpuHz;
        }
        // Hold the play position a fixed distance behind the incoming
        // stamps. The correction is bounded hard: a fraction of a percent is
        // inaudible (a few cents) and still removes any slow divergence
        // between the guest's clock and this device's.
        if (newest !== null) {
          // A quiet stretch drains both queues and leaves the play position
          // wherever it stopped; when sound resumes, its stamps can be far
          // ahead of it. Re-anchor rather than racing through the gap.
          const gapSec = (newest - this.playCycle) / this.cpuHz;
          if (gapSec > this.maxLeadSec || gapSec < -0.5) {
            const dropped = (gapSec - this.targetLeadSec) * sampleRate;
            if (dropped > 0) this.trimmed += dropped;
            this.playCycle = newest - this.targetLeadSec * this.cpuHz;
          }
          const leadSec = (newest - this.playCycle) / this.cpuHz;
          const err = leadSec - this.targetLeadSec;
          // Asymmetric on purpose. Going slightly fast is only ever needed
          // to shed a small surplus, so +1% (about 17 cents) is plenty. Going
          // slow is the fallback when the machine itself has fallen behind
          // and the audio does not exist yet: -4% bends the pitch about 70
          // cents, which is audible but gradual and recovers, where running
          // out of samples gives dropouts instead. Either way it is one
          // smooth drift rather than a per-post lurch.
          this.rateTrim = 1 + Math.max(-0.04, Math.min(0.01, err * 1.5));
        }
        const step = (this.cpuHz / sampleRate) * this.rateTrim;
        for (let i = 0; i < outL.length; i++) {
          this.playCycle += step;
          // Never run past the newest sample the card has actually produced.
          // The step rate is only an estimate of the guest's cycles per real
          // second; pinning the play position here instead makes that
          // estimate a hint rather than something that has to be right, and
          // the machine not having produced the audio yet is the one case no
          // amount of buffering can fix anyway.
          if (newest !== null && this.playCycle > newest) {
            this.playCycle = newest;
            this.starved++;
          }
          this.advance(this.sb, this.playCycle);
          this.advance(this.fm, this.playCycle);
          this.advance(this.cd, this.playCycle);
          outL[i] = (this.sb.lastL * this.gain.sbL + this.fm.lastL * this.gain.fmL +
                     this.cd.lastL * this.gain.cdL) / 32768;
          outR[i] = (this.sb.lastR * this.gain.sbR + this.fm.lastR * this.gain.fmR +
                     this.cd.lastR * this.gain.cdR) / 32768;
          // The guest cycle each captured sample played, so a test can check
          // placement in guest time whatever pace the trim chose.
          if (this.capture && this.captureCyc.length < this.captureMax) this.captureCyc.push(this.playCycle);
        }
        if (this.capture) {
          for (let i = 0; i < outL.length && this.capture.length < this.captureMax; i++) {
            this.capture.push(outL[i]);
          }
          if (this.capture.length >= this.captureMax) {
            this.port.postMessage({ capture: this.capture, captureCyc: this.captureCyc });
            this.capture = null;
          }
        }
        this.report(outL.length, newest);
        return true;
      }

      silence(outL, outR) {
        for (let i = 0; i < outL.length; i++) { outL[i] = 0; outR[i] = 0; }
        this.report(outL.length, null);
      }

      // Same shape the Performance panel already reads: depth is the lead
      // expressed in output samples, so it still renders as milliseconds.
      report(frames, newest) {
        this.statFrames += frames;
        if (this.statFrames < sampleRate / 2) return;
        const leadSamples = (newest !== null && this.playCycle !== null)
          ? Math.max(0, ((newest - this.playCycle) / this.cpuHz) * sampleRate)
          : 0;
        this.port.postMessage({
          stats: { depth: leadSamples, starved: this.starved,
                   trimmed: this.trimmed, secs: this.statFrames / sampleRate,
                   targetMs: this.targetLeadSec * 1000 },
        });
        this.statFrames = 0;
        this.starved = 0;
        this.trimmed = 0;
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
      if (!e.data) return;
      if (e.data.stats) audioStats = e.data.stats;
      if (e.data.capture) {
        sbCapture = e.data.capture;
        sbCaptureCyc = e.data.captureCyc;
      }
    };
    sbGainNode = audioCtx.createGain();
    sbGainNode.gain.value = wheelGain(Number(sbVolume.value));

    // Tone controls sit in the mixer, before the output amp/wheel: split to
    // mono, run each channel through its own bass (low-shelf) then treble
    // (high-shelf) filter, and recombine -- see the constants above.
    const splitter = audioCtx.createChannelSplitter(2);
    const merger = audioCtx.createChannelMerger(2);
    sbBassLeft = audioCtx.createBiquadFilter();
    sbBassLeft.type = "lowshelf";
    sbBassLeft.frequency.value = kBassShelfHz;
    sbTrebleLeft = audioCtx.createBiquadFilter();
    sbTrebleLeft.type = "highshelf";
    sbTrebleLeft.frequency.value = kTrebleShelfHz;
    sbBassRight = audioCtx.createBiquadFilter();
    sbBassRight.type = "lowshelf";
    sbBassRight.frequency.value = kBassShelfHz;
    sbTrebleRight = audioCtx.createBiquadFilter();
    sbTrebleRight.type = "highshelf";
    sbTrebleRight.frequency.value = kTrebleShelfHz;

    sbNode.connect(splitter);
    splitter.connect(sbBassLeft, 0);
    sbBassLeft.connect(sbTrebleLeft);
    sbTrebleLeft.connect(merger, 0, 0);
    splitter.connect(sbBassRight, 1);
    sbBassRight.connect(sbTrebleRight);
    sbTrebleRight.connect(merger, 0, 1);
    merger.connect(sbGainNode);
    sbGainNode.connect(audioCtx.destination);
  }
  function applyWheel(pos, persist) {
    sbVolumeReadout.textContent = String(pos);
    if (sbGainNode) sbGainNode.gain.value = wheelGain(pos);
    if (persist) { try { localStorage.setItem(SB_VOLUME_KEY, String(pos)); } catch {} }
  }
  try {
    // Guard the null: Number(null) is 0, which would silently leave a
    // first-time visitor's wheel turned all the way down.
    const stored = localStorage.getItem(SB_VOLUME_KEY);
    const saved = stored === null ? NaN : Number(stored);
    if (Number.isFinite(saved) && saved >= 0 && saved <= 100) sbVolume.value = String(saved);
  } catch {}
  applyWheel(Number(sbVolume.value), false);
  sbVolume.addEventListener("input", () => applyWheel(Number(sbVolume.value), true));

  speakerCheckbox.addEventListener("change", () => {
    uiState.sound = speakerCheckbox.checked;
    persistUiState();
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

  // Bezel-corner icons mirror the checkboxes under the monitor -- same
  // state, same change handlers -- so sound / mouse stay reachable once
  // fullscreen covers the page chrome. KEYS mirrors Enable/Disable All.
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
  keymapAllBtn.addEventListener("click", () => {
    setAllMappingsEnabled(!anyMappingEnabled());
  });
  syncToggleAllBtn();

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
    // Sized from the guest time this chunk covered, and mapped in the same
    // timebase -- see pumpSbAudio for why a wall-clock-sized buffer ends in
    // a held fragment every post. The carry keeps the rounding exact.
    const exactSamples = dtSeconds * sampleRate + spkSampleCarry;
    const sampleCount = Math.max(1, Math.floor(exactSamples));
    spkSampleCarry = Math.max(0, exactSamples - sampleCount);
    const data = new Float32Array(sampleCount);

    let level = lastLevel, sampleIdx = 0;
    const cycles = edges.cycles, levels = edges.levels;
    // Effective this-chunk rate: real 66 MHz normally, `multiplier`x that
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
  // Re-reads the four tone registers and only touches an AudioParam when its
  // register actually changed, so a silent mixer doesn't write 60 times/sec.
  function refreshSbTone() {
    if (!sbTrebleLeft) return;
    const tl = machine.sbMixerRegister(0x44);
    if (tl !== lastTrebleLeftReg) { sbTrebleLeft.gain.value = mixerLevelDb(tl); lastTrebleLeftReg = tl; }
    const tr = machine.sbMixerRegister(0x45);
    if (tr !== lastTrebleRightReg) { sbTrebleRight.gain.value = mixerLevelDb(tr); lastTrebleRightReg = tr; }
    const bl = machine.sbMixerRegister(0x46);
    if (bl !== lastBassLeftReg) { sbBassLeft.gain.value = mixerLevelDb(bl); lastBassLeftReg = bl; }
    const br = machine.sbMixerRegister(0x47);
    if (br !== lastBassRightReg) { sbBassRight.gain.value = mixerLevelDb(br); lastBassRightReg = br; }
  }

  function pumpSbAudio(frameStartCycle, cyclesThisFrame, dtSeconds) {
    // Always drain all three -- even if muted, so no log can grow unbounded.
    const s = machine.sbDrainSamples();
    const fm = machine.fmDrainSamples();
    const cd = machine.cdromDrainSamples();
    if (!audioCtx || !sbNode || !speakerCheckbox.checked) return;
    refreshSbTone();
    // Hand the worklet the card's samples exactly as the emulator stamped
    // them, plus the CT1745 attenuators to apply, and nothing else. No
    // buffer length, no mapping, no mixing: all three used to be computed
    // here from performance.now() deltas measured on the same thread
    // runCycles() blocks, and placing samples by that measurement is what
    // made FM music scratchy whenever the guest was busy. The audio thread
    // has an exact clock of its own -- see kSbWorkletSrc.
    const gain = {
      sbL: machine.sbGainLeft() / kMixerUnityGain,
      sbR: machine.sbGainRight() / kMixerUnityGain,
      fmL: machine.fmGainLeft() / kMixerUnityGain,
      fmR: machine.fmGainRight() / kMixerUnityGain,
      cdL: machine.cdGainLeft() / kMixerUnityGain,
      cdR: machine.cdGainRight() / kMixerUnityGain,
    };
    guestCycAcc += cyclesThisFrame;
    guestDtAcc += dtSeconds;
    if (guestDtAcc >= 0.25) {
      const aggregate = guestCycAcc / guestDtAcc;
      guestHzEma = guestHzEma === 0 ? aggregate : guestHzEma + 0.3 * (aggregate - guestHzEma);
      guestCycAcc = 0;
      guestDtAcc = 0;
    }
    const msg = { gain, cpuHz: guestHzEma || cpuHz * TEST_CPU_MULTIPLIER };
    const transfer = [];
    if (s.cycles.length) {
      msg.sbCyc = s.cycles; msg.sbL = s.left; msg.sbR = s.right;
      transfer.push(s.cycles.buffer, s.left.buffer, s.right.buffer);
    }
    if (fm.cycles.length) {
      msg.fmCyc = fm.cycles; msg.fmL = fm.left; msg.fmR = fm.right;
      transfer.push(fm.cycles.buffer, fm.left.buffer, fm.right.buffer);
    }
    if (cd.cycles.length) {
      msg.cdCyc = cd.cycles; msg.cdL = cd.left; msg.cdR = cd.right;
      transfer.push(cd.cycles.buffer, cd.left.buffer, cd.right.buffer);
    }
    if (dbg.on) {
      dbg.posted += s.cycles.length + fm.cycles.length;
      dbg.dsp += s.cycles.length; dbg.fm += fm.cycles.length;
      dbg.posts++;
      dbg.guestCyc += cyclesThisFrame;
      dbg.wallSec += dtSeconds;
      const postMs = dtSeconds * 1000;
      if (postMs > dbg.postMsMax) dbg.postMsMax = postMs;
      if (dbg.postMs.length < 2000) dbg.postMs.push(postMs);
    }
    if (audioTrace && audioTrace.length < audioTraceMax) {
      const fc = fm.cycles;
      audioTrace.push({
        tMs: Math.round(performance.now()),
        dtMs: dtSeconds * 1000,
        cycMs: (cyclesThisFrame / cpuHz) * 1000,
        nFm: fc.length,
        nDsp: s.cycles.length,
        fmMs: (fc.length / 49715.9) * 1000,
        fmSpanMs: fc.length > 1 ? ((fc[fc.length - 1] - fc[0]) / cpuHz) * 1000 : 0,
        ringMs: audioStats ? (audioStats.depth / audioCtx.sampleRate) * 1000 : null,
        starved: audioStats ? audioStats.starved : null,
        trimmed: audioStats ? audioStats.trimmed : null,
      });
    }
    sbNode.port.postMessage(msg, transfer);
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
  // Per-post audio pacing, accumulated for the Performance panel. The pump's
  // cadence is what feeds the ring, so its spread -- not just its average --
  // is the thing to look at when audio breaks up while the clock holds.
  const dbg = { on: false, emuMs: 0, renderMs: 0, frames: 0, dropped: 0, pumps: 0,
    posts: 0, postMsMax: 0, postMs: [], guestCyc: 0, wallSec: 0,
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

  // Ring depth over the last minute, with the seconds the ring actually ran
  // dry marked underneath it. This is the pair that says whether audio broke
  // up because the machine fell behind or because the main thread did: the
  // clock chart can sit at 100% through a dropout that shows plainly here.
  function drawAudioChart(c, canvas, history, targetMs) {
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
    // Full height is half again the cushion the audio thread is actually
    // holding, so the line has somewhere to go above target rather than
    // clipping at the top.
    const target = targetMs || 80;
    const kFull = target * 1.5;

    if (history.length > 1) {
      c.beginPath();
      c.moveTo(xAt(first), h);
      history.forEach((p, i) => c.lineTo(xAt(first + i), h - Math.min(kFull, p.ring || 0) / kFull * h));
      c.lineTo(xAt(first + history.length - 1), h);
      c.closePath();
      c.fillStyle = "rgba(120, 200, 255, 0.20)";
      c.fill();
      c.beginPath();
      history.forEach((p, i) => {
        const x = xAt(first + i), y = h - Math.min(kFull, p.ring || 0) / kFull * h;
        if (i === 0) c.moveTo(x, y); else c.lineTo(x, y);
      });
      c.strokeStyle = "#78c8ff";
      c.lineWidth = 1.5;
      c.stroke();
    }

    // A second the ring ran dry gets a mark on the floor, scaled by how long
    // it was dry -- any mark at all is a dropout the ear heard.
    history.forEach((p, i) => {
      if (!p.starvedMs) return;
      const x = xAt(first + i);
      const barH = Math.max(3, Math.min(h / 3, (p.starvedMs / 20) * (h / 3)));
      c.fillStyle = "#e05545";
      c.fillRect(x - 1, h - barH, 3, barH);
    });

    const targetY = Math.round(h - target / kFull * h) + 0.5;
    c.strokeStyle = "#4a5a4a";
    c.setLineDash([3, 3]);
    c.beginPath(); c.moveTo(0, targetY); c.lineTo(w, targetY); c.stroke();
    c.setLineDash([]);

    c.font = "11px ui-monospace, Menlo, Consolas, monospace";
    c.fillStyle = "#78c8ff";
    const label = "audio ring (" + target.toFixed(0) + " ms target)";
    c.fillText(label, 6, 13);
    c.fillStyle = "#e05545";
    c.fillText("starved", 12 + c.measureText(label).width, 13);
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
    const chart3 = document.getElementById("perfChart3");
    const cctx3 = chart3.getContext("2d");
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
      const targetHz = machine.cpuHz();
      // Pump cadence: the spread matters more than the mean, because the
      // ring only has to be empty once for the ear to hear it.
      const postList = dbg.postMs.slice().sort((a, b) => a - b);
      const pct = (f) => postList.length ? postList[Math.floor(f * (postList.length - 1))] : 0;
      const postsPerSec = dbg.posts / secs;
      const guestPerWall = dbg.wallSec > 0 ? (dbg.guestCyc / dbg.wallSec) / targetHz : 0;
      const fmPerWall = dbg.wallSec > 0 ? (dbg.fm / dbg.wallSec) / 49715.9 : 0;
      const postMsMax = dbg.postMsMax;
      dbg.emuMs = dbg.renderMs = dbg.dropped = 0;
      dbg.frames = dbg.pumps = 0;
      dbg.posted = dbg.dsp = dbg.fm = 0;
      dbg.posts = 0; dbg.postMsMax = 0; dbg.postMs.length = 0;
      dbg.guestCyc = 0; dbg.wallSec = 0;

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
        // The instant reading above can sit right on target while still
        // having swung well past it a moment ago -- a brief main-thread
        // skew builds a surplus, the correction bleeds it off, and the
        // single current number never shows either happened. Range over
        // the same 60s window the chart below covers does.
        const ringHist = history.map((p) => p.ring).concat(depthMs);
        const ringMin = Math.min(...ringHist), ringMax = Math.max(...ringHist);
        audio =
          "audio   ring " + depthMs.toFixed(0) + " ms of " +
          (st && st.targetMs ? st.targetMs.toFixed(0) : "--") + " target" +
          "   range " + ringMin.toFixed(0) + "-" + ringMax.toFixed(0) + " ms (60s)\n" +
          "        " + audioCtx.state + " " + (sr / 1000).toFixed(1) + " kHz\n" +
          "        starved " + starvedMs.toFixed(1) + " ms/s   trimmed " +
          trimMs.toFixed(1) + " ms/s   latency " + (outLat * 1000).toFixed(0) + " ms\n" +
          // `fed` is what the main thread hands the audio thread -- the
          // card's own samples at the rate the card makes them, not the
          // device rate. The audio thread resamples to the device itself.
          "        fed " + (posted / 1000).toFixed(1) + "k/s   dsp " +
          (dspRate / 1000).toFixed(1) + "k/s   fm " + (fmRate / 1000).toFixed(1) + "k/s\n" +
          // How the ring is being fed, which is the other half of why it
          // empties: a post that arrives late is a hole the cushion has to
          // cover, and the p95/max are where that shows up.
          "  post  " + postsPerSec.toFixed(0) + "/s   p50 " + pct(0.5).toFixed(1) +
          " ms   p95 " + pct(0.95).toFixed(1) + " ms   max " + postMsMax.toFixed(1) + " ms\n" +
          // Guest time per wall second, and FM audio produced per wall
          // second. Below 1.00 means the machine itself has not generated
          // the audio yet -- no amount of buffering invents it.
          "  pace  guest " + guestPerWall.toFixed(3) + "x   fm " + fmPerWall.toFixed(3) +
          "x of real time\n";
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
      const targetMhz = targetHz / 1e6;
      const sr0 = audioCtx ? audioCtx.sampleRate : 48000;
      history.push({
        cpu: guestPct, clock: mhz / targetMhz * 100, host: cpuPct, fps: fps,
        ring: audioStats ? (audioStats.depth / sr0) * 1000 : 0,
        starvedMs: audioStats && audioStats.secs
          ? (audioStats.starved / sr0) * 1000 / audioStats.secs : 0,
      });
      if (history.length > 60) history.shift();
      drawPerfChart(cctx, chart, history);
      drawHostChart(cctx2, chart2, history);
      drawAudioChart(cctx3, chart3, history, audioStats ? audioStats.targetMs : 0);

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
    setFrameSize(kTextRenderWidth, kTextRenderHeight);
    frameCtx.fillStyle = "#000";
    frameCtx.fillRect(0, 0, frameCanvas.width, frameCanvas.height);
    presentFrame();
  }
  const kTextRenderWidth = 640, kTextRenderHeight = 350;  // matches ega_render.h's text-mode default

  // The guest frame lands in frameCanvas at native resolution, then is
  // blown up by a whole factor per axis into #screen with nearest-neighbour,
  // and the browser smooth-scales that to the CSS box. Every guest pixel
  // stays the same size (a plain pixelated stretch to 860px doubles every
  // third column). The box is always 4:3, because a VGA monitor fills its
  // tube in every mode: 320x200 and 720x400 are stretched tall, as on the
  // real screen.
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
    ctx.imageSmoothingEnabled = false;  // reset by every resize
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
    // Without device-pixel-content-box, a move to a monitor with a different
    // pixel ratio changes no CSS size, so watch the ratio itself.
    (function watchDpr() {
      matchMedia("(resolution: " + (window.devicePixelRatio || 1) + "dppx)")
        .addEventListener("change", () => { fitScreen(); watchDpr(); }, { once: true });
    })();
  }

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

  function frame() {
    if (!poweredOn || !machine) return;  // power switched off mid-loop -- stop, don't reschedule
    const frameT0 = dbg.on ? performance.now() : 0;
    const rgba = machine.renderFrame();
    // Resolution varies by mode (720x400 text, 320x200 CGA-compatible and
    // VGA 256-color graphics, 640x350 to 640x480 16-color, up to 1024x768
    // in the card's SVGA modes -- see ega_render.h). The 4:3 box stays put.
    const frameW = machine.renderWidth(), frameH = machine.renderHeight();
    setFrameSize(frameW, frameH);
    const img = frameCtx.createImageData(frameW, frameH);
    img.data.set(rgba);
    frameCtx.putImageData(img, 0, 0);
    presentFrame();

    floppyBay.querySelector('[data-role="led"]').classList.toggle(
      "on", machine.floppyPresent() && machine.floppyMotorOn());
    cdromBay.querySelector('[data-role="led"]').classList.toggle("on", machine.cdromBusy());
    hddLed.classList.toggle("on", machine.hddBusy());

    if (dbg.on) { dbg.renderMs += performance.now() - frameT0; dbg.frames++; }
    requestAnimationFrame(frame);
  }

  // ---- hard disk persistence (IndexedDB, in hdd-worker.js) ---------------
  // A real fixed disk keeps its contents when the machine is off; this
  // emulator's own Machine is fully discarded on power-off (see the power
  // switch section below), so C: is mirrored to IndexedDB. hdd-worker.js
  // owns that database, so saves, loads, and the one-time conversion of an
  // old 504MB C: never run on this thread next to the emulator.
  const hddWorker = (() => {
    const worker = new Worker("hdd-worker.js");
    const pending = new Map();
    let nextId = 1;
    const api = {
      onProgress: null,
      call(op, args, transfer) {
        const id = nextId++;
        return new Promise((resolve, reject) => {
          pending.set(id, { resolve, reject });
          worker.postMessage({ id, op, args }, transfer || []);
        });
      },
    };
    worker.onmessage = (e) => {
      const m = e.data;
      if (m.progress) {
        if (api.onProgress) api.onProgress(m.progress);
        return;
      }
      const p = pending.get(m.id);
      pending.delete(m.id);
      if (m.ok) p.resolve(m.result);
      else p.reject(new Error(m.error));
    };
    worker.onerror = (e) => {
      console.error("hard disk worker failed:", e.message);
      for (const p of pending.values()) p.reject(new Error(e.message));
      pending.clear();
    };
    return api;
  })();
  hddWorker.onProgress = ({ fraction }) => {
    if (loadBusyDepth > 0) {
      loadOverlayLabel.textContent = "Converting hard disk to 256MB\u2026 " + Math.floor(fraction * 100) + "%";
    }
  };

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
    const digits = document.querySelectorAll("#clockDisplay .sevenseg");
    if (mhz == null) {
      digits.forEach((d) => d.querySelectorAll("i").forEach((seg) => seg.classList.remove("on")));
      return;
    }
    const tens = Math.floor(mhz / 10) % 10;
    const ones = mhz % 10;
    [tens, ones].forEach((n, i) => {
      const on = kSevenSegOn[n] || {};
      digits[i].querySelectorAll("i").forEach((seg) => {
        seg.classList.toggle("on", !!on[seg.classList[0]]);
      });
    });
  }
  // Turbo LED and clock need power -- the button still latches while off
  // (like a real tower switch), but the jewelry goes dark with Power.
  function syncTurboChrome() {
    const on = turboBtn.getAttribute("aria-pressed") === "true";
    if (!poweredOn) {
      turboLed.classList.remove("turbo-on");
      setClockDisplay(null);
      return;
    }
    turboLed.classList.toggle("turbo-on", on);
    setClockDisplay(on ? 66 : 33);
  }
  function applyTurbo(on) {
    turboBtn.setAttribute("aria-pressed", on ? "true" : "false");
    cpuHz = on ? 66000000 : 33000000;
    if (machine) machine.setTurbo(on);
    syncTurboChrome();
  }
  turboBtn.addEventListener("click", () => {
    applyTurbo(turboBtn.getAttribute("aria-pressed") !== "true");
  });
  let poweredOn = false;
  syncTurboChrome();  // start dark until the first power-on
  let firmware = null;  // {Module, bios, vga, hdd} once fetched -- fetched once, reused every power-on
  // The WD Caviar AC2250's 1010 cyl x 9 head x 55 sec x 512 bytes
  // (wd1003.h), and the 1024/16/63 drive older builds saved, which
  // hdd-convert.js converts.
  const kHddImageBytes = 255974400;
  const kLegacyHddImageBytes = 528482304;
  let pendingFloppy = null;  // {name, bytes} -- "what's physically in the drive" (this system's one bay)
  let pendingCdrom = null;   // {name, bytes} for a plain ISO, or {name, cueText, bin}
                             // for a mounted CUE+BIN -- same idea, for the CD-ROM bay

  resetBtn.addEventListener("click", () => {
    // A real reset button pulses the RESET line to the CPU/chipset only --
    // RAM and CMOS both survive, unlike the power switch above. No-op while
    // powered off, matching a real machine (nothing to reset without power).
    if (machine) machine.reset();
  });

  // What C: mounts next power-on. `pendingHdd` is an image already read for
  // it at page load, so power-on doesn't read it twice; otherwise power-on
  // asks the worker, and with nothing saved it mounts the factory image and
  // hands those bytes to the worker as the new C:.
  let pendingHdd = null;  // Uint8Array | null
  let hddLabel = "FreeDOS (default)";
  // Set while an old 504MB save that couldn't be converted is still stored.
  let legacyHddNote = null;
  const kLegacyHddNote = "Your old 504MB C: couldn't be converted to 256MB. Download it before you reset.";
  let legacyConvertFailed = false;  // don't retry it at every power-on
  // Only fetched when a session actually needs the pristine factory image
  // (nothing saved yet, or "Reset to factory").
  let factoryHddPromise = null;
  function factoryHddUrl() {
    return new URL("disks/freedos-hdd.img", location.href).href;
  }
  // Cheap identity for whatever freedos-hdd.img the server is currently
  // shipping, so an untouched C: seeded from an older build can be told
  // apart from a current one without downloading the body.
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
          console.error("could not check factory hard disk freshness:", err);
          return null;
        }
      })();
    }
    return factoryFingerprintPromise;
  }
  let currentFactoryFingerprint = null;

  function ensureFactoryHdd() {
    if (firmware && firmware.hdd) return Promise.resolve(firmware.hdd);
    if (!factoryHddPromise) {
      factoryHddPromise = (async () => {
        currentFactoryFingerprint = await factoryFingerprint();
        if (loadBusyDepth > 0) loadOverlayLabel.textContent = "Downloading hard disk…";
        const r = await fetch("disks/freedos-hdd.img");
        if (!r.ok) throw new Error("freedos-hdd.img HTTP " + r.status);
        const bytes = await r.arrayBuffer();
        if (firmware) firmware.hdd = bytes;
        return bytes;
      })();
      factoryHddPromise.catch(() => { factoryHddPromise = null; });
    }
    beginLoad("Loading hard disk…");
    return factoryHddPromise.finally(() => endLoad());
  }

  // The saved C: as {bytes, status, modified}, or null when the factory
  // image should be used. An untouched factory C: seeded from an older
  // freedos-hdd.img is dropped so the visitor gets the current one.
  async function loadSavedHdd() {
    const info = await hddWorker.call("info");
    legacyHddNote = info.legacy ? kLegacyHddNote : null;
    if (info.kind === "none" || (info.kind === "legacy" && legacyConvertFailed)) return null;
    if (info.kind === "saved" && !info.modified && info.factoryFp) {
      const fp = await factoryFingerprint();
      if (fp !== null && fp !== info.factoryFp) {
        await hddWorker.call("forget");
        return null;
      }
    }
    const r = await hddWorker.call("load");
    if (r.status === "legacy-unconverted") {
      console.error("old 504MB hard disk not converted:", r.reason);
      legacyHddNote = kLegacyHddNote;
      legacyConvertFailed = true;
      return null;
    }
    if (r.status === "none") return null;
    if (r.status === "converted") legacyHddNote = null;
    return { bytes: new Uint8Array(r.buffer), status: r.status, modified: r.modified };
  }

  const hddStatus = document.getElementById("hddStatus");
  const hddResetBtn = document.getElementById("hddResetBtn");
  const hddBlankBtn = document.getElementById("hddBlankBtn");
  const hddDownloadBtn = document.getElementById("hddDownloadBtn");
  const hddLegacyDownloadBtn = document.getElementById("hddLegacyDownloadBtn");
  const hddUploadInput = document.getElementById("hddUploadInput");
  function refreshHddControls() {
    hddStatus.textContent = "Using: " + hddLabel + (legacyHddNote ? ". " + legacyHddNote : "");
    hddLegacyDownloadBtn.hidden = !legacyHddNote;
    // A real fixed disk can't be swapped while the machine is running --
    // every one of these actions only ever affects the *next* power-on.
    hddResetBtn.disabled = !firmware || poweredOn;
    hddBlankBtn.disabled = !firmware || poweredOn;
    hddDownloadBtn.disabled = !firmware;  // download works even while running -- it's read-only
    hddUploadInput.disabled = !firmware || poweredOn;
    document.getElementById("hddUploadBtn").disabled = !firmware || poweredOn;
  }
  hddResetBtn.addEventListener("click", () => {
    pendingHdd = null;
    legacyHddNote = null;
    hddLabel = "FreeDOS (default). Takes effect at next power-on";
    refreshHddControls();
    // "Reset to factory" means the image the server has right now, so drop
    // any copy already fetched and download it again.
    factoryHddPromise = null;
    if (firmware) firmware.hdd = null;
    hddPersistChain = hddPersistChain.then(() => hddWorker.call("clear")).catch((err) => {
      console.error("could not clear saved hard disk:", err);
    });
    ensureFactoryHdd().catch((err) => console.error(err));
  });
  hddBlankBtn.addEventListener("click", () => {
    if (!firmware) return;
    pendingHdd = null;
    legacyHddNote = null;
    hddLabel = "blank drive (unformatted). Takes effect at next power-on";
    refreshHddControls();
    // All zero -- unformatted, like a drive fresh from the factory floor.
    hddPersistChain = hddPersistChain.then(() => hddWorker.call("blank", { length: kHddImageBytes })).catch((err) => {
      console.error("could not save blank hard disk:", err);
    });
  });
  function downloadBytes(bytes, name) {
    const blob = new Blob([bytes], { type: "application/octet-stream" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  }
  // A real file on the visitor's own disk, independent of this browser's
  // storage -- the same "save modified media" idea the floppy eject flow
  // already offers, just for C: (which isn't ejectable, so it needs its
  // own explicit control instead of piggybacking on a drive-swap gesture).
  hddDownloadBtn.addEventListener("click", async () => {
    if (!firmware) return;
    // Whatever is *actually* current: the live, possibly-just-written
    // image if the machine is running, else whatever's staged for the
    // next power-on, else the pristine factory image.
    let bytes;
    if (poweredOn && machine) {
      bytes = machine.hddImage();
    } else if (pendingHdd) {
      bytes = pendingHdd;
    } else {
      await hddPersistChain;
      const r = await hddWorker.call("load");
      bytes = r.buffer ? new Uint8Array(r.buffer) : new Uint8Array(await ensureFactoryHdd());
    }
    downloadBytes(bytes, "pc486-hdd.img");
  });
  hddLegacyDownloadBtn.addEventListener("click", async () => {
    const buf = await hddWorker.call("loadLegacy");
    if (buf) downloadBytes(new Uint8Array(buf), "pc486-hdd-504mb.img");
  });
  hddUploadInput.addEventListener("change", async () => {
    const f = hddUploadInput.files[0];
    hddUploadInput.value = "";
    if (!f || !firmware) return;
    await withLoad("Loading hard disk…", async () => {
      const buf = await f.arrayBuffer();
      // The AC2250's geometry is fixed in CMOS, not derived from the image.
      // An image of the wrong size would still fail safely (wd1003.cpp's
      // own bounds check reports a genuine IDNF error), but refusing it up
      // front gives a clearer reason than a disk error deep into a boot.
      if (buf.byteLength === kLegacyHddImageBytes) {
        loadOverlayLabel.textContent = "Converting hard disk to 256MB…";
        const r = await hddWorker.call("convert", { buffer: buf }, [buf]);
        if (!r.ok) {
          alert(r.code === "full"
            ? "That 504MB image has more on it than fits in 256MB. Not mounted."
            : "That 504MB image couldn't be converted (" + r.reason + "). Not mounted.");
          return;
        }
        hddLabel = "converted from 504MB (" + f.name + "). Takes effect at next power-on";
      } else if (buf.byteLength === kHddImageBytes) {
        await hddWorker.call("replace", { buffer: buf, modified: true }, [buf]);
        hddLabel = "uploaded image (" + f.name + "). Takes effect at next power-on";
      } else {
        alert("That file is " + buf.byteLength + " bytes. This machine's hard disk is a 256MB " +
              "WD Caviar AC2250, exactly " + kHddImageBytes + " bytes. An older 504MB pc486 " +
              "image (" + kLegacyHddImageBytes + " bytes) gets converted. Not mounted.");
        return;
      }
      pendingHdd = null;
      legacyHddNote = null;
      refreshHddControls();
    });
  });

  async function powerOn() {
    if (poweredOn || !firmware) return;
    if (!machine) {
      // Free the previous power cycle's machine before building the next
      // one. An embind handle owns a C++ object that outlives the JS
      // reference, so dropping `machine` at powerOff() freed nothing and
      // every power cycle leaked another C:-sized block of wasm heap (plus
      // 419MB more whenever a CD-ROM disc happens to be loaded too) -- the
      // second power-on then died trying to grow past it. Deleted here
      // rather than in powerOff() so the handle
      // stays callable while the machine is off, which is how "power off
      // really does stop the cycle counter" is observed (tests/boot.spec.ts).
      if (lastMachine) {
        lastMachine.delete();
        lastMachine = null;
        // Power-on waits on the hard disk worker below, so tests waiting
        // for the new machine must not see the one just deleted.
        if (window.__test) window.__test.machine = null;
      }
      machine = new firmware.Module.Machine();
      applyTurbo(turboBtn.getAttribute("aria-pressed") === "true");
      if (perfRequested) startPerfPanel();
      machine.loadRom(0x100000 - firmware.bios.byteLength, new Uint8Array(firmware.bios));
      machine.loadRom(0xC0000, new Uint8Array(firmware.vga));
      // Factory image is fetched on demand -- a session that already has
      // C: in IndexedDB never downloads it (see ensureFactoryHdd).
      let hdd = pendingHdd;
      pendingHdd = null;
      if (!hdd) {
        const saved = await loadSavedHdd();
        if (saved) hdd = saved.bytes;
      }
      const seedFactory = !hdd;
      if (seedFactory) hdd = new Uint8Array(await ensureFactoryHdd());
      machine.mountHdd(hdd);
      if (seedFactory) {
        // From here on the worker's copy is C:, so this thread lets go of
        // the factory bytes instead of holding a second copy.
        const buf = hdd.buffer;
        firmware.hdd = null;
        factoryHddPromise = null;
        hddPersistChain = hddPersistChain.then(() => hddWorker.call("replace",
          { buffer: buf, factoryFp: currentFactoryFingerprint, modified: false }, [buf])).catch((err) => {
          console.error("could not save hard disk:", err);
        });
      }
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
        if (pendingCdrom.cueText) {
          machine.mountCdromCue(pendingCdrom.cueText, pendingCdrom.bin);
        } else {
          machine.mountCdrom(pendingCdrom.bytes);
        }
        setBayLoaded(cdromBay, pendingCdrom.name);
      }
      // The board's battery-backed clock has been keeping local time all
      // along, so a fresh power-on reads the visitor's own clock.
      const now = new Date();
      machine.setRtc(now.getFullYear(), now.getMonth() + 1, now.getDate(),
        now.getHours(), now.getMinutes(), now.getSeconds(), now.getDay() + 1);
    }
    poweredOn = true;
    powerLed.classList.add("power-on");
    syncTurboChrome();
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
        machine, sendKey, screenEl, mapKey, fitScreen, frameCanvas,
        get keymapRows() { return keymapRows.map((r) => ({ ...r, to: r.to.slice() })); },
        applyWasdPreset: () => applyPresetRows(kWasdPresetRows),
        applyShiftCtrlPreset: () => applyPresetRows(kShiftCtrlPresetRows),
        setWasdPresetEnabled,
        isWasdPresetActive,
        clearKeymap,
        setAllMappingsEnabled,
        upsertMapping: (from, to, preset) => {
          upsertMapping(from, to, preset);
          releaseAllHeldKeys();
          persistKeymap();
          renderKeymapTable();
        },
        setMappingEnabled: (from, enabled) => {
          const row = keymapRows.find((r) => r.from === from);
          if (!row) return false;
          row.enabled = !!enabled;
          releaseAllHeldKeys();
          persistKeymap();
          renderKeymapTable();
          return true;
        },
        deleteMapping: (from) => {
          const before = keymapRows.length;
          keymapRows = keymapRows.filter((r) => r.from !== from);
          if (keymapRows.length === before) return false;
          releaseAllHeldKeys();
          persistKeymap();
          renderKeymapTable();
          return true;
        },
        get audioState() { return audioCtx ? audioCtx.state : null; },
        get loadOverlayVisible() { return loadOverlayEl.classList.contains("visible"); },
        get loadOverlayText() { return loadOverlayLabel.textContent; },
        beginLoad, endLoad,
        get sbWheelGain() { return sbGainNode ? sbGainNode.gain.value : null; },
        // Ring depth in ms as the worklet last reported it (it posts twice a
        // second), null until the first report arrives.
        get audioStatsRaw() { return audioStats; },
        get hasSbCapture() { return sbCapture !== null; },
        startSbCapture: (n) => { sbCapture = null; sbNode.port.postMessage({ startCapture: n || 48000 }); },
        takeSbCapture: () => { const c = sbCapture; sbCapture = null; return c; },
        // The guest cycle each sample of the last capture played.
        get sbCaptureCyc() { return sbCaptureCyc; },
        get audioSampleRate() { return audioCtx ? audioCtx.sampleRate : null; },
        get sbRingMs() {
          return audioStats && audioCtx ? (audioStats.depth / audioCtx.sampleRate) * 1000 : null;
        },
        // Current tone-shelf filter gains in dB, for tests -- null until
        // audio has started and at least one frame has refreshed them.
        sbToneGains: () => sbTrebleLeft ? {
          trebleLeft: sbTrebleLeft.gain.value, trebleRight: sbTrebleRight.gain.value,
          bassLeft: sbBassLeft.gain.value, bassRight: sbBassRight.gain.value,
        } : null,
        get heldKeysSize() { return heldKeys.size; },
        // Flush C: to IndexedDB and resolve when the worker's write
        // finishes -- tests that reload must await this, or the save races
        // the navigation and the next boot falls back to factory.
        persistHdd: () => persistHddIfDirty(),
        // Mark C: as the visitor's own even when clean. Persistence specs
        // can't rely on FreeDOS having dirtied the image during a fast boot.
        forcePersistHdd: () => persistHddSnapshot(),
        whenHddSaved: () => hddPersistChain,
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
  // Returns a promise that settles when the worker's write completes (or
  // immediately when there is nothing dirty). Callers that are about to
  // tear the page down (powerOff, a test about to reload) must await it.
  let hddPersistChain = Promise.resolve();
  function queueHddSave(m, patches) {
    hddLabel = "saved (this session)";
    refreshHddControls();
    const transfer = patches.map((p) => p.bytes.buffer);
    hddPersistChain = hddPersistChain.then(async () => {
      const r = await hddWorker.call("patch", { patches }, transfer);
      // Nothing stored to patch yet: store the whole image instead.
      if (r.needFull) {
        const img = m.hddImage();
        await hddWorker.call("replace", { buffer: img.buffer, modified: true }, [img.buffer]);
      }
    }).catch((err) => {
      console.error("could not save hard disk changes:", err);
    });
    return hddPersistChain;
  }
  function persistHddIfDirty() {
    if (!machine || !machine.hddDirty()) return hddPersistChain;
    // Only the sectors this session actually wrote -- see wd1003.h's
    // dirty_ranges() comment and the audio-worklet comment above this
    // file's speaker code.
    const patches = machine.hddDirtyPatches();
    machine.clearHddDirty();
    return queueHddSave(machine, patches);
  }
  // Test helper: mark C: as saved even when hddDirty() is false (a fast
  // ?test=1&fast=1 boot often never dirties the factory image).
  function persistHddSnapshot() {
    if (!machine) return hddPersistChain;
    if (machine.hddDirty()) return persistHddIfDirty();
    return queueHddSave(machine, []);
  }

  async function powerOff() {
    if (!poweredOn) return;
    await persistHddIfDirty();
    poweredOn = false;  // pump()/frame() see this on their next tick and stop rescheduling
    lastMachine = machine;  // freed at the next powerOn() -- see there
    machine = null;      // real hardware: RAM is gone the instant power is cut
    powerLed.classList.remove("power-on");
    syncTurboChrome();
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
    beginLoad("Loading\u2026");
    try {
      await paintLoadOverlay();
      await loadEmulatorModule();
      const [Module, saved, bios, vga] = await Promise.all([
        Pc486({}),
        loadSavedHdd(),
        fetch("roms/BIOS-bochs-legacy").then((r) => r.arrayBuffer()),
        fetch("roms/VGABIOS-lgpl-latest.bin").then((r) => r.arrayBuffer()),
      ]);
      firmware = { Module, bios, vga, hdd: null };
      if (saved) {
        pendingHdd = saved.bytes;
        hddLabel = saved.status === "converted" ? "converted from 504MB"
          : saved.modified ? "saved (previous visit)" : "FreeDOS (default)";
      } else {
        await ensureFactoryHdd();
      }

      powerSwitch.disabled = false;
      refreshHddControls();
      // Boot straight to a running machine once firmware is ready, rather
      // than making the visitor find and click the power switch themselves.
      powerSwitch.checked = true;
      await powerOn();
    } finally {
      endLoad();
    }
  })().catch((err) => {
    // no on-page error surface -- the power switch simply never enables;
    // the real failure detail goes to the console for diagnosis.
    console.error(err);
  });
})();
