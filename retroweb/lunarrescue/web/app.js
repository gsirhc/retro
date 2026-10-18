// Lunar Rescue arcade front end. Guest CPU is always 1.9968 MHz vs wall clock.

const CPU_HZ = 1_996_800;
const IDB_NAME = "retroweb-lunarrescue";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.lunarrescue.dips";
const ROM_KEY = "set";
// MAME hiscore.dat lrescue: three RAM blocks, and what they hold once init has run.
const HISCORE_BLOCKS = [[0x20CF, 10], [0x20DB, 1], [0x20F4, 2]];
const HISCORE_LEN = 13;
const FACTORY = [0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x0A, 0x00, 0x05];
const CHIPS = ["1", "2", "3", "4", "5", "6"];

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "7643-1.cpu") return "prom";
  for (const c of CHIPS) {
    if (n === `lrescue.${c}`) return c;
  }
  return null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || found[role]) continue;
    found[role] = data;
  }
  const chips = CHIPS.map((c) => clip(found[c], 0x800));
  const prom = clip(found.prom, 0x400);
  if (!chips.every(Boolean) || !prom) return null;
  const program = new Uint8Array(0x2000);
  const programHi = new Uint8Array(0x1000);
  chips.forEach((c, i) => (i < 4 ? program.set(c, i * 0x800) : programHi.set(c, (i - 4) * 0x800)));
  const kind = hwtestCrcs && arcadeHexCrc(chips[0]) === hwtestCrcs["lrescue.1"] ? "hwtest" : "user";
  return { kind, program, programHi, prom };
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

LunarRescueArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 260);
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "lunarrescue");

  let usingUserRom = false;
  let programCrc = 0;
  const keys = {};
  let coinUntil = 0;
  const coinDoor = document.getElementById("coinDoor");
  const dipLives = document.getElementById("dipLives");

  function applyKeys() {
    // Upright PCB: P1 fire/left/right on IN1 and IN2 bits 4-6, IN0 tied high. COIN is active-low.
    const down = (k) => keys[k];
    let ctl = 0;
    if (down("Space") || down("KeyZ")) ctl |= 0x10;
    if (down("ArrowLeft") || down("KeyA")) ctl |= 0x20;
    if (down("ArrowRight") || down("KeyD")) ctl |= 0x40;
    let n1 = 0x89 | ctl;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil) n1 &= ~0x01;
    if (down("Digit2") || down("Numpad2")) n1 |= 0x02;
    if (down("Digit1") || down("Numpad1")) n1 |= 0x04;
    const n2 = (Number(dipLives.value) & 0x03) | ctl;
    machine.setIn0(0xFF);
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
    img.data.set(machine.frameBuffer());
    ctx.putImageData(img, 0, 0);
  }

  function setStatus(t) { status.textContent = t; }

  function readHiscore() {
    const b = [];
    for (const [addr, n] of HISCORE_BLOCKS) {
      for (let i = 0; i < n; i++) b.push(machine.ramByte(addr + i) & 0xff);
    }
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    let k = 0;
    for (const [addr, n] of HISCORE_BLOCKS) {
      for (let i = 0; i < n; i++) machine.setRamByte(addr + i, bytes[k++] & 0xff);
    }
  }

  function hiscoreIsFactory(b) { return FACTORY.every((v, i) => b[i] === v); }

  // Restore once init has written the factory table, as MAME's hiscore plugin waits for it.
  function postDone() {
    return machine.state().frames >= 30 && hiscoreIsFactory(readHiscore());
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
    dips.save({ lives: Number(dipLives.value) });
    applyKeys();
  }

  function loadDips() {
    const s = dips.load();
    if (s) {
      if (s.lives !== undefined) dipLives.value = String(s.lives);
    }
    applyKeys();
  }

  dipLives.addEventListener("change", saveDips);
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.programHi || !set.prom) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.programHi, set.prom);
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
