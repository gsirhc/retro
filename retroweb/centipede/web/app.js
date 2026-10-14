// Centipede arcade front end. Guest CPU is always 1.512 MHz vs wall clock.

const CPU_HZ = 1_512_000;
const IDB_NAME = "retroweb-centipede";
const ROM_KEY = "set";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.centipede.dips";
const DSW1_FACTORY = 0x54; // 3 lives, 12000 bonus, easy, English (MAME centiped)
const DSW2_FACTORY = 0x02; // 1C/1C
const HISCORE_LEN = 0x40; // ER2055 is 64 x 8
const TRACK_STEP = 2;

function concat(parts, size) {
  const out = new Uint8Array(parts.length * size);
  parts.forEach((p, i) => out.set(p, i * size));
  return out;
}

// Board locations as named in MAME. 213/214 are the colour PROMs, which
// this core doesn't read, so dumps that include them are ignored.
const SIZE = { program: 0x2000, gfx: 0x1000, chip: 0x0800 };
// Program chips: rev 3 is 307–310, rev 4 (common MAME centiped set) is 407–410.
const NUM_RE = /(?:^|[._-])(307|308|309|310|407|408|409|410|211|212)(?:[^a-z0-9]|$)/i;
const LOC_RE = /(?:^|[._-])(d1|e1|fh1|j1|f7|hj7)(?:[^a-z0-9]|$)/i;
const IGNORE_RE = /(?:^|[._-])(213|214)(?:[^a-z0-9]|$)/i;
const LOC_NUM = { d1: "307", e1: "308", fh1: "309", j1: "310", f7: "211", hj7: "212" };
const NUM_ALIAS = { "407": "307", "408": "308", "409": "309", "410": "310" };
const PROGRAM_CHIPS = ["307", "308", "309", "310"];
const GFX_CHIPS = ["211", "212"];

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "gfx.bin" || n === "gfx") return "gfx";
  if (IGNORE_RE.test(n)) return "ignore";
  const m = n.match(NUM_RE);
  if (m) return NUM_ALIAS[m[1]] || m[1];
  const l = n.match(LOC_RE);
  return l ? LOC_NUM[l[1].toLowerCase()] : null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || role === "ignore" || found[role]) continue;
    found[role] = data;
  }
  let program = clip(found.program, SIZE.program);
  if (!program) {
    const chips = PROGRAM_CHIPS.map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) program = concat(chips, SIZE.chip);
  }
  let gfx = clip(found.gfx, SIZE.gfx);
  if (!gfx) {
    const chips = GFX_CHIPS.map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) gfx = concat(chips, SIZE.chip);
  }
  if (!program || !gfx) return null;

  let kind = "user";
  if (hwtestCrcs &&
      arcadeHexCrc(program) === hwtestCrcs.program &&
      arcadeHexCrc(gfx) === hwtestCrcs.gfx) {
    kind = "hwtest";
  }
  return { kind, program, gfx };
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

CentipedeArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(240, 256);
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "cen");

  let usingUserRom = false;
  let romKey = 0;
  let selfTest = false;
  const keys = {};
  const coinUntil = { 1: 0, 2: 0 };
  const coinDoor = document.getElementById("coinDoor");
  const track = { x: 0, y: 0, signX: 0, signY: 0 };

  function blit() {
    img.data.set(machine.frameBuffer());
    ctx.putImageData(img, 0, 0);
  }

  // IN1 is active-low, idle 0xFF.
  function applyKeys() {
    let n1 = 0xff;
    const down = (k) => keys[k];
    if (down("Digit1") || down("Numpad1")) n1 &= ~0x01;
    if (down("Digit2") || down("Numpad2")) n1 &= ~0x02;
    if (down("KeyZ") || down("Space")) n1 &= ~0x04;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil[1]) n1 &= ~0x20;
    if (down("Digit6") || down("Numpad6") || Date.now() < coinUntil[2]) n1 &= ~0x40;
    machine.setIn1(n1);
    machine.setIn0Switches(selfTest ? 0x00 : 0x20);
  }

  // Mouse deltas accumulated between frames (pointer-lock or drag on #screen).
  let mouseDx = 0, mouseDy = 0;

  // The 4-bit counter holds its value between pushes and the sign latches the
  // last direction (MAME centiped.cpp read_trackball). X is wired reversed
  // (TRACK0_X PORT_REVERSE), so right counts down.
  function nudgeTrack(dx, dy) {
    if (dx) {
      track.x = (track.x + dx) & 0xff;
      track.signX = dx < 0 ? 0x80 : 0;
    }
    if (dy) {
      track.y = (track.y + dy) & 0xff;
      track.signY = dy < 0 ? 0x80 : 0;
    }
    machine.setTrackX(track.signX | (track.x & 0x0f));
    machine.setTrackY(track.signY | (track.y & 0x0f));
  }

  function stepTrackball() {
    const kdx = ((keys.ArrowLeft || keys.KeyA) ? 1 : 0) - ((keys.ArrowRight || keys.KeyD) ? 1 : 0);
    const kdy = ((keys.ArrowDown || keys.KeyS) ? 1 : 0) - ((keys.ArrowUp || keys.KeyW) ? 1 : 0);
    if (kdx || kdy) nudgeTrack(kdx * TRACK_STEP, kdy * TRACK_STEP);
    // Scale mouse pixels into a few counter ticks per frame.
    const mdx = Math.max(-7, Math.min(7, Math.round(mouseDx / 4)));
    const mdy = Math.max(-7, Math.min(7, Math.round(mouseDy / 4)));
    mouseDx = 0;
    mouseDy = 0;
    // Same PORT_REVERSE on X as the keyboard path.
    if (mdx || mdy) nudgeTrack(-mdx, mdy);
  }

  function pointerLocked() {
    return document.pointerLockElement === screen;
  }

  screen.addEventListener("click", () => {
    if (!pointerLocked()) screen.requestPointerLock?.();
  });
  document.addEventListener("pointerlockchange", () => {
    if (pointerLocked()) screen.focus();
  });
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
    buttons: [{ code: "Space", label: "Fire", cls: "red" }],
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
    return Array.from(machine.earomBytes());
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    for (let i = 0; i < HISCORE_LEN; i++) machine.setEaromByte(i, bytes[i] & 0xff);
  }

  function hiscoreIsFactory(b) {
    for (let i = 0; i < b.length; i++) if (b[i]) return false;
    return true;
  }

  // The EAROM holds its contents with the power off, so the saved image goes
  // in at load (applySet), before the game reads it, then the board restarts.
  const hiscore = arcadeHiscore({
    store,
    len: HISCORE_LEN,
    active: () => usingUserRom,
    key: () => String(romKey >>> 0),
    read: readHiscore,
    write: writeHiscore,
    isFactory: hiscoreIsFactory,
    ready: () => false,
    restore: (saved) => {
      if (saved && saved.length >= HISCORE_LEN) {
        writeHiscore(saved);
        machine.reset();
      }
      return readHiscore();
    },
  });

  async function resetHiscoreNow() {
    if (!usingUserRom) return;
    await hiscore.forget();
    writeHiscore(new Array(HISCORE_LEN).fill(0));
    machine.reset();
    applyDips();
    await hiscore.restoreNow();
  }
  const syncHiscoreResetBtn = arcadeHiscoreReset(() => usingUserRom, resetHiscoreNow);

  const dipLives = document.getElementById("dipLives");
  const dipBonus = document.getElementById("dipBonus");
  const dipDifficulty = document.getElementById("dipDifficulty");
  const dipLanguage = document.getElementById("dipLanguage");
  const dipCoinage = document.getElementById("dipCoinage");
  const dipSelfTest = document.getElementById("dipSelfTest");

  function encodeDsw1() {
    return (Number(dipLives.value) | Number(dipBonus.value) | Number(dipDifficulty.value) |
            Number(dipLanguage.value)) & 0xff;
  }

  function encodeDsw2() {
    return Number(dipCoinage.value) & 0xff;
  }

  function applyDips() {
    machine.setDsw1(encodeDsw1());
    machine.setDsw2(encodeDsw2());
    selfTest = !!dipSelfTest.checked;
    applyKeys();
  }

  const dips = arcadePrefs(DIP_KEY);
  function saveDips() {
    dips.save({
      dsw1: encodeDsw1(),
      dsw2: encodeDsw2(),
      selfTest: !!dipSelfTest.checked,
    });
    applyDips();
  }

  function loadDips() {
    let dsw1 = DSW1_FACTORY;
    let dsw2 = DSW2_FACTORY;
    const s = dips.load();
    if (s) {
      if (typeof s.dsw1 === "number") dsw1 = s.dsw1 & 0xff;
      if (typeof s.dsw2 === "number") dsw2 = s.dsw2 & 0xff;
      dipSelfTest.checked = !!s.selfTest;
    }
    dipLives.value = String(dsw1 & 0x0c);
    dipBonus.value = String(dsw1 & 0x30);
    dipDifficulty.value = String(dsw1 & 0x40);
    dipLanguage.value = String(dsw1 & 0x03);
    dipCoinage.value = String(dsw2 & 0x03);
    applyDips();
  }

  [dipLives, dipBonus, dipDifficulty, dipLanguage, dipCoinage, dipSelfTest]
    .forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  async function applySet(set, label) {
    if (!set || !set.program || !set.gfx) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.gfx);
    usingUserRom = set.kind !== "hwtest";
    romKey = arcadeCrc32(set.program) ^ arcadeCrc32(set.gfx);
    hiscore.rearm();
    // EAROM survives load_roms/reset; clear so restore sees a clean chip.
    writeHiscore(new Array(HISCORE_LEN).fill(0));
    syncHiscoreResetBtn();
    setStatus(label);
    await hiscore.restoreNow().catch(() => {});
  }

  let hwtestCrcs = null;
  try {
    const c = await (await fetch("roms/crc.json")).json();
    hwtestCrcs = { program: c.program, gfx: c.gfx };
  } catch {}

  function loadBuiltInRom() {
    machine.loadHwtest();
    usingUserRom = false;
    hiscore.rearm();
    writeHiscore(new Array(HISCORE_LEN).fill(0));
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
      encodeDsw1,
      encodeDsw2,
      get in0Switches() { return selfTest ? 0x00 : 0x20; },
      get trackPos() { return { x: track.x, y: track.y }; },
      readHiscore,
      writeHiscore,
      saveHiscoreNow: hiscore.saveNow,
      maybeSaveHiscore: hiscore.maybeSave,
      resetHiscoreNow,
      restoreHiscoreNow: hiscore.restoreNow,
      hiscoreReady: hiscore.ready,
      savedHiscores: hiscore.loadAll,
      get hiscoreRestored() { return hiscore.restored; },
      get hiscoreKey() { return String(romKey >>> 0); },
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
