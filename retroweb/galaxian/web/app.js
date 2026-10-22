// Galaxian arcade front end. Guest CPU is always 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-galaxian";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.galaxian.dips";
const ROM_KEY = "set";
const HISCORE_ADDR = 0x40A8;
const HISCORE_LEN = 3;

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
  if (n === "gfx.bin") return "gfx";
  if (/6l\.bpr|6l\.bin/.test(n)) return "prom";
  if (n === "galmidw.u" || n === "galaxian.u") return "u";
  if (n === "galmidw.v" || n === "galaxian.v") return "v";
  if (n === "galmidw.w" || n === "galaxian.w") return "w";
  if (n === "galmidw.y" || n === "galaxian.y") return "y";
  if (n === "7l" || n === "7l.bin") return "7l";
  if (n === "7f.bin") return "7f";
  if (n === "7j.bin") return "7j";
  if (n === "1h.bin") return "1h";
  if (n === "1k.bin") return "1k";
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
    const chips = ["u", "v", "w", "y", "7l"].map((k) => clip(found[k], 0x800));
    if (chips.every(Boolean)) program = concat(chips.concat([new Uint8Array(0x1800)]), 0x4000);
  }
  if (!program) {
    const a = clip(found["7f"], 0x1000);
    const b = clip(found["7j"], 0x1000);
    const c = clip(found["7l"], 0x800);
    if (a && b && c) program = concat([a, b, c, new Uint8Array(0x1800)], 0x4000);
  }
  let gfx = clip(found.gfx, 0x1000);
  if (!gfx) {
    const a = clip(found["1h"], 0x800);
    const b = clip(found["1k"], 0x800);
    if (a && b) gfx = concat([a, b], 0x1000);
  }
  const color = clip(found.prom, 0x20);
  if (!program || !gfx || !color) return null;

  let kind = "user";
  if (hwtestCrcs &&
      arcadeHexCrc(program) === hwtestCrcs["program.bin"] &&
      arcadeHexCrc(gfx) === hwtestCrcs["gfx.bin"] &&
      arcadeHexCrc(color) === hwtestCrcs["6l.bpr"]) {
    kind = "hwtest";
  }
  return { kind, program, gfx, color };
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

GalaxianArcade().then(async (Module) => {
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
  const dipBonus = document.getElementById("dipBonus");

  function applyKeys() {
    // Active-high Namco ports. DIP bits sit in IN1 7:6, IN2 2:0, IN0 bit 5.
    let n0 = Number(dipCabinet.value) & 0x20;
    let n1 = Number(dipCoinage.value) & 0xC0;
    let n2 = (Number(dipBonus.value) & 0x03) | (Number(dipLives.value) & 0x04);
    const down = (k) => keys[k];
    if (down("ArrowLeft") || down("KeyA")) n0 |= 0x04;
    if (down("ArrowRight") || down("KeyD")) n0 |= 0x08;
    if (down("Space") || down("KeyZ")) n0 |= 0x10;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil) n0 |= 0x01;
    if (down("Digit1") || down("Numpad1")) n1 |= 0x01;
    if (down("Digit2") || down("Numpad2")) n1 |= 0x02;
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
    stick: "2way",
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
    for (let i = 0; i < HISCORE_LEN; i++) b.push(machine.ramByte(HISCORE_ADDR + i) & 0xff);
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    for (let i = 0; i < HISCORE_LEN; i++) machine.setRamByte(HISCORE_ADDR + i, bytes[i] & 0xff);
  }

  function hiscoreIsFactory(b) { return !b[0] && !b[1] && !b[2]; }

  // Self-test clears work RAM ($1B8A) before it turns NMI on ($1BC7); nothing sets HI-SCORE after that.
  function postDone() {
    return machine.state().nmiEnable && hiscoreIsFactory(readHiscore());
  }

  const hiscore = arcadeHiscore({
    store,
    len: HISCORE_LEN,
    active: () => usingUserRom,
    key: () => String(programCrc >>> 0),
    read: readHiscore,
    write: writeHiscore,
    isFactory: hiscoreIsFactory,
    ready: postDone,
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
      bonus: Number(dipBonus.value),
    });
    applyKeys();
  }

  function loadDips() {
    const s = dips.load();
    if (s) {
      if (s.lives !== undefined) dipLives.value = String(s.lives);
      if (s.coinage !== undefined) dipCoinage.value = String(s.coinage);
      if (s.cabinet !== undefined) dipCabinet.value = String(s.cabinet);
      if (s.bonus !== undefined) dipBonus.value = String(s.bonus);
    }
    applyKeys();
  }

  [dipCoinage, dipLives, dipCabinet, dipBonus].forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.gfx || !set.color)
      throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.gfx, set.color);
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
