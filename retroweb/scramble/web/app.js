// Scramble arcade front end. Guest main CPU is always 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-scramble";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.scramble.dips";
const ROM_KEY = "set";
const HISCORE_LEN = 0x21;

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function concat(parts, total) {
  const out = new Uint8Array(total);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "sound.bin") return "sound";
  if (n === "gfx.bin") return "gfx";
  if (/c01s|6e/.test(n)) return "prom";
  const m = n.match(/^s([1-8])\./);
  if (m) return "s" + m[1];
  if (/^ot1/.test(n)) return "ot1";
  if (/^ot2/.test(n)) return "ot2";
  if (/^ot3/.test(n)) return "ot3";
  if (/^c2\.|^2\.5f/.test(n)) return "c2";
  if (/^c1\.|^1\.5h/.test(n)) return "c1";
  return null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || found[role]) continue;
    found[role] = data;
  }
  let program = clip(found.program, 0x4000);
  if (!program) {
    const chips = [1, 2, 3, 4, 5, 6, 7, 8].map((i) => clip(found["s" + i], 0x800));
    if (chips.every(Boolean)) program = concat(chips, 0x4000);
  }
  let sound = clip(found.sound, 0x1800);
  if (!sound) {
    const a = clip(found.ot1, 0x800);
    const b = clip(found.ot2, 0x800);
    const c = clip(found.ot3, 0x800);
    if (a && b && c) sound = concat([a, b, c], 0x1800);
  }
  let gfx = clip(found.gfx, 0x1000);
  if (!gfx) {
    const a = clip(found.c2, 0x800);
    const b = clip(found.c1, 0x800);
    if (a && b) gfx = concat([a, b], 0x1000);
  }
  const color = clip(found.prom, 0x20);
  if (!program || !sound || !gfx || !color) return null;

  let kind = "user";
  if (hwtestCrcs &&
      arcadeHexCrc(program) === hwtestCrcs["program.bin"] &&
      arcadeHexCrc(sound) === hwtestCrcs["sound.bin"] &&
      arcadeHexCrc(gfx) === hwtestCrcs["gfx.bin"] &&
      arcadeHexCrc(color) === hwtestCrcs["c01s.6e"]) {
    kind = "hwtest";
  }
  return { kind, program, sound, gfx, color };
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
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

ScrambleArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 256);
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "ay");

  let usingUserRom = false;
  let programCrc = 0;
  const keys = {};
  let coinUntil = 0;
  const coinDoor = document.getElementById("coinDoor");
  const dipCoinage = document.getElementById("dipCoinage");
  const dipLives = document.getElementById("dipLives");
  const dipCabinet = document.getElementById("dipCabinet");

  function applyKeys() {
    let n0 = 0xFF;
    let n1 = (0xFC & ~0x03) | (Number(dipLives.value) & 0x03);
    let n2 = (0x51 & ~0x0E) | (Number(dipCoinage.value) & 0x06) | (Number(dipCabinet.value) & 0x08);
    const down = (k) => keys[k];
    if (down("ArrowRight") || down("KeyD")) n0 &= ~0x10;
    if (down("ArrowLeft") || down("KeyA")) n0 &= ~0x20;
    if (down("Space") || down("KeyZ")) n0 &= ~0x08;
    if (down("KeyX") || down("ControlLeft") || down("ControlRight")) n0 &= ~0x02;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil) n0 &= ~0x80;
    if (down("Digit1") || down("Numpad1")) n1 &= ~0x80;
    if (down("Digit2") || down("Numpad2")) n1 &= ~0x40;
    if (down("ArrowUp") || down("KeyW")) n2 &= ~0x10;
    if (down("ArrowDown") || down("KeyS")) n2 &= ~0x40;
    machine.setIn0(n0);
    machine.setIn1(n1);
    machine.setIn2(n2);
  }

  function insertCoin() {
    coinUntil = Date.now() + 120;
    applyKeys();
    window.setTimeout(applyKeys, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
    audio.ensure().catch(() => {});
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
      audio.ensure().catch(() => {});
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
    buttons: [{ code: "Space", label: "Fire", cls: "red" }, { code: "KeyX", label: "Bomb" }],
    setKey: (code, down) => {
      keys[code] = down;
      applyKeys();
    },
  });

  window.addEventListener("keydown", (e) => {
    keys[e.code] = true;
    applyKeys();
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Space"].includes(e.code)) e.preventDefault();
    audio.ensure().catch(() => {});
  });
  window.addEventListener("keyup", (e) => {
    keys[e.code] = false;
    applyKeys();
  });

  function blit() {
    const rgba = machine.frameBuffer();
    img.data.set(rgba);
    ctx.putImageData(img, 0, 0);
  }

  function setStatus(t) { status.textContent = t; }

  function readHiscore() {
    const b = [];
    for (let i = 0; i < 0x1E; i++) b.push(machine.ramByte(0x4200 + i) & 0xff);
    for (let i = 0; i < 3; i++) b.push(machine.ramByte(0x40A8 + i) & 0xff);
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    for (let i = 0; i < 0x1E; i++) machine.setRamByte(0x4200 + i, bytes[i] & 0xff);
    for (let i = 0; i < 3; i++) machine.setRamByte(0x40A8 + i, bytes[0x1E + i] & 0xff);
  }

  // Cold start writes ten 00 00 01 entries at $4200 and $01 at $40AA (code $0195).
  function hiscoreIsFactory(b) {
    for (let i = 0; i < HISCORE_LEN; i++) if (b[i] !== (i % 3 === 2 ? 1 : 0)) return false;
    return true;
  }

  const hiscore = arcadeHiscore({
    store,
    len: HISCORE_LEN,
    active: () => usingUserRom,
    key: () => String(programCrc >>> 0),
    read: readHiscore,
    write: writeHiscore,
    isFactory: hiscoreIsFactory,
    ready: () => hiscoreIsFactory(readHiscore()),
  });

  async function resetHiscoreNow() {
    if (!usingUserRom) return;
    await hiscore.forget();
    machine.reset();
    applyKeys();
  }
  const syncHiscoreResetBtn = arcadeHiscoreReset(() => usingUserRom, resetHiscoreNow);

  const dips = arcadePrefs(DIP_KEY);
  function saveDips() {
    dips.save({
      lives: Number(dipLives.value),
      coinage: Number(dipCoinage.value),
      cabinet: Number(dipCabinet.value),
    });
    applyKeys();
  }

  function loadDips() {
    const s = dips.load();
    if (s) {
      if (s.lives !== undefined) dipLives.value = String(s.lives);
      if (s.coinage !== undefined) dipCoinage.value = String(s.coinage);
      if (s.cabinet !== undefined) dipCabinet.value = String(s.cabinet);
    }
    applyKeys();
  }

  [dipCoinage, dipLives, dipCabinet].forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.sound || !set.gfx || !set.color)
      throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.sound, set.gfx, set.color);
    usingUserRom = set.kind !== "hwtest";
    programCrc = arcadeCrc32(set.program);
    hiscore.rearm();
    syncHiscoreResetBtn();
    setStatus(label);
  }

  let hwtestCrcs = null;
  try { hwtestCrcs = await (await fetch("roms/crc.json")).json(); } catch {}

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
      readHiscore,
      writeHiscore,
      saveHiscoreNow: hiscore.saveNow,
      maybeSaveHiscore: hiscore.maybeSave,
      get hiscoreRestored() { return hiscore.restored; },
      resetHiscoreNow,
      restoreHiscoreNow: hiscore.restoreNow,
      hiscoreReady: hiscore.ready,
      savedHiscores: hiscore.loadAll,
      crc32: arcadeCrc32,
      identifySet,
      hwtestCrcs,
      applySet,
      ROM_KEY,
    };
  }
});
