// Asteroids arcade front end. Guest CPU is always 1.512 MHz vs wall clock.

const CPU_HZ = 1_512_000;
const IDB_NAME = "retroweb-asteroids";
const ROM_KEY = "set";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.asteroids.dips";
const DSW1_FACTORY = 0x12; // 1C/1C, x1 center, 3 lives (bit4), English
// $2802 pair: 0=1x&4 lives, 1=1x&3, 2=2x&4, 3=2x&3 (computerarcheology).
// Work RAM range holding the high-score table, through the initials at $51.
const HISCORE_ADDR = 0x1d;
const HISCORE_LEN = 0x35;

function concat3(parts) {
  const out = new Uint8Array(0x1800);
  for (let i = 0; i < 3; i++) out.set(parts[i], i * 0x800);
  return out;
}

// Board locations as named in MAME. 034602 is the bonus-coin PROM, which
// this core doesn't implement, so dumps that include it are ignored.
const SIZE = { program: 0x1800, chip: 0x0800, vector: 0x0800 };
const CHIP_RE = /(?:^|[._-])(035145|ef2|035144|h2|035143|j2|035127|np3)(?:[^a-z0-9]|$)/i;
const IGNORE_RE = /(?:^|[._-])(034602)(?:[^a-z0-9]|$)/i;
const ROLE_MAP = {
  "035145": "ef2", ef2: "ef2",
  "035144": "h2", h2: "h2",
  "035143": "j2", j2: "j2",
  "035127": "np3", np3: "np3",
};

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "vector.bin" || n === "vec.bin") return "vector";
  if (IGNORE_RE.test(n)) return "ignore";
  const m = n.match(CHIP_RE);
  return m ? ROLE_MAP[m[1].toLowerCase()] : null;
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
    const chips = ["ef2", "h2", "j2"].map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) program = concat3(chips);
  }
  let vector = clip(found.vector, SIZE.vector);
  if (!vector) vector = clip(found.np3, SIZE.vector);
  if (!program || !vector) return null;

  let kind = "user";
  if (hwtestCrcs &&
      arcadeHexCrc(program) === hwtestCrcs.program &&
      arcadeHexCrc(vector) === hwtestCrcs.vector) {
    kind = "hwtest";
  }
  return { kind, program, vector };
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

AsteroidsArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d", { alpha: false });
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "ast");

  let usingUserRom = false;
  let romKey = 0;
  let selfTest = false;
  const keys = {};
  const coinUntil = { left: 0, center: 0, right: 0 };
  const coinDoor = document.getElementById("coinDoor");

  // Stroke at device-pixel size; a fixed 1024² blit goes soft when scaled.
  function syncCanvasBacking() {
    const dpr = window.devicePixelRatio || 1;
    const css = screen.getBoundingClientRect();
    const w = Math.max(1, Math.round(css.width * dpr));
    const h = Math.max(1, Math.round(css.height * dpr));
    if (screen.width !== w || screen.height !== h) {
      screen.width = w;
      screen.height = h;
    }
    return { w, h, dpr };
  }

  function blit() {
    const { w, h, dpr } = syncCanvasBacking();
    const segs = machine.vectorSegments();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, w, h);
    // 4:3 tube. Attract uses beam Y 96..927, so open the height to 64..960.
    const sx = w / 1024;
    const sy = h / 896;
    const yTop = 960;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    const glowW = Math.max(2, 2.2 * dpr);
    const coreW = Math.max(1.1, 1.15 * dpr);
    const spotR = Math.max(1.2, 1.4 * dpr);
    for (let pass = 0; pass < 2; pass++) {
      const glow = pass === 0;
      ctx.lineWidth = glow ? glowW : coreW;
      for (let i = 0; i + 4 < segs.length; i += 5) {
        const x0 = segs[i] * sx;
        const y0 = (yTop - segs[i + 1]) * sy;
        const x1 = segs[i + 2] * sx;
        const y1 = (yTop - segs[i + 3]) * sy;
        const bri = segs[i + 4];
        const a = glow ? 0.18 + bri * 0.02 : 0.45 + bri * 0.036;
        const c = glow ? 160 + bri * 4 : 200 + bri * 3;
        const col = `rgba(${c|0},${c|0},${Math.min(255, c + 20)|0},${Math.min(1, a)})`;
        // Zero-length bright VEC is a photon shot.
        if (Math.abs(x1 - x0) < 0.5 && Math.abs(y1 - y0) < 0.5) {
          ctx.fillStyle = col;
          ctx.beginPath();
          ctx.arc(x0, y0, glow ? spotR * 1.8 : spotR, 0, Math.PI * 2);
          ctx.fill();
          continue;
        }
        ctx.strokeStyle = col;
        ctx.beginPath();
        ctx.moveTo(x0, y0);
        ctx.lineTo(x1, y1);
        ctx.stroke();
      }
    }
  }

  // Active-high (1 = pressed), unlike the Pac-Man/Galaxian boards.
  function applyKeys() {
    let n0 = 0, n1 = 0;
    const down = (k) => keys[k];
    if (down("ShiftLeft") || down("ShiftRight")) n0 |= 0x08;   // hyperspace
    if (down("KeyZ") || down("Space")) n0 |= 0x10; // fire
    if (selfTest) n0 |= 0x80;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil.left) n1 |= 0x01;
    if (down("Digit6") || down("Numpad6") || Date.now() < coinUntil.center) n1 |= 0x02;
    if (down("Digit7") || down("Numpad7") || Date.now() < coinUntil.right) n1 |= 0x04;
    if (down("Digit1") || down("Numpad1")) n1 |= 0x08; // 1P start
    if (down("Digit2") || down("Numpad2")) n1 |= 0x10; // 2P start
    if (down("ArrowUp") || down("KeyW")) n1 |= 0x20;   // thrust
    if (down("ArrowRight") || down("KeyD")) n1 |= 0x40; // rotate right
    if (down("ArrowLeft") || down("KeyA")) n1 |= 0x80;  // rotate left
    machine.setIn0(n0);
    machine.setIn1(n1);
  }

  function insertCoin() {
    // Both slots are the left coin (IN1 bit 0). Center and right stay on 6 and 7.
    coinUntil.left = Date.now() + 120;
    applyKeys();
    window.setTimeout(applyKeys, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", insertCoin);
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
    stick: null,
    buttons: [
      { code: "ArrowLeft", label: "Left" },
      { code: "ArrowRight", label: "Right" },
      { code: "ShiftLeft", label: "Hyperspace" },
      { code: "ArrowUp", label: "Thrust" },
      { code: "Space", label: "Fire", cls: "red" },
    ],
    groups: [["ArrowLeft", "ArrowRight"], ["ShiftLeft"], ["ArrowUp", "Space"]],
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

  // Scores are $1D-$30. $31-$33 are live initials-entry state that
  // CheckHighScore fills with $FF on attract (6502disassembly.com $7664),
  // so they must not count as a filled table.
  function hiscoreIsFactory(b) {
    for (let i = 0; i < 20; i++) if (b[i]) return false;
    return true;
  }

  // NMI runs at about 246 Hz. A few hundred frames is past the ROM's startup RAM clear.
  function postDone() {
    return machine.frames() >= 180 && hiscoreIsFactory(readHiscore());
  }

  const hiscore = arcadeHiscore({
    store,
    len: HISCORE_LEN,
    active: () => usingUserRom,
    key: () => String(romKey >>> 0),
    read: readHiscore,
    write: writeHiscore,
    isFactory: hiscoreIsFactory,
    ready: postDone,
  });

  async function resetHiscoreNow() {
    if (!usingUserRom) return;
    await hiscore.forget();
    machine.reset();
    applyDips();
  }
  const syncHiscoreResetBtn = arcadeHiscoreReset(() => usingUserRom, resetHiscoreNow);

  const dipCoinage = document.getElementById("dipCoinage");
  const dipRightMech = document.getElementById("dipRightMech");
  const dipCenterMech = document.getElementById("dipCenterMech");
  const dipLives = document.getElementById("dipLives");
  const dipLanguage = document.getElementById("dipLanguage");
  const dipSelfTest = document.getElementById("dipSelfTest");

  function encodeDsw1() {
    return (Number(dipCoinage.value) | Number(dipRightMech.value) | Number(dipCenterMech.value) |
            Number(dipLives.value) | Number(dipLanguage.value)) & 0xff;
  }

  function applyDips() {
    machine.setDsw1(encodeDsw1());
    selfTest = !!dipSelfTest.checked;
    applyKeys();
  }

  const dips = arcadePrefs(DIP_KEY);
  function saveDips() {
    dips.save({
      dsw1: encodeDsw1(),
      selfTest: !!dipSelfTest.checked,
    });
    applyDips();
  }

  function loadDips() {
    let dsw1 = DSW1_FACTORY;
    const s = dips.load();
    if (s) {
      if (typeof s.dsw1 === "number") dsw1 = s.dsw1 & 0xff;
      dipSelfTest.checked = !!s.selfTest;
    }
    dipCoinage.value = String(dsw1 & 0x03);
    dipRightMech.value = String(dsw1 & 0x0c);
    dipCenterMech.value = String(dsw1 & 0x20);
    dipLives.value = String(dsw1 & 0x10);
    dipLanguage.value = String(dsw1 & 0xc0);
    applyDips();
  }

  [dipCoinage, dipRightMech, dipCenterMech, dipLives, dipLanguage, dipSelfTest]
    .forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.vector) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.vector);
    usingUserRom = set.kind !== "hwtest";
    romKey = arcadeCrc32(set.program) ^ arcadeCrc32(set.vector);
    hiscore.rearm();
    syncHiscoreResetBtn();
    setStatus(label);
  }

  let hwtestCrcs = null;
  try {
    const c = await (await fetch("roms/crc.json")).json();
    hwtestCrcs = { program: c.program, vector: c.vector };
  } catch {}

  function loadBuiltInRom() {
    machine.loadHwtest();
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

  arcadeLoop({
    machine,
    hz: CPU_HZ,
    audio,
    hiscore,
    blit,
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
      readHiscore,
      writeHiscore,
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
