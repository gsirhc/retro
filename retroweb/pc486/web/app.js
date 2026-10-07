"use strict";
(() => {
  // ---- page theme ----
  // Shared via the retro8080.theme localStorage key; see shared/theme-picker.js.
  initThemePicker();

  // "Last built" is the wasm's mtime on the server, like the other machines' footers.
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

  // Test-only CPU speed multiplier (`?test=1&fast=1`); no page control reaches it. The suite's
  // boot() opts most tests in, and a few smoke tests omit it to verify real-speed pacing.
  // The value (20, as ibmpc-at) is untuned against this machine's boot time.
  const testParams = new URLSearchParams(location.search);
  const TEST_CPU_MULTIPLIER =
    testParams.get("test") === "1" && testParams.get("fast") === "1" ? 20 : 1;

  // Opt-in diagnostic, not part of the machine: `?fmtrace` records every OPL3 register write with
  // its CPU cycle stamp (window.__fm). Free unless __fm.start() is called.
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

  // Opt-in diagnostic: `?audiotrace` records one row per audio post (wall and guest-cycle length,
  // sample counts, ring depth and health) to replay a real session's pattern (PC486_REVIEW.md
  // section 31). Needs sound enabled.
  if (testParams.has("audiotrace")) {
    window.__audio = {
      start: (n) => { audioTraceMax = n || 40000; audioTrace = []; return "recording"; },
      stop: () => { const n = audioTrace ? audioTrace.length : 0; audioTraceMax = 0; return n; },
      summary: () => {
        if (!audioTrace || audioTrace.length === 0) return "no rows -- is sound enabled?";
        const q = (vals, f) => {
          const v = vals.slice().sort((a, b) => a - b);
          return v[Math.floor(f * (v.length - 1))];
        };
        const dt = audioTrace.map((r) => r.dtMs);
        const guestOverWall = audioTrace.map((r) => r.cycMs / r.dtMs);
        const audioOverWall = audioTrace.map((r) => r.fmMs / r.dtMs);
        // The worklet reports twice a second, so early rows lack a ring figure.
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

  // ---- UI state ----
  // Prefs in localStorage under one key, opt-in/open by default. Keymap bindings have their own key.
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

  // ---- keyboard: physical key -> IBM AT Set 1 scan code ----
  // i8042.h inject_scancode() passes Set 1 through, so this table gives what an AT keyboard's
  // Set-2-to-Set-1 translation yields. A two-entry array is an 0xE0-prefixed extended key (added
  // after the 84-key layout used every plain code). Break = make | 0x80 on the last byte.
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
    // Print Screen (4-byte make and break) and Pause/Break (one 6-byte sequence on press, no break
    // code) are fixed AT sequences (Scan Code Set 1).
    PrintScreen: { make: [0xE0, 0x2A, 0xE0, 0x37], break: [0xE0, 0xB7, 0xE0, 0xAA] },
    Pause: { make: [0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5], break: [] },
  };

  let machine = null;
  // Kept so powerOn() can free it; never run.
  let lastMachine = null;

  // The 8042 has one single-byte output register, as on hardware: a second byte before IRQ1 reads
  // the first overwrites it. A JS loop calling injectScancode() twice in one turn outruns it, so
  // every byte of a multi-byte sequence needs real spacing, not just make to break
  // (IBM_PCAT_REVIEW.md).
  // gapMs defaults to 20, enough for INT 9 to drain a byte on an idle host. Ctrl-Alt-Del passes 50
  // so a busy main thread can't collapse the makes into the buffer.
  // One queue so keys' bytes never interleave; an E0 split from its code reaches DOS as another key.
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
      // Fixed sequences that don't fit prefix + break-bit (see the SET1 entries).
      injectScancodeSequence(isBreak ? entry.break : entry.make);
      return;
    }
    const bytes = Array.isArray(entry) ? entry.slice() : [entry];
    const last = bytes.length - 1;
    bytes[last] = isBreak ? (bytes[last] | 0x80) : bytes[last];
    injectScancodeSequence(bytes);
  }
  // Tracks held keys so a lost keyup (focus loss, tab hide, Pointer Lock release swallowing Escape)
  // can't leave a key held in the guest. Browser safety net, not hardware behavior.
  const heldKeys = new Set();
  // Physical key -> guest codes for that press. Guest keys are refcounted so a shared chord
  // modifier (WASD's Alt strafe) stays down while any source is held.
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

  // ---- Key Mapper ----
  // Browser-edge remap of one KeyboardEvent.code to guest codes; the guest still sees Set 1.
  // Doom 1.2 era games predate WASD, so the preset sends A/D as Alt+arrow to strafe.
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
    // In-place checkbox flips; a table rebuild made KEYS feel unlike the other toggles.
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
    // WASD's four rows render as one line.
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

  // Add mapping: From (one key), then To (one key; held modifiers become a chord).
  let capturePhase = null;
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
    // Bare modifiers are valid targets (Left Shift -> Ctrl); defer to keyup so Alt+Arrow can chord.
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
    // Resumes a context that was created or suspended without a gesture; a restored sound preference would stay silent until the box is toggled.
    if (speakerCheckbox.checked) ensureAudioStarted();
  });
  window.addEventListener("blur", releaseAllHeldKeys);
  document.addEventListener("visibilitychange", () => { if (document.hidden) releaseAllHeldKeys(); });

  // ---- PS/2 mouse (8042 AUX port) ----
  // Off by default, remembered once chosen. Pointer Lock needs a canvas click.
  const mouseCaptureCheckbox = document.getElementById("mouseCaptureEnabled");
  mouseCaptureCheckbox.checked = uiState.mouse;
  mouseCaptureCheckbox.addEventListener("change", () => {
    uiState.mouse = mouseCaptureCheckbox.checked;
    persistUiState();
  });
  let mouseButtons = 0;
  document.addEventListener("pointerlockchange", () => {
    if (document.pointerLockElement !== screenEl) {
      mouseButtons = 0;
      // Escape can exit Pointer Lock without a keyup reaching us; release held keys.
      releaseAllHeldKeys();
    }
  });
  // DOM button numbering (0=left, 1=middle, 2=right) differs from the PS/2 AUX bit order
  // (bit0=left, bit1=right, bit2=middle, i8042.h kMouse*).
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
    // dy is +Y away from the user, opposite of the browser's movementY.
    machine.injectMouseEvent(e.movementX, -e.movementY, mouseButtons);
  });

  // Click-to-type banner and fullscreen are web-UI conveniences (shared/focus-hint.js, fullscreen.js).
  // poweredOn is passed as a predicate because it is declared further down.
  const isRunning = () => poweredOn;
  const updateFocusHint = initFocusHint(screenEl, isRunning);
  // escBtn/sendEscape omitted: #escBtn already uses the [data-key] injection of the F-key row.
  initFullscreen({
    bezelEl: document.getElementById("bezel"),
    screenEl,
    fullscreenBtn: document.getElementById("fullscreenBtn"),
    fsEscHint: document.getElementById("fsEscHint"),
    fsEscHintOkBtn: document.getElementById("fsEscHintOk"),
    isRunning,
  });

  // "Barebones FreeDOS" notice: explains why C: has no games (Base package set, PC486_REVIEW.md §19.5)
  // and that JEMMEX is a real V86 manager. Dismissed when the screen gains focus (click or Tab), and
  // remembered in localStorage so it only returns for a fresh visitor.
  const BOOT_NOTICE_KEY = "retro8080.pc486BootNoticeDismissed";
  const bootNoticeEl = document.getElementById("bootNotice");
  function hideBootNotice() { bootNoticeEl.classList.remove("visible"); }
  // Only focus counts as seen; powerOff() hides it without marking it seen.
  function dismissBootNotice() {
    hideBootNotice();
    try { localStorage.setItem(BOOT_NOTICE_KEY, "1"); } catch {}
  }
  document.addEventListener("focusin", () => {
    if (screenEl.contains(document.activeElement)) dismissBootNotice();
  });

  // Overlay while a large image downloads or loads. Nestable; two rAFs after show let the spinner
  // paint before a sync wasm mount freezes the main thread.
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

  // ---- floppy drive (single 3.5" bay) ----
  // Slots work powered on or off; pendingFloppy is remounted at power-on.
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
      // Hand back the image before ejecting if the session wrote to it; those writes live only in memory.
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

  // ---- CD-ROM drive (atapi_cdrom) ----
  // Read-only media, so no dirty-image path. Empty by default since C: already ships with FreeDOS;
  // "Insert FreeDOS CD..." fetches the ~400MB .iso lazily (PC486_REVIEW.md).
  // ---- freeware disks & drivers ----
  // Fetches web/disks/ctmouse.img (disks/build-ctmouse-floppy.sh) into A: via the same path as a
  // picked file. The button stays live: a real drive takes a diskette over a loaded one.
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
      // A mixed-mode disc is a CUE sheet plus its BIN, selected together. Anything else is a plain ISO.
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

  // ---- PC speaker ----
  // Muted every page load, never restored: browsers block audio until a gesture, and off stays off
  // until the visitor opts in.
  // Output is one persistent AudioWorkletNode fed by a ring buffer, not back-to-back one-shot
  // source nodes: a main-thread hiccup would leave a gap then a jump to a nonzero level, an audible
  // click (IBM_PCAT_REVIEW.md). The worklet runs on the audio thread, so a stall just holds the
  // last level, and there is no scheduling clock to resync.
  const speakerCheckbox = document.getElementById("speakerEnabled");
  const sbVolume = document.getElementById("sbVolume");
  const sbVolumeReadout = document.getElementById("sbVolumeReadout");
  // The wheel position persists. Enable Sound is remembered too; a restored checkmark may need one
  // click before audio starts.
  const SB_VOLUME_KEY = "retro8080.pc486SbVolume";
  speakerCheckbox.checked = uiState.sound;
  let audioCtx = null, speakerNode = null, lastLevel = false;
  // SB16 backplate volume wheel: an analog pot after the output amp (CT1740/CT1750), outside what the
  // guest sees; the CT1745 Master/FM attenuators are in pumpSbAudio's gains. The motherboard speaker
  // is separate on a real tower, so speakerNode bypasses this.
  let sbGainNode = null;
  // Square law as a stand-in for an audio taper: 50 is unity (kMixerUnityGain), 100 is +12 dB.
  const kWheelMaxGain = 4.0;
  function wheelGain(pos) { const f = pos / 100; return f * f * kWheelMaxGain; }
  let sbNode = null;
  let audioStats = null;
  // Fractional sample carried between posts (pumpSbAudio) and the ring depth the pump holds,
  // a little under the worklet's 50ms trim ceiling.
  let spkSampleCarry = 0;
  // Guest cycles per real second averaged over ~0.2s of posts. The worklet steps its play position
  // by this, so it must be the achieved rate (nominal is wrong under the fast multiplier). Safe to
  // average because the worklet closes a loop on the lead it observes (PC486_REVIEW.md section 31).
  // Accumulated over a window: the mean of per-post ratios is biased high under load.
  let guestHzEma = 0, guestCycAcc = 0, guestDtAcc = 0;
  // `?audiotrace` capture buffer (window.__audio). Null unless started.
  let audioTrace = null, audioTraceMax = 0;
  let sbCapture = null, sbCaptureCyc = null;

  // CT1745 tone controls (SBPG ch. 4): 44h/45h Treble L/R, 46h/47h Bass L/R, high nibble, default
  // 8<<4. 0-7 is -14 to 0 dB, 8-15 is 0 to +14 dB, 2 dB steps (7 and 8 are flat). Upstream of the
  // output amp and the wheel (sbGainNode), unlike output_gain_*/fm_gain_* which the core applies.
  let sbBassLeft = null, sbBassRight = null, sbTrebleLeft = null, sbTrebleRight = null;
  let lastTrebleLeftReg = -1, lastTrebleRightReg = -1, lastBassLeftReg = -1, lastBassRightReg = -1;
  // Shelf corner frequencies: SBPG gives only the +/-14 dB range, so these two are an uncited
  // estimate of a period analog tone control.
  const kBassShelfHz = 100;
  const kTrebleShelfHz = 5000;
  function mixerLevelDb(reg) {
    const level = (reg >> 4) & 0x0f;
    return level <= 7 ? level * 2 - 14 : (level - 8) * 2;
  }

  // Worklet source, registered from a Blob URL so the speaker path stays in this script.
  const kSpeakerWorkletSrc = `
    class PcSpeakerProcessor extends AudioWorkletProcessor {
      constructor() {
        super();
        // ~350ms at 48kHz: absorbs a jank spike, not steady-state depth.
        this.ring = new Float32Array(16384);
        this.writeIdx = 0;
        this.readIdx = 0;
        this.available = 0;
        this.lastSample = 0;
        // Trim back to target in process(), not onmessage(): only process() runs on
        // the real audio clock. Trimming per message walks readIdx into stale audio.
        this.targetAvailable = Math.round(sampleRate * 0.05);  // 50ms
        // DC blocker: Web Audio is DC-coupled, so a speaker parked on a rail thumps.
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
              // overflow: drop oldest, like an unread FIFO
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
          // underrun: hold last sample to avoid a click
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

  // Stereo, sample-locked SB16 mixer worklet.
  const kSbWorkletSrc = `
    // Samples are placed by guest-cycle stamp against the audio clock here;
    // main-thread timing is too bursty to derive a rate from.
    class Sb16Processor extends AudioWorkletProcessor {
      constructor() {
        super();
        // FM, digitized audio and CD-DA mix in analog, each behind its own CT1745 attenuator.
        const kCap = 1 << 16;   // ~1.3s of FM at 49.7kHz
        this.cap = kCap;
        this.sb = { cyc: new Float64Array(kCap), l: new Int16Array(kCap), r: new Int16Array(kCap),
                    head: 0, tail: 0, lastL: 0, lastR: 0 };
        this.fm = { cyc: new Float64Array(kCap), l: new Int16Array(kCap), r: new Int16Array(kCap),
                    head: 0, tail: 0, lastL: 0, lastR: 0 };
        this.cd = { cyc: new Float64Array(kCap), l: new Int16Array(kCap), r: new Int16Array(kCap),
                    head: 0, tail: 0, lastL: 0, lastR: 0 };
        this.gain = { sbL: 1, sbR: 1, fmL: 1, fmR: 1, cdL: 1, cdR: 1 };
        // exact by construction: the pump grants this much credit per second
        this.cpuHz = 66000000;
        this.playCycle = null;     // guest cycle the next output sample sits at
        this.rateTrim = 1;         // tiny correction, see below
        // 80ms lead absorbs GC and disk-persist stalls; 40ms dropped out.
        this.targetLeadSec = 0.08;
        // caps latency when the guest outruns real time
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
          // capture for tests/fmquality.spec.ts
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

      // sample-and-hold, like the card's DAC
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
        // Bounded rate trim keeps a fixed lead without audible pitch shift.
        if (newest !== null) {
          // re-anchor after silence instead of racing through the gap
          const gapSec = (newest - this.playCycle) / this.cpuHz;
          if (gapSec > this.maxLeadSec || gapSec < -0.5) {
            const dropped = (gapSec - this.targetLeadSec) * sampleRate;
            if (dropped > 0) this.trimmed += dropped;
            this.playCycle = newest - this.targetLeadSec * this.cpuHz;
          }
          const leadSec = (newest - this.playCycle) / this.cpuHz;
          const err = leadSec - this.targetLeadSec;
          // +1% sheds surplus; -4% (~70 cents) when the guest falls behind beats dropouts.
          this.rateTrim = 1 + Math.max(-0.04, Math.min(0.01, err * 1.5));
        }
        const step = (this.cpuHz / sampleRate) * this.rateTrim;
        for (let i = 0; i < outL.length; i++) {
          this.playCycle += step;
          // can't play audio the guest hasn't produced yet
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
          // guest cycle per captured sample
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

      // depth is lead in output samples for the Performance panel
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
    // A context can be suspended silently (Safari may not auto-resume on the constructing gesture;
    // any browser may suspend an idle one), so re-checking the box must resume an existing context.
    if (audioCtx) {
      if (audioCtx.state === "suspended") await audioCtx.resume();
      return;
    }
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === "suspended") await audioCtx.resume();
    // AudioWorklet is missing outside a secure context (https or localhost, not a LAN IP). Degrade
    // to silent: speakerNode/sbNode stay null and the pumps no-op.
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
    // The ring's health (depth, underruns); only the worklet thread can see it.
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

    // Tone controls sit before the output amp/wheel: split to mono, bass low-shelf then treble
    // high-shelf per channel, recombine.
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
    // Number(null) is 0, which would leave a first-time visitor's wheel at zero.
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
      // A stopped pump leaves the context running, which keeps the tab's speaker icon lit. Suspend,
      // like powerOff(); ensureAudioStarted() resumes on re-check.
      audioCtx.suspend().catch(() => {});
    }
  });

  // Bezel-corner icons mirror the checkboxes so sound and mouse stay reachable in fullscreen.
  // KEYS mirrors Enable/Disable All.
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

  // Converts this frame's (cpu_cycle, level) edge trace (speakerEdges()) into samples for the
  // worklet's ring. The worklet plays them at its own pace.
  function pumpAudio(frameStartCycle, cyclesThisFrame, dtSeconds) {
    const edges = machine.speakerEdges();
    if (!audioCtx || !speakerNode || !speakerCheckbox.checked || cyclesThisFrame <= 0) return;
    const sampleRate = audioCtx.sampleRate;
    // Sized from the guest time this chunk covered, in the same timebase; a wall-clock-sized buffer
    // ends in a held fragment every post (see pumpSbAudio). The carry keeps rounding exact.
    const exactSamples = dtSeconds * sampleRate + spkSampleCarry;
    const sampleCount = Math.max(1, Math.floor(exactSamples));
    spkSampleCarry = Math.max(0, exactSamples - sampleCount);
    const data = new Float32Array(sampleCount);

    let level = lastLevel, sampleIdx = 0;
    const cycles = edges.cycles, levels = edges.levels;
    // Effective rate for this chunk: 66 MHz, times the multiplier under fast-test.
    const cyclesPerRealSecond = cyclesThisFrame / dtSeconds;
    for (let i = 0; i < cycles.length; i++) {
      let edgeSample = Math.round(((cycles[i] - frameStartCycle) / cyclesPerRealSecond) * sampleRate);
      if (edgeSample < 0) edgeSample = 0;
      if (edgeSample > sampleCount) edgeSample = sampleCount;
      // +/-0.25 square wave; a held rail is DC that the worklet's DC blocker settles.
      const v = level ? 0.25 : -0.25;
      for (; sampleIdx < edgeSample; sampleIdx++) data[sampleIdx] = v;
      level = levels[i] !== 0;
    }
    const vTail = level ? 0.25 : -0.25;
    for (; sampleIdx < sampleCount; sampleIdx++) data[sampleIdx] = vTail;
    lastLevel = level;

    speakerNode.port.postMessage(data, [data.buffer]);
  }

  // Same cycle-to-time mapping as pumpAudio() for the SB16's samples, held from each position to the next.
  // The CT1745 attenuators only attenuate, and Master and per-source power on at 24 (-14 dB), ~28 dB
  // down. A real card makes that up in the output amp. Normalizing by the power-on product models
  // it: default settings play at full scale and sliders attenuate relative to that.
  const kMixerUnityGain = 0.2 * 0.2;

  // Sample-and-holds one cycle-stamped stream into left/right, scaled by its CT1745 attenuator, and
  // ADDS it (the card sums FM and digitized audio in the analog domain). Returns the last value so
  // the next frame resumes the hold.
  // Re-reads the four tone registers and touches an AudioParam only when one changed.
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
    // Drain all three even when muted so no log grows unbounded.
    const s = machine.sbDrainSamples();
    const fm = machine.fmDrainSamples();
    const cd = machine.cdromDrainSamples();
    if (!audioCtx || !sbNode || !speakerCheckbox.checked) return;
    refreshSbTone();
    // Hand the worklet the samples with the emulator's stamps and the CT1745 attenuators, nothing
    // else. Placing by performance.now() on the thread runCycles() blocks made FM scratchy under load;
    // the audio thread has an exact clock (kSbWorkletSrc).
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

  // Names for the opcode forms DOS games spend time in; unlisted ones show the raw byte.
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

  // ?perf loads the instrumented build, a separate binary with the emulator's counters (Tier 2).
  // Only one is fetched; if it is missing, fall back so the panel keeps its host-side half.
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
      // Check the instrumented build exists before committing, so only one module script is appended;
      // a second after a failed one leaves two half-initialized modules.
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

  // Per-second accumulators for the Performance panel; dbg.on is set only while it is open, so
  // measuring costs nothing otherwise. Also per-post audio pacing: the pump's cadence spread, not
  // just its average, matters when audio breaks up while the clock holds.
  const dbg = { on: false, emuMs: 0, renderMs: 0, frames: 0, dropped: 0, pumps: 0,
    posts: 0, postMsMax: 0, postMs: [], guestCyc: 0, wallSec: 0,
                posted: 0, dsp: 0, fm: 0 };

  // Trace of the last minute: filled area for the main thread's share of a core, line for the
  // emulated clock against 66 MHz. Same axis, so one chart shows "busy" and "keeping up" together.
  function drawPerfChart(c, canvas, history) {
    const w = canvas.width, h = canvas.height;
    c.clearRect(0, 0, w, h);
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

  // The host's side of the minute: share of one core and frame rate. Separate from the 486 chart;
  // a fullscreen stall shows here while the machine's chart barely moves. Sticky fps scale below.
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
    // fps gets its own scale off the fastest rate seen. It grows at once but shrinks only after a
    // while low; per-frame recomputation flipped between buckets as a spike aged out.
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

  // Ring depth over the last minute, with dry seconds marked under it. Says whether audio broke up
  // because the machine or the main thread fell behind; the clock chart can sit at 100% through a dropout.
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
    // Full height is 1.5x the cushion held, so the line has room above target.
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

    // A dry second gets a floor mark scaled by how long it was dry; any mark is an audible dropout.
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

  // ---- performance panel ----
  // Shown only when the wasm module reports a debug build (DEBUG_PERF=1). Output goes in the page,
  // not the console: DevTools and a log line a second spend the main-thread budget being measured.
  function startPerfPanel() {
    const card = document.getElementById("perfCard");
    const out = document.getElementById("perfReadout");
    card.hidden = false;
    dbg.on = true;
    // Tier 2 needs the instrumented binary. Say so on the page, or absent counters read as zeros.
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
    const history = [];
    let c0 = machine.totalCycles(), h0 = machine.haltCycles();
    let i0 = machine.idleCycles(), t0 = performance.now();
    // The panel opens before ROMs load and power-on, so the first sample spans idle time. Including it
    // in a cumulative average leaves it permanently wrong; a reading above 66 MHz is the giveaway.
    let first = true;
    const recent = [];
    // Discard what accumulated before the panel opened.
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
      if (secs <= 0 || cyc <= 0) return;
      const mhz = cyc / secs / 1e6;
      if (first) { first = false; return; }
      recent.push(mhz);
      if (recent.length > 30) recent.shift();
      const avg = recent.reduce((a, b) => a + b, 0) / recent.length;
      // Windowed like the average; a session-wide minimum just reports the boot or a level load.
      const worst = Math.min(...recent);
      const per = {};
      for (const pair of stats.split(" ")) {
        const [k, v] = pair.split("=");
        per[k] = Number(v) / secs;
      }
      const m = (n) => (n / 1e6).toFixed(2) + "M/s";
      // Ranked opcode counts: frequency, not time, since timing each instruction would cost more.
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
      // Pump cadence spread matters more than the mean: the ring only has to be empty once to be heard.
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

      // Ring numbers from the worklet, twice a second. A starved ring is "the sound got off": the
      // emulator can keep time while a main-thread stall empties the ring.
      let audio = "audio   off\n";
      if (audioCtx && sbNode && speakerCheckbox.checked) {
        const sr = audioCtx.sampleRate;
        const st = audioStats;
        const depthMs = st ? (st.depth / sr) * 1000 : 0;
        const starvedMs = st && st.secs ? (st.starved / sr) * 1000 / st.secs : 0;
        const trimMs = st && st.secs ? (st.trimmed / sr) * 1000 / st.secs : 0;
        const outLat = audioCtx.outputLatency || audioCtx.baseLatency || 0;
        // The instant reading can sit on target after swinging past it; show the range over the same 60s
        // window as the chart.
        const ringHist = history.map((p) => p.ring).concat(depthMs);
        const ringMin = Math.min(...ringHist), ringMax = Math.max(...ringHist);
        audio =
          "audio   ring " + depthMs.toFixed(0) + " ms of " +
          (st && st.targetMs ? st.targetMs.toFixed(0) : "--") + " target" +
          "   range " + ringMin.toFixed(0) + "-" + ringMax.toFixed(0) + " ms (60s)\n" +
          "        " + audioCtx.state + " " + (sr / 1000).toFixed(1) + " kHz\n" +
          "        starved " + starvedMs.toFixed(1) + " ms/s   trimmed " +
          trimMs.toFixed(1) + " ms/s   latency " + (outLat * 1000).toFixed(0) + " ms\n" +
          // `fed` is the card's samples at the card's rate, not the device rate; the audio thread resamples.
          "        fed " + (posted / 1000).toFixed(1) + "k/s   dsp " +
          (dspRate / 1000).toFixed(1) + "k/s   fm " + (fmRate / 1000).toFixed(1) + "k/s\n" +
          // Feed lateness: a late post is a hole the cushion must cover; p95/max show it.
          "  post  " + postsPerSec.toFixed(0) + "/s   p50 " + pct(0.5).toFixed(1) +
          " ms   p95 " + pct(0.95).toFixed(1) + " ms   max " + postMsMax.toFixed(1) + " ms\n" +
          // Guest time and FM audio produced per wall second. Below 1.00 the machine hasn't generated the
          // audio yet, which no buffering fixes.
          "  pace  guest " + guestPerWall.toFixed(3) + "x   fm " + fmPerWall.toFixed(3) +
          "x of real time\n";
      }
      // "CPU" is the share of ONE core of this page's main thread (the browser has no system-wide
      // figure); it also decides whether the pump drops. Clamped, since a long runCycles chunk
      // straddling the tick can attribute more than `secs`.
      const cpuPct = Math.min(100, busy / 10);
      const cores = navigator.hardwareConcurrency || 0;
      // Guest RAM is in the wasm heap, so its size is the memory footprint. performance.memory is Chrome-only.
      const heapMB = machine.heapBytes() / 1048576;
      const jsMem = performance.memory
        ? (performance.memory.usedJSHeapSize / 1048576).toFixed(0) + " / " +
          (performance.memory.jsHeapSizeLimit / 1048576).toFixed(0) + " MB JS heap"
        : "JS heap n/a";

      // Share of cycles spent working rather than halted. Bare DOS busy-waits at the prompt, so 100% is
      // honest; an idle driver (FDAPM, POWER) lowers it as on real hardware.
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

  // ---- main loop ----
  // pump() advances the machine in chunks bounded by real wall-clock time, yielding between them.
  // frame() (rAF) only draws. runCycles() is synchronous and blocks keydown, click and rAF, so one
  // frame-sized call made input latency equal to its duration and starved keystrokes when the host
  // couldn't quite hold 66 MHz (PC486_REVIEW.md §8).
  const ctx = screenEl.getContext("2d");
  const hddLed = document.getElementById("hddLed");
  let cycleCredit = 0, lastT = null;

  // Longest a runCycles() call may hold the main thread. Not a speed control: at 66 MHz a 60 Hz
  // frame needs ~15 ms, so 12 ms just splits a frame in two with a yield between.
  const kChunkMs = 12;
  // Host throughput in cycles per ms, turning kChunkMs into a cycle count. Seeded at 66 MHz, then
  // tracked with a slow EWMA.
  let cyclesPerMs = 66000;

  // A macrotask yield: a microtask would run the next chunk in the same turn and dispatch no input.
  // MessageChannel is the zero-delay macrotask (setTimeout(0) clamps to ~4 ms).
  const pumpChannel = new MessageChannel();
  let pumpScheduled = false;
  pumpChannel.port1.onmessage = () => { pumpScheduled = false; pump(); };
  function schedulePump() {
    if (pumpScheduled) return;
    pumpScheduled = true;
    pumpChannel.port2.postMessage(0);
  }

  // pump() reschedules via a zero-delay macrotask, so an idle CPU (HLT) can re-enter thousands of
  // times a second. Posting audio on every spin flooded the worklets with near-empty messages
  // (sampleCount floors at 1), and the resync trim couldn't keep the ring pointers from walking
  // into stale audio, heard as a sound looping after silence (PC486_REVIEW.md). Accumulate
  // cycles/dt and post once enough real time has passed; the normal ~12ms call flushes immediately.
  const kMinAudioPumpDt = 0.001;
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

  // The guest frame lands in frameCanvas at native resolution, is scaled by a whole factor per
  // axis (nearest) into #screen, then smooth-scaled to the CSS box, so guest pixels stay uniform.
  // The box is 4:3 in every mode: 320x200 and 720x400 stretch tall, as on a VGA monitor.
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

  function pump() {
    if (!poweredOn || !machine) return;
    const t = performance.now();
    if (lastT === null) lastT = t;
    let dtSeconds = (t - lastT) / 1000;
    lastT = t;
    dtSeconds = Math.min(dtSeconds, 0.25);  // clamp a backgrounded-tab gap -- no runaway catch-up burst

    // The DX2's real 66 MHz, Turbo on or off. TEST_CPU_MULTIPLIER is 1 outside `?test=1&fast=1`.
    cycleCredit += dtSeconds * cpuHz * TEST_CPU_MULTIPLIER;
    let cyclesThisChunk = Math.floor(cycleCredit);

    // Bound this call's wall-clock cost. Overage is dropped, not banked: a host that can't sustain
    // the rate would never work off a backlog, and each try is the input-starving call this bound
    // prevents. Emulated time runs slow on such a host, a departure from the real machine, but a 486
    // 10% slow beats one whose keyboard stops answering.
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
      // Only re-measure off a chunk long enough to time; sub-millisecond samples are noise.
      if (elapsed >= 1) cyclesPerMs += 0.25 * (cyclesThisChunk / elapsed - cyclesPerMs);
      if (dbg.on) { dbg.emuMs += elapsed; dbg.pumps++; }
    }

    pumpAudioCoalesced(chunkStartCycle, cyclesThisChunk, dtSeconds);
    schedulePump();
  }

  function frame() {
    if (!poweredOn || !machine) return;
    const frameT0 = dbg.on ? performance.now() : 0;
    const rgba = machine.renderFrame();
    // Resolution varies by mode (720x400 text up to 1024x768 SVGA, ega_render.h); the 4:3 box stays put.
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

  // ---- hard disk persistence (IndexedDB, hdd-worker.js) ----
  // The Machine is discarded on power-off, so C: is mirrored to IndexedDB. The worker owns that
  // database so saves, loads and the 504MB conversion stay off this thread.
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

  // ---- power switch ----
  // Power-off discards the whole Machine (RAM gone, like unplugging) while a diskette or CD stays
  // seated: power-on builds a fresh Machine and remounts the images remembered in JS. The tower has
  // a momentary Reset button (resetBtn), which the genuine 5170 lacked.
  const powerSwitch = document.getElementById("powerSwitch");
  const powerLed = document.getElementById("powerLed");
  const resetBtn = document.getElementById("resetBtn");
  const turboBtn = document.getElementById("turboBtn");
  const turboLed = document.getElementById("turboLed");
  // Turbo off makes the SiS 471 hold the CPU off the bus 4us of every 12us (Machine::set_turbo);
  // the clock stays 66 MHz. The readout shows 66 or 33, as a tower's jumpers set it. Default on.
  const cpuHz = 66000000;
  // Segment maps for the digits shown (3 and 6).
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
  // Turbo LED and clock need power; the button still latches while off, like a real switch.
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
    if (machine) machine.setTurbo(on);
    syncTurboChrome();
  }
  turboBtn.addEventListener("click", () => {
    applyTurbo(turboBtn.getAttribute("aria-pressed") !== "true");
  });
  let poweredOn = false;
  syncTurboChrome();
  let firmware = null;  // {Module, bios, vga, hdd} once fetched
  // WD Caviar AC2250: 1010 cyl x 9 head x 55 sec x 512 bytes (wd1003.h), and the older
  // 1024/16/63 drive that hdd-convert.js converts.
  const kHddImageBytes = 255974400;
  const kLegacyHddImageBytes = 528482304;
  let pendingFloppy = null;
  let pendingCdrom = null;  // {name, bytes} for a plain ISO, or {name, cueText, bin} for CUE+BIN

  resetBtn.addEventListener("click", () => {
    // RESET pulses the CPU/chipset only; RAM and CMOS survive. No-op while powered off.
    if (machine) machine.reset();
  });

  // What C: mounts next power-on. `pendingHdd` is an image already read at page load; otherwise
  // the worker is asked, and with nothing saved the factory image is mounted and handed to it.
  let pendingHdd = null;
  let hddLabel = "FreeDOS (default)";
  let legacyHddNote = null;
  const kLegacyHddNote = "Your old 504MB C: couldn't be converted to 256MB. Download it before you reset.";
  let legacyConvertFailed = false;
  // Fetched only when a session needs the pristine factory image (nothing saved, or Reset to factory).
  let factoryHddPromise = null;
  function factoryHddUrl() {
    return new URL("disks/freedos-hdd.img", location.href).href;
  }
  // Cheap identity of the server's freedos-hdd.img (ETag, else Last-Modified, plus Content-Length)
  // to tell an untouched older C: from a current one without the body. One HEAD per page load.
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
          // Offline or server down: fail open, a stale image beats a machine that won't boot.
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

  // The saved C: as {bytes, status, modified}, or null to use the factory image. An untouched C:
  // seeded from an older freedos-hdd.img is dropped.
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
    // A fixed disk can't be swapped while running; these only affect the next power-on.
    hddResetBtn.disabled = !firmware || poweredOn;
    hddBlankBtn.disabled = !firmware || poweredOn;
    hddDownloadBtn.disabled = !firmware;
    hddUploadInput.disabled = !firmware || poweredOn;
    document.getElementById("hddUploadBtn").disabled = !firmware || poweredOn;
  }
  hddResetBtn.addEventListener("click", () => {
    pendingHdd = null;
    legacyHddNote = null;
    hddLabel = "FreeDOS (default). Takes effect at next power-on";
    refreshHddControls();
    // Reset to factory means the image the server has now, so drop any fetched copy.
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
    // Zeroed, like a drive fresh from the factory.
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
  // A real file on the visitor's disk, like the floppy eject flow's "save modified media". C: isn't
  // ejectable, so it needs its own control.
  hddDownloadBtn.addEventListener("click", async () => {
    if (!firmware) return;
    // The live image if running, else what's staged for next power-on, else the factory image.
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
      // The AC2250 geometry is fixed in CMOS, not derived from the image. A wrong size would fail
      // (wd1003.cpp reports IDNF) but refusing up front gives a clearer reason.
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
      // Free the previous machine first: an embind handle owns a C++ object that outlives the JS
      // reference, so dropping `machine` at powerOff() leaked a C:-sized heap block per cycle (plus 419MB
      // with a CD loaded) until the second power-on died. Deleted here, not in powerOff(), so the
      // handle stays callable while off (tests/boot.spec.ts).
      if (lastMachine) {
        lastMachine.delete();
        lastMachine = null;
        // Power-on waits on the hard disk worker, so tests must not see the machine just deleted.
        if (window.__test) window.__test.machine = null;
      }
      machine = new firmware.Module.Machine();
      applyTurbo(turboBtn.getAttribute("aria-pressed") === "true");
      if (perfRequested) startPerfPanel();
      machine.loadRom(0x100000 - firmware.bios.byteLength, new Uint8Array(firmware.bios));
      machine.loadRom(0xC0000, new Uint8Array(firmware.vga));
      // Factory image is fetched on demand; a session with C: in IndexedDB never downloads it.
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
        // The worker's copy is C: from here, so drop the factory bytes.
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
      // Empty by default (see the CD-ROM section). `pendingCdrom.bytes` must already be a Uint8Array:
      // mountCdrom() goes through embind's convertJSArrayToNumberVector, which reads `.length`, and a
      // bare ArrayBuffer only has `byteLength`, giving an empty vector and no disc, silently.
      if (pendingCdrom) {
        if (pendingCdrom.cueText) {
          machine.mountCdromCue(pendingCdrom.cueText, pendingCdrom.bin);
        } else {
          machine.mountCdrom(pendingCdrom.bytes);
        }
        setBayLoaded(cdromBay, pendingCdrom.name);
      }
      // The battery-backed clock kept local time, so a fresh power-on reads the visitor's own clock.
      const now = new Date();
      machine.setRtc(now.getFullYear(), now.getMonth() + 1, now.getDate(),
        now.getHours(), now.getMinutes(), now.getSeconds(), now.getDay() + 1);
    }
    poweredOn = true;
    powerLed.classList.add("power-on");
    syncTurboChrome();
    lastT = null;
    requestAnimationFrame(frame);
    schedulePump();
    refreshHddControls();
    refreshFkeyControls();
    updateFocusHint();
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
        // Ring depth in ms as last reported by the worklet (twice a second); null until the first report.
        get audioStatsRaw() { return audioStats; },
        get hasSbCapture() { return sbCapture !== null; },
        startSbCapture: (n) => { sbCapture = null; sbNode.port.postMessage({ startCapture: n || 48000 }); },
        takeSbCapture: () => { const c = sbCapture; sbCapture = null; return c; },
        // Guest cycle each sample of the last capture played.
        get sbCaptureCyc() { return sbCaptureCyc; },
        get audioSampleRate() { return audioCtx ? audioCtx.sampleRate : null; },
        get sbRingMs() {
          return audioStats && audioCtx ? (audioStats.depth / audioCtx.sampleRate) * 1000 : null;
        },
        // Tone-shelf filter gains in dB; null until audio has started and a frame refreshed them.
        sbToneGains: () => sbTrebleLeft ? {
          trebleLeft: sbTrebleLeft.gain.value, trebleRight: sbTrebleRight.gain.value,
          bassLeft: sbBassLeft.gain.value, bassRight: sbBassRight.gain.value,
        } : null,
        get heldKeysSize() { return heldKeys.size; },
        // Flush C: to IndexedDB and resolve when the worker's write finishes; tests that reload must
        // await it or the save races the navigation.
        persistHdd: () => persistHddIfDirty(),
        // Mark C: the visitor's own even when clean; a fast boot may never dirty the image.
        forcePersistHdd: () => persistHddSnapshot(),
        whenHddSaved: () => hddPersistChain,
      };
    }
  }

  // Mirrors C: to IndexedDB only if written since the last mirror, periodically while running and
  // at powerOff(). Real disks need no save step; this exists because C: lives in a JS Uint8Array
  // that dies with the tab. Returns a promise that settles when the worker write completes (or
  // at once if clean); callers about to tear the page down must await it.
  let hddPersistChain = Promise.resolve();
  function queueHddSave(m, patches) {
    hddLabel = "saved (this session)";
    refreshHddControls();
    const transfer = patches.map((p) => p.bytes.buffer);
    hddPersistChain = hddPersistChain.then(async () => {
      const r = await hddWorker.call("patch", { patches }, transfer);
      // Nothing stored to patch yet: store the whole image.
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
    // Only the sectors written this session (wd1003.h dirty_ranges()).
    const patches = machine.hddDirtyPatches();
    machine.clearHddDirty();
    return queueHddSave(machine, patches);
  }
  // Test helper: mark C: saved even when hddDirty() is false (a fast boot often never dirties it).
  function persistHddSnapshot() {
    if (!machine) return hddPersistChain;
    if (machine.hddDirty()) return persistHddIfDirty();
    return queueHddSave(machine, []);
  }

  async function powerOff() {
    if (!poweredOn) return;
    await persistHddIfDirty();
    poweredOn = false;
    lastMachine = machine;  // freed at the next powerOn()
    machine = null;  // RAM is gone when power is cut
    powerLed.classList.remove("power-on");
    syncTurboChrome();
    floppyBay.querySelector('[data-role="led"]').classList.remove("on");
    cdromBay.querySelector('[data-role="led"]').classList.remove("on");
    hddLed.classList.remove("on");
    clearScreenToBlack();
    if (audioCtx) { audioCtx.suspend().catch(() => {}); }
    refreshHddControls();
    refreshFkeyControls();
    updateFocusHint();
    hideBootNotice();
  }

  // ---- function/extended-key panel ----
  // F-keys and the extended block for anyone lacking the key (a Mac has no Insert, PrintScreen,
  // ScrollLock or Pause). Works only while running. #bezel's escBtn shares the tap logic because
  // the browser never delivers Esc to the page in fullscreen, so injecting the scancode from a
  // click bypasses the native key event.
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
      // A make+break in one JS turn never lets the rAF-paced instruction loop run between them, so
      // the guest can miss the make. 50ms mirrors a fast real keystroke.
      setTimeout(() => sendKey(key, true), 50);
    });
  }
  ctrlAltDelBtn.addEventListener('click', () => {
    if (!machine) return;
    // The warm-boot combo: Ctrl, Alt, then the original non-extended Del (0x53, the 84-key numpad
    // Del) that the Bochs-legacy BIOS checks for, not SET1.Delete (0xE0 0x53). Gaps matter: back to
    // back makes clobbered all but Del (IBM_PCAT_REVIEW §31). Under FreeDOS+JEMMEX (V86) the BIOS
    // INT 9 check has no effect, so the sequence is followed by a CPU+chipset reset.
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

  powerSwitch.checked = false;
  powerSwitch.disabled = true;  // enabled once firmware loads
  clearScreenToBlack();
  refreshFkeyControls();
  powerSwitch.addEventListener("change", () => {
    if (powerSwitch.checked) powerOn();
    else void powerOff();
  });

  // Autosave C: every 5s while running, not only at power-off: the machine boots itself and most
  // visitors never switch it off before closing the tab, which once lost a whole game install.
  // Idle sessions (hddDirty() false) do nothing.
  setInterval(() => { void persistHddIfDirty(); }, 5000);

  // Last defense for the gap since the last tick: ask first, like an "unsaved changes" prompt. Some
  // gesture navigations (trackpad swipe) bypass beforeunload; "Download image" is the backup.
  window.addEventListener("beforeunload", (e) => {
    if (poweredOn && machine && machine.hddDirty()) {
      e.preventDefault();
      e.returnValue = "";
    }
  });

  // ---- fetch firmware once, up front ----
  // Web delivery only, so it isn't gated by the power switch. The factory HDD image is left out:
  // someone with C: in IndexedDB never needs it, and ensureFactoryHdd() fetches on demand. The
  // ~400MB install CD stays out for the same reason.
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
      // Boot straight to a running machine once firmware is ready.
      powerSwitch.checked = true;
      await powerOn();
    } finally {
      endLoad();
    }
  })().catch((err) => {
    // No on-page error surface: the power switch never enables; the detail goes to the console.
    console.error(err);
  });
})();
