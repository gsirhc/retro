// Battlezone arcade front end. Guest CPU is always 1.512 MHz vs wall clock.

const CPU_HZ = 1_512_000;
const IDB_NAME = "retroweb-battlezone";
const ROM_KEY = "set";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.battlezone.dips";
const DSW0_FACTORY = 0x15; // 3 tanks, missile at 10000, bonus 15k and 100k, English
const DSW1_FACTORY = 0x63; // 2 coins / 1 play, mechs x1, 6 credits / 4 coins (manual fig. 8)
const HISCORE_ADDR = 0x300;
const HISCORE_LEN = 0x3c;
const INITLS_ROM = 0x2B7F; // program offset of the initials the reset code copies to $031E

// Both 036414 revisions share every other chip (sets bzone and bzonea).
// 036408 is the AVG state PROM; avg.cpp carries its sequence, so it isn't needed.
const PROGRAM_PARTS = ["036414", "036413", "036412", "036411", "036410", "036409"];
const VECTOR_PARTS = ["036422", "036421"];
const MAP_PART = "036174";
const UCODE_PARTS = ["036175", "036176", "036177", "036178", "036179", "036180"];
const KNOWN = [...PROGRAM_PARTS, ...VECTOR_PARTS, MAP_PART, ...UCODE_PARTS];
const PART_RE = /(?:^|[^0-9])(0364\d\d|0361[78]\d)(?:-\d\d[a-z]?)?(?:[._]|$)/i;

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin") return "program";
  if (n === "vector.bin") return "vector";
  const m = n.match(PART_RE);
  if (!m) return null;
  return KNOWN.includes(m[1]) ? m[1] : "ignore";
}

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function join(parts, size) {
  if (!parts.every(Boolean)) return null;
  const out = new Uint8Array(parts.length * size);
  parts.forEach((p, i) => out.set(p, i * size));
  return out;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || role === "ignore" || found[role]) continue;
    found[role] = data;
  }
  const program = clip(found.program, 0x3000) || join(PROGRAM_PARTS.map((p) => clip(found[p], 0x800)), 0x800);
  const vector = clip(found.vector, 0x1000) || join(VECTOR_PARTS.map((p) => clip(found[p], 0x800)), 0x800);
  const map = clip(found[MAP_PART], 32);
  const ucode = join(UCODE_PARTS.map((p) => clip(found[p], 256)), 256);
  if (!program || !vector || !map || !ucode) return null;

  let kind = "user";
  if (hwtestCrcs &&
      arcadeHexCrc(program) === hwtestCrcs.program &&
      arcadeHexCrc(vector) === hwtestCrcs.vector) {
    kind = "hwtest";
  }
  return { kind, program, vector, map, ucode };
}

initThemePicker();
initFullscreen({
  bezelEl: document.getElementById("bezel"),
  screenEl: document.getElementById("screen"),
  fullscreenBtn: document.getElementById("fullscreenBtn"),
  isRunning: () => true,
});
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

BattlezoneArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d", { alpha: false });
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "bzone");
  const startBtn = document.querySelector('[data-start="1"]');
  const picW = machine.screenWidth();
  const picH = machine.screenHeight();
  const splitY = picH * machine.overlaySplit();

  let usingUserRom = false;
  let romKey = 0;
  let program = new Uint8Array(0x3000);
  let selfTest = false;
  let diagStep = false;
  const keys = {};
  const coinUntil = { left: 0, right: 0 };
  const coinDoor = document.getElementById("coinDoor");

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

  // The cabinet's overlay film: red across the top quarter, green below.
  function tint(red, bri, glow) {
    const hi = glow ? 150 + bri * 7 : 190 + bri * 4;
    const lo = glow ? 20 : 40 + bri * 3;
    const a = glow ? 0.16 + bri * 0.02 : 0.45 + bri * 0.036;
    return red ? `rgba(${hi},${lo},${lo},${Math.min(1, a)})` : `rgba(${lo},${hi},${lo},${Math.min(1, a)})`;
  }

  function blit() {
    const { w, h, dpr } = syncCanvasBacking();
    const segs = machine.vectorSegments();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, w, h);
    const sx = w / picW;
    const sy = h / picH;
    ctx.lineCap = "round";
    const glowW = Math.max(2, 2.2 * dpr);
    const coreW = Math.max(1.1, 1.15 * dpr);
    const stroke = (x0, y0, x1, y1, red, bri, glow) => {
      ctx.strokeStyle = tint(red, bri, glow);
      ctx.beginPath();
      ctx.moveTo(x0 * sx, y0 * sy);
      ctx.lineTo(x1 * sx, y1 * sy);
      ctx.stroke();
    };
    for (let pass = 0; pass < 2; pass++) {
      const glow = pass === 0;
      ctx.lineWidth = glow ? glowW : coreW;
      for (let i = 0; i + 4 < segs.length; i += 5) {
        const x0 = segs[i], y0 = segs[i + 1], x1 = segs[i + 2], y1 = segs[i + 3], bri = segs[i + 4];
        if (Math.abs(x1 - x0) < 0.3 && Math.abs(y1 - y0) < 0.3) {
          ctx.fillStyle = tint(y0 < splitY, bri, glow);
          ctx.beginPath();
          ctx.arc(x0 * sx, y0 * sy, glow ? glowW : coreW, 0, Math.PI * 2);
          ctx.fill();
          continue;
        }
        if ((y0 < splitY) !== (y1 < splitY)) {
          const t = (splitY - y0) / (y1 - y0);
          const xm = x0 + (x1 - x0) * t;
          stroke(x0, y0, xm, splitY, y0 < splitY, bri, glow);
          stroke(xm, splitY, x1, y1, y1 < splitY, bri, glow);
        } else {
          stroke(x0, y0, x1, y1, y0 < splitY, bri, glow);
        }
      }
    }
  }

  // IN0 active low: D0 right mech, D1 middle mech, the left slot here (COIN65.MAC COIN01).
  function applyKeys() {
    const down = (k) => keys[k];
    let in0 = 0x3f;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil.left) in0 &= ~0x02;
    if (down("Digit6") || down("Numpad6") || Date.now() < coinUntil.right) in0 &= ~0x01;
    if (selfTest) in0 &= ~0x10;
    if (diagStep || down("F1")) in0 &= ~0x20;

    let leftUp = down("KeyW") || down("ArrowUp") || down("ArrowRight");
    let leftDown = down("KeyS") || down("ArrowDown") || down("ArrowLeft");
    let rightUp = down("KeyI") || down("ArrowUp") || down("ArrowLeft");
    let rightDown = down("KeyK") || down("ArrowDown") || down("ArrowRight");
    // A lever has one switch per direction; it can't be pushed both ways.
    if (leftUp && leftDown) leftUp = leftDown = false;
    if (rightUp && rightDown) rightUp = rightDown = false;
    let in3 = 0;
    if (rightDown) in3 |= 0x01;
    if (rightUp) in3 |= 0x02;
    if (leftDown) in3 |= 0x04;
    if (leftUp) in3 |= 0x08;
    if (down("Space") || down("KeyZ") || down("ControlLeft")) in3 |= 0x10;
    if (down("Digit1") || down("Numpad1")) in3 |= 0x20;
    machine.setIn0(in0);
    machine.setIn3(in3);
  }

  function insertCoin(slot) {
    coinUntil[slot] = Date.now() + 120;
    applyKeys();
    window.setTimeout(applyKeys, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", () => insertCoin(el.getAttribute("data-coin")));
  });
  document.querySelectorAll("[data-start]").forEach((el) => {
    const code = "Digit1";
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
      { code: "KeyW", label: "L fwd" },
      { code: "KeyS", label: "L back" },
      { code: "Space", label: "Fire", cls: "red" },
      { code: "KeyI", label: "R fwd" },
      { code: "KeyK", label: "R back" },
    ],
    groups: [["KeyW", "KeyS"], ["Space"], ["KeyI", "KeyK"]],
    setKey: (code, isDown) => {
      keys[code] = isDown;
      applyKeys();
    },
  });

  const GAME_KEYS = ["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Space", "F1"];
  window.addEventListener("keydown", (e) => {
    keys[e.code] = true;
    applyKeys();
    if (GAME_KEYS.includes(e.code)) e.preventDefault();
  });
  window.addEventListener("keyup", (e) => {
    keys[e.code] = false;
    applyKeys();
  });
  window.addEventListener("blur", () => {
    for (const k of Object.keys(keys)) keys[k] = false;
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

  // Reset code ($7B5C) writes 5 to each score's low byte and ROM $7B7F+ to the initials; the rest stays cleared.
  function hiscoreIsFactory(b) {
    for (let i = 0; i < 30; i++) if (b[i] !== (i % 3 === 0 ? 5 : 0)) return false;
    for (let i = 0; i < 30; i++) if (b[30 + i] !== program[INITLS_ROM + i]) return false;
    return true;
  }

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
    applyDips();
  }
  const syncHiscoreResetBtn = arcadeHiscoreReset(() => usingUserRom, resetHiscoreNow);

  const dip = (id) => document.getElementById(id);
  const DSW0_FIELDS = ["dipLives", "dipMissile", "dipBonus", "dipLanguage"];
  const DSW1_FIELDS = ["dipCoinage", "dipRightMech", "dipLeftMech", "dipBonusCoins"];
  const dipSelfTest = dip("dipSelfTest");

  function encode(fields) {
    return fields.reduce((v, id) => v | Number(dip(id).value), 0) & 0xff;
  }
  function encodeDsw0() { return encode(DSW0_FIELDS); }
  function encodeDsw1() { return encode(DSW1_FIELDS); }

  function applyDips() {
    machine.setDsw0(encodeDsw0());
    machine.setDsw1(encodeDsw1());
    selfTest = !!dipSelfTest.checked;
    applyKeys();
  }

  const dips = arcadePrefs(DIP_KEY);
  function saveDips() {
    dips.save({ dsw0: encodeDsw0(), dsw1: encodeDsw1(), selfTest: !!dipSelfTest.checked });
    applyDips();
  }

  const MASKS = {
    dipLives: 0x03, dipMissile: 0x0c, dipBonus: 0x30, dipLanguage: 0xc0,
    dipCoinage: 0x03, dipRightMech: 0x0c, dipLeftMech: 0x10, dipBonusCoins: 0xe0,
  };
  function loadDips() {
    let dsw0 = DSW0_FACTORY;
    let dsw1 = DSW1_FACTORY;
    const s = dips.load();
    if (s) {
      if (typeof s.dsw0 === "number") dsw0 = s.dsw0 & 0xff;
      if (typeof s.dsw1 === "number") dsw1 = s.dsw1 & 0xff;
      dipSelfTest.checked = !!s.selfTest;
    }
    for (const id of DSW0_FIELDS) dip(id).value = String(dsw0 & MASKS[id]);
    for (const id of DSW1_FIELDS) dip(id).value = String(dsw1 & MASKS[id]);
    applyDips();
  }

  [...DSW0_FIELDS, ...DSW1_FIELDS].forEach((id) => dip(id).addEventListener("change", saveDips));
  dipSelfTest.addEventListener("change", saveDips);
  const diagBtn = dip("diagStep");
  const diagUp = () => { diagStep = false; applyKeys(); };
  diagBtn.addEventListener("pointerdown", () => { diagStep = true; applyKeys(); });
  diagBtn.addEventListener("pointerup", diagUp);
  diagBtn.addEventListener("pointerleave", diagUp);
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.vector || !set.map || !set.ucode) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.vector, set.map, set.ucode);
    usingUserRom = set.kind !== "hwtest";
    romKey = arcadeCrc32(set.program) ^ arcadeCrc32(set.vector);
    program = set.program;
    hiscore.rearm();
    syncHiscoreResetBtn();
    applyDips();
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
    applyDips();
    setStatus("Running test ROM");
  }

  const roms = arcadeRomLoader({
    store,
    key: ROM_KEY,
    identify: (files) => identifySet(files, hwtestCrcs),
    apply: (set, stored) => applySet(set, stored ? "Loaded stored ROM set" : set.kind === "hwtest" ? "Hardware test ROM" : "Loaded ROM set"),
    builtIn: loadBuiltInRom,
    setStatus,
  });
  await roms.boot();

  arcadeLoop({
    machine,
    hz: CPU_HZ,
    audio,
    hiscore,
    afterRun: () => startBtn.classList.toggle("lamp-on", machine.startLamp()),
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
      encodeDsw0,
      encodeDsw1,
      readHiscore,
      writeHiscore,
      hiscoreIsFactory,
      saveHiscoreNow: hiscore.saveNow,
      maybeSaveHiscore: hiscore.maybeSave,
      resetHiscoreNow,
      get hiscoreRestored() { return hiscore.restored; },
      get hiscoreKey() { return String(romKey >>> 0); },
      restoreHiscoreNow: hiscore.restoreNow,
      hiscoreReady: hiscore.ready,
      savedHiscores: hiscore.loadAll,
      crc32: arcadeCrc32,
      roleOf,
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
