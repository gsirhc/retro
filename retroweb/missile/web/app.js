// Missile Command arcade front end. Guest CPU runs at the board average of 1.171875 MHz, paced to wall time.

const CPU_HZ = 1_171_875;
const IDB_NAME = "retroweb-missile";
const ROM_KEY = "set";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.missile.dips";
const R8_FACTORY = 0x73; // 6 cities, 10000 bonus, mini trak-ball (TM-147, rev 3)
const R10_FACTORY = 0x81; // 2 coins 1 play, English (TM-147 fig. 7)
const HISCORE_ADDR = 0x2c;
const HISCORE_LEN = 48; // 24 initials, 24 BCD
const HISCORE_INITIALS = 0x24f3; // program offsets of the factory table
const HISCORE_SCORES = 0x250b;
const IN0_IDLE = 0xff;
const IN1_IDLE = 0x67;
const TRACK_STEP = 2;

function concat(parts, size) {
  const out = new Uint8Array(parts.length * size);
  parts.forEach((p, i) => out.set(p, i * size));
  return out;
}

// 035820-02 through 035825-02 at H1, JK1, KL1, LM1, NP1, R1. 035826-01 is the optional L6 PROM.
const SIZE = { program: 0x3000, chip: 0x0800, prom: 0x20 };
const CHIPS = ["h1", "jk1", "kl1", "lm1", "np1", "r1"];
const NUM_RE = /(?:^|[._-])0358(2[0-6])(?:[^a-z0-9]|$)/i;
const LOC_RE = /(?:^|[._-])(h1|jk1|kl1|lm1|np1|r1|l6)(?:[^a-z0-9]|$)/i;
const NUM_LOC = { 20: "h1", 21: "jk1", 22: "kl1", 23: "lm1", 24: "np1", 25: "r1", 26: "l6" };

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "prom.bin") return "l6";
  const m = n.match(NUM_RE);
  if (m) return NUM_LOC[m[1]];
  const l = n.match(LOC_RE);
  return l ? l[1].toLowerCase() : null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || found[role]) continue;
    found[role] = data;
  }
  let program = clip(found.program, SIZE.program);
  if (!program) {
    const chips = CHIPS.map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) program = concat(chips, SIZE.chip);
  }
  if (!program) return null;
  let prom = null;
  if (found.l6) {
    prom = clip(found.l6, SIZE.prom);
    if (!prom) return null;
  }
  const kind = hwtestCrcs && arcadeHexCrc(program) === hwtestCrcs.program ? "hwtest" : "user";
  return { kind, program, prom };
}

function labelFor(kind) {
  if (kind === "hwtest") return "Hardware test ROM";
  return "Loaded ROM set";
}

initThemePicker();
initFullscreen({
  bezelEl: document.getElementById("bezel"),
  screenEl: document.getElementById("screen"),
  fullscreenBtn: document.getElementById("fullscreenBtn"),
  isRunning: () => true,
});
// Nothing focuses the screen on load, so nudge the hint once up front.
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

MissileArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(machine.screenWidth(), machine.screenHeight());
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "msl");

  let usingUserRom = false;
  let program = null;
  let romKey = 0;
  let selfTest = false;
  const keys = {};
  const coinUntil = { 1: 0, 2: 0 };
  const coinDoor = document.getElementById("coinDoor");
  const track = { x: 0, y: 0 };

  function blit() {
    img.data.set(machine.frameBuffer());
    ctx.putImageData(img, 0, 0);
  }

  // IN0 and IN1 are active-low.
  function applyKeys() {
    let n0 = IN0_IDLE;
    let n1 = IN1_IDLE;
    const down = (k) => keys[k];
    const now = Date.now();
    if (down("Digit1") || down("Numpad1")) n0 &= ~0x10;
    if (down("Digit2") || down("Numpad2")) n0 &= ~0x08;
    if (down("Digit5") || down("Numpad5") || now < coinUntil[1]) n0 &= ~0x20;
    if (down("Digit6") || down("Numpad6") || now < coinUntil[2]) n0 &= ~0x80;
    if (down("KeyZ") || down("Mouse0")) n1 &= ~0x04;
    if (down("KeyX") || down("Mouse1")) n1 &= ~0x02;
    if (down("KeyC") || down("Mouse2")) n1 &= ~0x01;
    if (down("KeyT")) n1 &= ~0x20;
    if (selfTest) n1 &= ~0x40;
    machine.setIn0(n0);
    machine.setIn1(n1);
  }

  // Mouse deltas accumulated between frames (pointer-lock or drag on #screen).
  let mouseDx = 0, mouseDy = 0;

  // Two 4-bit counters, read as VVVVHHHH. Upward roll counts the vertical
  // nibble up (rev 3 IRQ at $7BE6).
  function nudgeTrack(dx, dy) {
    track.x = (track.x + dx) & 0x0f;
    track.y = (track.y + dy) & 0x0f;
    machine.setTrackX(track.x);
    machine.setTrackY(track.y);
  }

  function stepTrackball() {
    const kdx = ((keys.ArrowRight || keys.KeyD) ? 1 : 0) - ((keys.ArrowLeft || keys.KeyA) ? 1 : 0);
    const kdy = ((keys.ArrowDown || keys.KeyS) ? 1 : 0) - ((keys.ArrowUp || keys.KeyW) ? 1 : 0);
    // Scale mouse pixels into a few counter ticks per frame.
    const mdx = Math.max(-7, Math.min(7, Math.round(mouseDx / 4)));
    const mdy = Math.max(-7, Math.min(7, Math.round(mouseDy / 4)));
    mouseDx = 0;
    mouseDy = 0;
    const dx = kdx * TRACK_STEP + mdx;
    const dy = kdy * TRACK_STEP + mdy;
    if (dx || dy) nudgeTrack(dx, -dy);
  }

  function pointerLocked() {
    return document.pointerLockElement === screen;
  }

  function releaseMouseButtons() {
    keys.Mouse0 = keys.Mouse1 = keys.Mouse2 = false;
    applyKeys();
  }

  screen.addEventListener("click", () => {
    if (!pointerLocked()) screen.requestPointerLock?.();
  });
  document.addEventListener("pointerlockchange", () => {
    if (pointerLocked()) screen.focus();
    else releaseMouseButtons();
  });
  screen.addEventListener("contextmenu", (e) => e.preventDefault());
  screen.addEventListener("mousemove", (e) => {
    if (pointerLocked()) {
      mouseDx += e.movementX;
      mouseDy += e.movementY;
      return;
    }
    // Drag without lock still rolls the ball (touchpads / denied lock).
    if (e.buttons & 1) {
      mouseDx += e.movementX;
      mouseDy += e.movementY;
    }
  });
  screen.addEventListener("mousedown", (e) => {
    if (!pointerLocked() || e.button > 2) return;
    e.preventDefault();
    keys["Mouse" + e.button] = true;
    applyKeys();
  });
  window.addEventListener("mouseup", (e) => {
    if (e.button > 2 || !keys["Mouse" + e.button]) return;
    keys["Mouse" + e.button] = false;
    applyKeys();
  });

  function insertCoin(which) {
    coinUntil[which] = Date.now() + 120;
    applyKeys();
    window.setTimeout(applyKeys, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", () => insertCoin(el.getAttribute("data-coin") === "2" ? 2 : 1));
  });
  document.querySelectorAll("[data-start]").forEach((el) => {
    const code = el.getAttribute("data-start") === "2" ? "Digit2" : "Digit1";
    const down = (e) => {
      if (e.button != null && e.button !== 0) return;
      e.preventDefault();
      try { el.setPointerCapture(e.pointerId); } catch {}
      el.classList.add("pressed");
      keys[code] = true;
      applyKeys();
      screen.focus();
    };
    const up = () => {
      el.classList.remove("pressed");
      keys[code] = false;
      applyKeys();
    };
    el.addEventListener("pointerdown", down);
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
  });

  const touch = initArcadeTouch({
    root: document.getElementById("touchControls"),
    stick: "8way",
    buttons: [
      { code: "KeyZ", label: "Alpha", cls: "red" },
      { code: "KeyX", label: "Delta", cls: "red" },
      { code: "KeyC", label: "Omega", cls: "red" },
    ],
    setKey: (code, down) => {
      keys[code] = down;
      applyKeys();
    },
  });

  window.addEventListener("keydown", (e) => {
    keys[e.code] = true;
    applyKeys();
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Space"].includes(e.code)) e.preventDefault();
  });
  window.addEventListener("keyup", (e) => {
    keys[e.code] = false;
    applyKeys();
  });

  function setStatus(t) { status.textContent = t; }

  function readHiscore() {
    const b = [];
    for (let i = 0; i < HISCORE_LEN; i++) b.push(machine.ramByte(HISCORE_ADDR + i) & 0xff);
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    for (let i = 0; i < HISCORE_LEN; i++) machine.setRamByte(HISCORE_ADDR + i, bytes[i] & 0xff);
  }

  // The ROM copies its own default table into RAM after clearing zero page.
  function factoryHiscore() {
    if (!program) return null;
    return [
      ...program.subarray(HISCORE_INITIALS, HISCORE_INITIALS + 24),
      ...program.subarray(HISCORE_SCORES, HISCORE_SCORES + 24),
    ];
  }

  function hiscoreIsFactory(b) {
    const f = factoryHiscore();
    return !!f && b.every((v, i) => v === f[i]);
  }

  // The ROM copies its factory table in after clearing zero page; restore over that.
  const hiscore = arcadeHiscore({
    store,
    len: HISCORE_LEN,
    active: () => usingUserRom,
    key: () => String(romKey >>> 0),
    read: readHiscore,
    write: writeHiscore,
    isFactory: hiscoreIsFactory,
    ready: () => hiscoreIsFactory(readHiscore()),
  });

  async function resetHiscoreNow() {
    if (!usingUserRom) return;
    await hiscore.forget();
    machine.reset();
  }
  const syncHiscoreResetBtn = arcadeHiscoreReset(() => usingUserRom, resetHiscoreNow);

  const dipIds = ["dipCities", "dipBonusCredit", "dipTrackball", "dipBonusCity", "dipCabinet"];
  const dipR10Ids = ["dipCoinage", "dipRightMult", "dipCenterMult", "dipLanguage", "dipSpare"];
  const dipEls = Object.fromEntries([...dipIds, ...dipR10Ids].map((id) => [id, document.getElementById(id)]));
  const dipSelfTest = document.getElementById("dipSelfTest");

  const sumOf = (ids) => ids.reduce((n, id) => n | Number(dipEls[id].value), 0) & 0xff;
  const encodeR8 = () => sumOf(dipIds);
  const encodeR10 = () => sumOf(dipR10Ids);

  function applyDips() {
    machine.setR8(encodeR8());
    machine.setR10(encodeR10());
    selfTest = !!dipSelfTest.checked;
    applyKeys();
  }

  const dips = arcadePrefs(DIP_KEY);
  function saveDips() {
    dips.save({
      r8: encodeR8(),
      r10: encodeR10(),
      selfTest: !!dipSelfTest.checked,
    });
    applyDips();
  }

  const R8_MASKS = { dipCities: 0x03, dipBonusCredit: 0x04, dipTrackball: 0x08, dipBonusCity: 0x70, dipCabinet: 0x80 };
  const R10_MASKS = { dipCoinage: 0x03, dipRightMult: 0x0c, dipCenterMult: 0x10, dipLanguage: 0x60, dipSpare: 0x80 };

  function loadDips() {
    let r8 = R8_FACTORY;
    let r10 = R10_FACTORY;
    const s = dips.load();
    if (s) {
      if (typeof s.r8 === "number") r8 = s.r8 & 0xff;
      if (typeof s.r10 === "number") r10 = s.r10 & 0xff;
      dipSelfTest.checked = !!s.selfTest;
    }
    for (const [id, mask] of Object.entries(R8_MASKS)) dipEls[id].value = String(r8 & mask);
    for (const [id, mask] of Object.entries(R10_MASKS)) dipEls[id].value = String(r10 & mask);
    applyDips();
  }

  [...Object.values(dipEls), dipSelfTest].forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  async function applySet(set, label) {
    if (!set || !set.program) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.prom || null);
    program = set.program;
    usingUserRom = set.kind !== "hwtest";
    romKey = arcadeCrc32(set.program);
    hiscore.rearm();
    syncHiscoreResetBtn();
    setStatus(label);
  }

  let hwtestCrcs = null;
  try {
    const c = await (await fetch("roms/crc.json")).json();
    hwtestCrcs = { program: c.program };
  } catch {}

  function loadBuiltInRom() {
    machine.loadHwtest();
    program = null;
    usingUserRom = false;
    hiscore.rearm();
    syncHiscoreResetBtn();
    setStatus("Running test ROM");
  }

  const roms = arcadeRomLoader({
    store,
    key: ROM_KEY,
    identify: (files) => identifySet(files, hwtestCrcs),
    apply: (set, stored) => applySet(set, stored ? "Loaded stored ROM set" : labelFor(set.kind)),
    builtIn: loadBuiltInRom,
    setStatus,
  });
  await roms.boot();

  let trackAcc = 0;
  arcadeLoop({
    machine,
    hz: CPU_HZ,
    audio,
    hiscore,
    blit,
    beforeRun: (dt) => {
      trackAcc += dt;
      while (trackAcc >= 1 / 60) {
        trackAcc -= 1 / 60;
        stepTrackball();
      }
    },
  });

  if (TEST) {
    window.__test = {
      machine,
      touch,
      get status() { return status.textContent; },
      get usingUserRom() { return usingUserRom; },
      mute: document.getElementById("mute"),
      get muted() { return audio.isMuted(); },
      encodeR8,
      encodeR10,
      get trackPos() { return { x: track.x, y: track.y }; },
      readHiscore,
      writeHiscore,
      factoryHiscore,
      saveHiscoreNow: hiscore.saveNow,
      maybeSaveHiscore: hiscore.maybeSave,
      resetHiscoreNow,
      get hiscoreRestored() { return hiscore.restored; },
      get hiscoreKey() { return String(romKey >>> 0); },
      restoreHiscoreNow: hiscore.restoreNow,
      hiscoreReady: hiscore.ready,
      savedHiscores: hiscore.loadAll,
      crc32: arcadeCrc32,
      identifySet,
      hwtestCrcs,
      applySet,
      ROM_KEY,
      loadSavedHiscores: hiscore.loadAll,
      wipeStorage: async () => {
        await store.del("set");
        await store.del("hiscores");
      },
    };
  }
});
