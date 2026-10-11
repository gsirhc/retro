"use strict";
// IBM Enhanced Keyboard (101-key, US) drawn to scale, plus the browser-edge Key Mapper.

// [code, legend, x, y, w, h, dark]. Units are key pitches (u). legend "a|b" is shifted over unshifted.
const PC_KEYBOARD_LAYOUT = (() => {
  const keys = [];
  const k = (code, legend, x, y, w = 1, h = 1, dark = false) => keys.push({ code, legend, x, y, w, h, dark });
  k("Escape", "Esc", 0, 0, 1, 1, true);
  ["F1", "F2", "F3", "F4"].forEach((c, i) => k(c, c, 2 + i, 0, 1, 1, true));
  ["F5", "F6", "F7", "F8"].forEach((c, i) => k(c, c, 6.5 + i, 0, 1, 1, true));
  ["F9", "F10", "F11", "F12"].forEach((c, i) => k(c, c, 11 + i, 0, 1, 1, true));
  k("PrintScreen", "Print Screen|SysRq", 15.25, 0, 1, 1, true);
  k("ScrollLock", "Scroll Lock", 16.25, 0, 1, 1, true);
  k("Pause", "Pause|Break", 17.25, 0, 1, 1, true);

  let y = 1.5;
  const row1 = [["Backquote", "~|`"], ["Digit1", "!|1"], ["Digit2", "@|2"], ["Digit3", "#|3"], ["Digit4", "$|4"],
    ["Digit5", "%|5"], ["Digit6", "^|6"], ["Digit7", "&|7"], ["Digit8", "*|8"], ["Digit9", "(|9"],
    ["Digit0", ")|0"], ["Minus", "_|-"], ["Equal", "+|="]];
  row1.forEach(([c, l], i) => k(c, l, i, y));
  k("Backspace", "←", 13, y, 2, 1, true);

  y = 2.5;
  k("Tab", "⇤|⇥", 0, y, 1.5, 1, true);
  "QWERTYUIOP".split("").forEach((ch, i) => k("Key" + ch, ch, 1.5 + i, y));
  k("BracketLeft", "{|[", 11.5, y);
  k("BracketRight", "}|]", 12.5, y);
  k("Backslash", "¦|\\", 13.5, y, 1.5);

  y = 3.5;
  k("CapsLock", "Caps Lock", 0, y, 1.75, 1, true);
  "ASDFGHJKL".split("").forEach((ch, i) => k("Key" + ch, ch, 1.75 + i, y));
  k("Semicolon", ":|;", 10.75, y);
  k("Quote", "\"|'", 11.75, y);
  k("Enter", "Enter ↵", 12.75, y, 2.25, 1, true);

  y = 4.5;
  k("ShiftLeft", "⇧ Shift", 0, y, 2.25, 1, true);
  "ZXCVBNM".split("").forEach((ch, i) => k("Key" + ch, ch, 2.25 + i, y));
  k("Comma", "<|,", 9.25, y);
  k("Period", ">|.", 10.25, y);
  k("Slash", "?|/", 11.25, y);
  k("ShiftRight", "⇧ Shift", 12.25, y, 2.75, 1, true);

  y = 5.5;
  k("ControlLeft", "Ctrl", 0, y, 1.5, 1, true);
  k("AltLeft", "Alt", 2.5, y, 1.5, 1, true);
  k("Space", "", 4, y, 7);
  k("AltRight", "Alt", 11, y, 1.5, 1, true);
  k("ControlRight", "Ctrl", 13.5, y, 1.5, 1, true);

  k("Insert", "Insert", 15.25, 1.5, 1, 1, true);
  k("Home", "Home", 16.25, 1.5, 1, 1, true);
  k("PageUp", "Page Up", 17.25, 1.5, 1, 1, true);
  k("Delete", "Delete", 15.25, 2.5, 1, 1, true);
  k("End", "End", 16.25, 2.5, 1, 1, true);
  k("PageDown", "Page Down", 17.25, 2.5, 1, 1, true);
  k("ArrowUp", "↑", 16.25, 4.5, 1, 1, true);
  k("ArrowLeft", "←", 15.25, 5.5, 1, 1, true);
  k("ArrowDown", "↓", 16.25, 5.5, 1, 1, true);
  k("ArrowRight", "→", 17.25, 5.5, 1, 1, true);

  const nx = 18.5;
  k("NumLock", "Num Lock", nx, 1.5, 1, 1, true);
  k("NumpadDivide", "/", nx + 1, 1.5, 1, 1, true);
  k("NumpadMultiply", "*", nx + 2, 1.5, 1, 1, true);
  k("NumpadSubtract", "-", nx + 3, 1.5, 1, 1, true);
  k("Numpad7", "7|Home", nx, 2.5);
  k("Numpad8", "8|↑", nx + 1, 2.5);
  k("Numpad9", "9|PgUp", nx + 2, 2.5);
  k("NumpadAdd", "+", nx + 3, 2.5, 1, 2, true);
  k("Numpad4", "4|←", nx, 3.5);
  k("Numpad5", "5", nx + 1, 3.5);
  k("Numpad6", "6|→", nx + 2, 3.5);
  k("Numpad1", "1|End", nx, 4.5);
  k("Numpad2", "2|↓", nx + 1, 4.5);
  k("Numpad3", "3|PgDn", nx + 2, 4.5);
  k("NumpadEnter", "Enter", nx + 3, 4.5, 1, 2, true);
  k("Numpad0", "0|Ins", nx, 5.5, 2);
  k("NumpadDecimal", ".|Del", nx + 2, 5.5);
  return { keys, width: nx + 4, height: 6.5 };
})();

const PC_KEYBOARD_MODIFIERS = new Set([
  "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft", "AltRight",
]);

function pcKeyboardIsTouchOnly() {
  try {
    return matchMedia("(pointer: coarse)").matches && !matchMedia("(any-pointer: fine)").matches;
  } catch {
    /* v8 ignore next -- only if matchMedia itself is missing */
    return false;
  }
}

function initPcKeyboard(opts) {
  const { root, sendKey, keymapStorageKey, openStateKey, bezelKeysBtn } = opts;
  const isEnabled = opts.isEnabled || (() => true);
  const readLeds = opts.readLeds || (() => 0);

  // ---- guest key routing ----
  // Guest keys are refcounted so a shared chord modifier (WASD's Alt strafe) stays down while any
  // source is held. heldKeys releases anything still down when focus leaves.
  const heldKeys = new Set();
  const physicalHeld = new Map();
  const guestKeyRefs = new Map();
  const keyEls = new Map();
  function paintDown(code, on) {
    const el = keyEls.get(code);
    if (el) el.classList.toggle("down", on);
  }
  function pressGuestKey(code) {
    const n = (guestKeyRefs.get(code) || 0) + 1;
    guestKeyRefs.set(code, n);
    if (n === 1) {
      heldKeys.add(code);
      paintDown(code, true);
      sendKey(code, false);
    }
  }
  function releaseGuestKey(code) {
    const n = (guestKeyRefs.get(code) || 0) - 1;
    if (n <= 0) {
      guestKeyRefs.delete(code);
      if (heldKeys.has(code)) {
        heldKeys.delete(code);
        paintDown(code, false);
        sendKey(code, true);
      }
    } else {
      guestKeyRefs.set(code, n);
    }
  }
  function releaseAll() {
    for (const code of heldKeys) {
      paintDown(code, false);
      sendKey(code, true);
    }
    heldKeys.clear();
    physicalHeld.clear();
    guestKeyRefs.clear();
    latched.clear();
    paintLatches();
  }

  // ---- Key Mapper ----
  // Remaps keys at the browser edge, typed or clicked; the guest still sees Set 1. Doom 1.2 era
  // games predate WASD, so the preset sends A/D as Alt+arrow to strafe.
  const kWasdPresetRows = [
    { from: "KeyW", to: ["ArrowUp"], preset: "wasd" },
    { from: "KeyS", to: ["ArrowDown"], preset: "wasd" },
    { from: "KeyA", to: ["AltLeft", "ArrowLeft"], preset: "wasd" },
    { from: "KeyD", to: ["AltLeft", "ArrowRight"], preset: "wasd" },
  ];
  const kShiftCtrlPresetRows = [
    { from: "ShiftLeft", to: ["ControlLeft"], preset: "shiftCtrl" },
  ];
  const kHostModifiers = new Set([...PC_KEYBOARD_MODIFIERS, "MetaLeft", "MetaRight"]);
  let keymapRows = [];
  let keymapNextId = 1;

  function mapKey(code) {
    for (const row of keymapRows) {
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
      Escape: "Esc", CapsLock: "Caps Lock", PageUp: "Page Up", PageDown: "Page Down",
      PrintScreen: "Print Screen", ScrollLock: "Scroll Lock", NumLock: "Num Lock",
    };
    if (labels[code]) return labels[code];
    if (/^Key[A-Z]$/.test(code)) return code.slice(3);
    if (/^Digit\d$/.test(code)) return code.slice(5);
    if (/^Numpad/.test(code)) return "Keypad " + code.slice(6);
    return code;
  }
  function labelKeyChord(codes) {
    return codes.map(labelKeyCode).join("+");
  }
  // Short badge text for the drawn key a mapping starts from.
  function badgeText(codes) {
    const short = { ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→",
      ShiftLeft: "Sh", ShiftRight: "Sh", ControlLeft: "Ctl", ControlRight: "Ctl", AltLeft: "Alt", AltRight: "Alt" };
    return codes.map((c) => short[c] || labelKeyCode(c)).join("+");
  }

  function persistKeymap() {
    try {
      localStorage.setItem(keymapStorageKey, JSON.stringify(keymapRows.map((r) => ({
        id: r.id, from: r.from, to: r.to, enabled: r.enabled, preset: r.preset || undefined,
      }))));
    } catch {}
  }
  function loadKeymap() {
    try {
      const raw = localStorage.getItem(keymapStorageKey);
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
  function keymapChanged() {
    releaseAll();
    persistKeymap();
    renderMappings();
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
    keymapChanged();
  }
  function setAllMappingsEnabled(on) {
    if (keymapRows.length === 0) return;
    for (const row of keymapRows) row.enabled = !!on;
    keymapChanged();
  }
  function clearKeymap() {
    keymapRows = [];
    keymapChanged();
  }

  // ---- DOM ----
  const bar = document.createElement("div");
  bar.className = "kbd-bar";
  const mkBtn = (id, text, title) => {
    const b = document.createElement("button");
    b.type = "button";
    b.id = id;
    b.textContent = text;
    if (title) b.title = title;
    bar.appendChild(b);
    return b;
  };
  const mapBtn = mkBtn("keymapAddBtn", "Map a key", "Make one of your keys send a different key");
  mapBtn.setAttribute("aria-pressed", "false");
  const wasdBtn = mkBtn("keymapPresetWasd", "WASD → Arrows");
  const shiftCtrlBtn = mkBtn("keymapPresetShiftCtrl", "Left Shift → Ctrl");
  const toggleAllBtn = mkBtn("keymapToggleAll", "Disable All");
  const clearBtn = mkBtn("keymapClearBtn", "Clear All");
  const status = document.createElement("span");
  status.className = "muted kbd-status";
  status.id = "keymapCaptureStatus";
  status.setAttribute("aria-live", "polite");

  const wrap = document.createElement("div");
  wrap.className = "kbd-wrap";
  const board = document.createElement("div");
  board.className = "kbd";
  board.id = "pcKeyboard";
  board.setAttribute("role", "group");
  board.setAttribute("aria-label", "IBM Enhanced Keyboard");
  board.style.setProperty("--kbd-w", String(PC_KEYBOARD_LAYOUT.width));
  board.style.setProperty("--kbd-h", String(PC_KEYBOARD_LAYOUT.height));
  const deck = document.createElement("div");
  deck.className = "kbd-deck";
  board.appendChild(deck);

  const ledEls = [];
  const ledBox = document.createElement("div");
  ledBox.className = "kbd-leds";
  // 0xED bit order: 0 Scroll Lock, 1 Num Lock, 2 Caps Lock. Drawn left to right as on the case.
  for (const [bit, name] of [[1, "Num Lock"], [2, "Caps Lock"], [0, "Scroll Lock"]]) {
    const cell = document.createElement("span");
    cell.className = "kbd-led-cell";
    const lamp = document.createElement("span");
    lamp.className = "kbd-led";
    lamp.dataset.led = String(bit);
    const lbl = document.createElement("span");
    lbl.className = "kbd-led-label";
    lbl.textContent = name;
    cell.append(lamp, lbl);
    ledBox.appendChild(cell);
    ledEls.push({ bit, lamp });
  }
  deck.appendChild(ledBox);

  for (const key of PC_KEYBOARD_LAYOUT.keys) {
    const b = document.createElement("button");
    b.type = "button";
    b.tabIndex = -1;
    b.className = "kbd-key" + (key.dark ? " dark" : "") + (key.code === "Space" ? " space" : "");
    b.dataset.key = key.code;
    b.style.setProperty("--x", String(key.x));
    b.style.setProperty("--y", String(key.y));
    b.style.setProperty("--w", String(key.w));
    b.style.setProperty("--h", String(key.h));
    b.setAttribute("aria-label", key.code === "Space" ? "Space" : labelKeyCode(key.code));
    const cap = document.createElement("span");
    cap.className = "kbd-cap";
    const parts = key.legend.split("|");
    if (parts.length === 2) {
      const top = document.createElement("span");
      top.textContent = parts[0];
      const bot = document.createElement("span");
      bot.textContent = parts[1];
      cap.append(top, bot);
      cap.classList.add("two");
    } else {
      cap.textContent = parts[0];
    }
    if (parts.length === 1 && key.legend.length >= 3 && !/^F\d+$/.test(key.legend)) cap.classList.add("word");
    if (parts.length === 2 && (parts[0].length > 1 || parts[1].length > 1)) cap.classList.add("word");
    b.appendChild(cap);
    const tag = document.createElement("span");
    tag.className = "kbd-map";
    tag.hidden = true;
    b.appendChild(tag);
    deck.appendChild(b);
    keyEls.set(key.code, b);
  }
  wrap.appendChild(board);
  root.append(wrap, bar, status);

  // ---- latched modifiers ----
  // Drawn key -> guest codes it holds. A key latches when everything it sends is a modifier.
  const latched = new Map();
  function paintLatches() {
    for (const [code, el] of keyEls) el.classList.toggle("latched", latched.has(code));
  }
  function releaseLatches() {
    for (const codes of latched.values()) {
      for (let i = codes.length - 1; i >= 0; i--) releaseGuestKey(codes[i]);
    }
    latched.clear();
    paintLatches();
  }

  // ---- map mode ----
  // "from": waiting for the source key (physical press or a click). "to": waiting for the target.
  let capturePhase = null;
  let captureFrom = null;
  let capturePendingModifier = null;
  const mapLatched = new Set();
  function setStatus(text) {
    status.textContent = text || "";
  }
  function syncMapBtn() {
    mapBtn.setAttribute("aria-pressed", capturePhase ? "true" : "false");
    board.classList.toggle("mapping", !!capturePhase);
    if (!capturePhase) mapBtn.textContent = "Map a key";
    else if (capturePhase === "to" && mapLatched.size > 0) mapBtn.textContent = "Done";
    else mapBtn.textContent = "Cancel";
    for (const code of PC_KEYBOARD_MODIFIERS) {
      const el = keyEls.get(code);
      if (el) el.classList.toggle("map-latched", mapLatched.has(code));
    }
    for (const [code, el] of keyEls) el.classList.toggle("map-from", capturePhase === "to" && code === captureFrom);
    paintDisabled();
  }
  function cancelCapture() {
    capturePhase = null;
    captureFrom = null;
    capturePendingModifier = null;
    mapLatched.clear();
    setStatus("");
    syncMapBtn();
  }
  function startCapture() {
    releaseAll();
    capturePhase = "from";
    captureFrom = null;
    capturePendingModifier = null;
    mapLatched.clear();
    setStatus("Press the key on your keyboard you want to change, or click it below. Esc cancels.");
    syncMapBtn();
  }
  function chooseFrom(code) {
    captureFrom = code;
    capturePhase = "to";
    capturePendingModifier = null;
    const mapped = keymapRows.some((r) => r.from === code);
    setStatus(`Your ${labelKeyCode(code)} key. Now click the key it should send. Click Shift, Ctrl or Alt first for a combination.` +
      (mapped ? " Click it again to clear its mapping." : ""));
    syncMapBtn();
  }
  function commitMapping(to) {
    if (to.length === 1 && to[0] === captureFrom) {
      keymapRows = keymapRows.filter((r) => r.from !== captureFrom);
      cancelCapture();
      keymapChanged();
      return;
    }
    upsertMapping(captureFrom, to, undefined);
    cancelCapture();
    keymapChanged();
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
      chooseFrom(e.code);
      return;
    }
    // Bare modifiers are valid targets (Left Shift -> Ctrl); defer to keyup so Alt+Arrow can chord.
    if (kHostModifiers.has(e.code)) {
      capturePendingModifier = e.code;
      return;
    }
    capturePendingModifier = null;
    commitMapping(chordFromEvent(e));
  }
  function onCaptureKeyUp(e) {
    if (!capturePhase) return;
    e.preventDefault();
    e.stopPropagation();
    if (capturePhase === "to" && capturePendingModifier && e.code === capturePendingModifier) {
      const to = [capturePendingModifier];
      capturePendingModifier = null;
      commitMapping(to);
    }
  }
  document.addEventListener("keydown", onCaptureKeyDown, true);
  document.addEventListener("keyup", onCaptureKeyUp, true);

  function mapModeClick(code) {
    if (capturePhase === "from") {
      chooseFrom(code);
      return;
    }
    if (PC_KEYBOARD_MODIFIERS.has(code)) {
      if (mapLatched.has(code)) mapLatched.delete(code);
      else mapLatched.add(code);
      syncMapBtn();
      return;
    }
    commitMapping([...mapLatched, code]);
  }

  mapBtn.addEventListener("click", () => {
    if (!capturePhase) {
      startCapture();
      return;
    }
    if (capturePhase === "to" && mapLatched.size > 0) {
      commitMapping([...mapLatched]);
      return;
    }
    cancelCapture();
  });
  wasdBtn.addEventListener("click", () => {
    cancelCapture();
    applyPresetRows(kWasdPresetRows);
  });
  shiftCtrlBtn.addEventListener("click", () => {
    cancelCapture();
    applyPresetRows(kShiftCtrlPresetRows);
  });
  toggleAllBtn.addEventListener("click", () => {
    cancelCapture();
    setAllMappingsEnabled(!anyMappingEnabled());
  });
  clearBtn.addEventListener("click", () => {
    cancelCapture();
    clearKeymap();
  });
  if (bezelKeysBtn) {
    bezelKeysBtn.addEventListener("click", () => setAllMappingsEnabled(!anyMappingEnabled()));
  }

  function renderMappings() {
    for (const [code, el] of keyEls) {
      const tag = el.querySelector(".kbd-map");
      const row = keymapRows.find((r) => r.from === code);
      el.classList.toggle("mapped", !!row);
      el.classList.toggle("mapped-off", !!row && !row.enabled);
      if (!row) {
        tag.hidden = true;
        tag.textContent = "";
        el.removeAttribute("title");
        continue;
      }
      tag.hidden = false;
      const name = labelKeyCode(code);
      const txt = document.createElement("span");
      txt.className = "kbd-map-txt";
      txt.dataset.action = "toggle";
      txt.setAttribute("role", "button");
      txt.setAttribute("aria-pressed", row.enabled ? "true" : "false");
      txt.setAttribute("aria-label", `${name} mapping ${row.enabled ? "on" : "off"}`);
      txt.textContent = "→" + badgeText(row.to);
      const del = document.createElement("span");
      del.className = "kbd-map-del";
      del.dataset.action = "delete";
      del.setAttribute("role", "button");
      del.setAttribute("aria-label", `Delete ${name} mapping`);
      del.textContent = "×";
      tag.replaceChildren(txt, del);
      el.title = `Your ${name} key sends ${labelKeyChord(row.to)}` + (row.enabled ? "" : " (off)");
    }
    const has = keymapRows.length > 0;
    const on = anyMappingEnabled();
    toggleAllBtn.disabled = !has;
    toggleAllBtn.textContent = on ? "Disable All" : "Enable All";
    clearBtn.disabled = !has;
    if (bezelKeysBtn) {
      bezelKeysBtn.disabled = !has;
      bezelKeysBtn.setAttribute("aria-pressed", on ? "true" : "false");
    }
  }

  // ---- drawn key presses ----
  // pointerdown's preventDefault keeps focus where it was, so typing on the real keyboard carries on.
  const pointerKeys = new Map();
  deck.addEventListener("pointerdown", (e) => {
    const el = e.target.closest(".kbd-key");
    if (!el || (e.button != null && e.button !== 0)) return;
    e.preventDefault();
    const code = el.dataset.key;
    // The tag on a mapped key switches that one mapping off or on; its × deletes it.
    const action = e.target.closest("[data-action]");
    if (action) {
      const row = keymapRows.find((r) => r.from === code);
      if (action.dataset.action === "delete") keymapRows = keymapRows.filter((r) => r !== row);
      else row.enabled = !row.enabled;
      keymapChanged();
      return;
    }
    if (capturePhase) {
      mapModeClick(code);
      return;
    }
    if (!isEnabled()) return;
    if (latched.has(code)) {
      const held = latched.get(code);
      latched.delete(code);
      for (let i = held.length - 1; i >= 0; i--) releaseGuestKey(held[i]);
      paintLatches();
      return;
    }
    const codes = mapKey(code);
    if (codes.every((c) => PC_KEYBOARD_MODIFIERS.has(c))) {
      latched.set(code, codes);
      for (const c of codes) pressGuestKey(c);
      paintLatches();
      return;
    }
    try { el.setPointerCapture(e.pointerId); } catch {}
    pointerKeys.set(e.pointerId, codes);
    for (const c of codes) pressGuestKey(c);
  });
  function endPointer(e) {
    const codes = pointerKeys.get(e.pointerId);
    if (codes === undefined) return;
    pointerKeys.delete(e.pointerId);
    for (let i = codes.length - 1; i >= 0; i--) releaseGuestKey(codes[i]);
    releaseLatches();
  }
  deck.addEventListener("pointerup", endPointer);
  deck.addEventListener("pointercancel", endPointer);
  deck.addEventListener("contextmenu", (e) => e.preventDefault());

  // ---- physical keys (wired to the screen by the page) ----
  function keyDown(e) {
    if (capturePhase) return;
    e.preventDefault();
    if (e.repeat || physicalHeld.has(e.code)) return;
    const codes = mapKey(e.code);
    physicalHeld.set(e.code, codes);
    for (const code of codes) pressGuestKey(code);
  }
  function keyUp(e) {
    if (capturePhase) return;
    e.preventDefault();
    const codes = physicalHeld.get(e.code) || mapKey(e.code);
    physicalHeld.delete(e.code);
    for (let i = codes.length - 1; i >= 0; i--) releaseGuestKey(codes[i]);
  }

  // ---- power and lock lights ----
  let poweredLive = false;
  // Map mode works with the power off, so the keys stay actionable while it runs.
  function paintDisabled() {
    const live = poweredLive || !!capturePhase;
    for (const el of keyEls.values()) el.setAttribute("aria-disabled", live ? "false" : "true");
  }
  function refresh() {
    const on = isEnabled();
    poweredLive = on;
    board.classList.toggle("off", !on);
    paintDisabled();
    if (!on) {
      releaseAll();
      paintLeds(0);
    }
  }
  function paintLeds(bits) {
    for (const { bit, lamp } of ledEls) lamp.classList.toggle("on", !!(bits & (1 << bit)));
  }
  setInterval(() => {
    if (isEnabled()) paintLeds(readLeds() | 0);
  }, 100);

  // ---- show/hide ----
  // Shown by default on touch screens, which have no keyboard of their own. Remembered once chosen.
  const toggle = document.getElementById(opts.toggleId);
  const card = document.getElementById(opts.cardId);
  function applyOpen(open) {
    toggle.checked = open;
    card.hidden = !open;
  }
  let open = pcKeyboardIsTouchOnly();
  try {
    const saved = localStorage.getItem(openStateKey);
    if (saved === "1" || saved === "0") open = saved === "1";
  } catch {}
  applyOpen(open);
  toggle.addEventListener("change", () => {
    applyOpen(toggle.checked);
    try { localStorage.setItem(openStateKey, toggle.checked ? "1" : "0"); } catch {}
  });

  loadKeymap();
  renderMappings();
  board.classList.add("off");
  paintDisabled();

  return {
    keyDown, keyUp, releaseAll, refresh, mapKey,
    get heldKeysSize() { return heldKeys.size; },
    get latched() { return [...latched.keys()]; },
    get keymapRows() { return keymapRows.map((r) => ({ ...r, to: r.to.slice() })); },
    applyWasdPreset: () => applyPresetRows(kWasdPresetRows),
    isWasdPresetActive, clearKeymap,
    upsertMapping: (from, to, preset) => {
      upsertMapping(from, to, preset);
      keymapChanged();
    },
  };
}
