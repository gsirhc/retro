// Midway Galaga front end. Each Z80 stays at 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-galaga";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.galaga.dips";
const ROM_KEY = "set";
// Work RAM table, then the score strip.
const HISCORE = [
  { addr: 0x8A20, len: 0x2D },
  { addr: 0x83ED, len: 6 },
];
const HISCORE_LEN = 0x2D + 6;
// Midway reset table starts as the character bytes for a 20000 score.
const FACTORY_PREFIX = [0x00, 0x00, 0x00, 0x00, 0x02, 0x24];

function exact(bytes, n) {
  if (!bytes || bytes.length !== n) return null;
  return bytes;
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

// Midway galagamw socket names. CRC only tells the self-test from a user set.
function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  const roles = {
    "main.bin": "main",
    "sub.bin": "sub",
    "sound.bin": "sound",
    "3200a.bin": "a", "3200a": "a",
    "3300b.bin": "b", "3300b": "b",
    "3400c.bin": "c", "3400c": "c",
    "3500d.bin": "d", "3500d": "d",
    "3600e.bin": "sub", "3600e": "sub",
    "3700g.bin": "sound", "3700g": "sound",
    "2600j.bin": "tiles", "2600j": "tiles",
    "sprites.bin": "sprites",
    "2800l.bin": "spr0", "2800l": "spr0",
    "2700k.bin": "spr1", "2700k": "spr1",
    "prom-5.5n": "palette",
    "prom-4.2n": "charLut",
    "prom-3.1c": "spriteLut",
    "prom-1.1d": "wave",
    "51xx.bin": "mcu51",
    "54xx.bin": "mcu54",
  };
  return roles[n] || null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || found[role]) continue;
    found[role] = data;
  }
  let main = exact(found.main, 0x4000);
  if (!main) {
    const chips = ["a", "b", "c", "d"].map((k) => exact(found[k], 0x1000));
    if (chips.every(Boolean)) main = concat(chips, 0x4000);
  }
  const sub = exact(found.sub, 0x1000);
  const sound = exact(found.sound, 0x1000);
  const tiles = exact(found.tiles, 0x1000);
  let sprites = exact(found.sprites, 0x2000);
  if (!sprites) {
    const a = exact(found.spr0, 0x1000);
    const b = exact(found.spr1, 0x1000);
    if (a && b) sprites = concat([a, b], 0x2000);
  }
  const palette = exact(found.palette, 0x20);
  const charLut = exact(found.charLut, 0x100);
  const spriteLut = exact(found.spriteLut, 0x100);
  const wave = exact(found.wave, 0x100);
  if (!main || !sub || !sound || !tiles || !sprites || !palette || !charLut || !spriteLut || !wave) {
    return null;
  }
  const mcu51 = exact(found.mcu51, 0x400);
  const mcu54 = exact(found.mcu54, 0x400);
  const set = { kind: "user", main, sub, sound, tiles, sprites, palette, charLut, spriteLut, wave, mcu51, mcu54 };
  if (hwtestCrcs &&
      arcadeHexCrc(main) === hwtestCrcs["main.bin"] &&
      arcadeHexCrc(sub) === hwtestCrcs["sub.bin"] &&
      arcadeHexCrc(sound) === hwtestCrcs["sound.bin"] &&
      arcadeHexCrc(tiles) === hwtestCrcs["2600j.bin"] &&
      arcadeHexCrc(sprites) === hwtestCrcs["sprites.bin"] &&
      arcadeHexCrc(palette) === hwtestCrcs["prom-5.5n"] &&
      arcadeHexCrc(charLut) === hwtestCrcs["prom-4.2n"] &&
      arcadeHexCrc(spriteLut) === hwtestCrcs["prom-3.1c"] &&
      arcadeHexCrc(wave) === hwtestCrcs["prom-1.1d"]) {
    set.kind = "hwtest";
  }
  return set;
}

function hleLine(set) {
  const parts = [];
  if (!set.mcu51) parts.push("51xx.bin missing — 51XX fell back to HLE");
  if (!set.mcu54) parts.push("54xx.bin missing — 54XX fell back to HLE");
  return parts.join(". ");
}

function statusFor(set, stored) {
  if (set.kind === "hwtest") return stored ? "Loaded stored ROM set" : "Running test ROM";
  const hle = hleLine(set);
  if (hle) return hle;
  return stored ? "Loaded stored ROM set" : "Loaded ROM set";
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

GalagaArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 288);
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "ay");

  let usingUserRom = false;
  let programCrc = 0;
  const keys = {};
  const coinDoor = document.getElementById("coinDoor");
  const dipCredits = document.getElementById("dipCredits");
  const dipDifficulty = document.getElementById("dipDifficulty");
  const dipDemo = document.getElementById("dipDemo");
  const dipFreeze = document.getElementById("dipFreeze");
  const dipRack = document.getElementById("dipRack");
  const dipCabinet = document.getElementById("dipCabinet");
  const dipCoinage = document.getElementById("dipCoinage");
  const dipBonus = document.getElementById("dipBonus");
  const dipLives = document.getElementById("dipLives");
  const dipEls = [dipCredits, dipDifficulty, dipDemo, dipFreeze, dipRack, dipCabinet, dipCoinage, dipBonus, dipLives];

  function padBits() {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const p = pads && pads[0];
    if (!p) return { left: false, right: false, fire: false, start: false, coin: false };
    const ax = p.axes && p.axes.length ? p.axes[0] : 0;
    const btn = (i) => !!(p.buttons && p.buttons[i] && p.buttons[i].pressed);
    return {
      left: ax < -0.5 || btn(14),
      right: ax > 0.5 || btn(15),
      fire: btn(0),
      start: btn(9),
      coin: btn(8),
    };
  }

  function applyKeys() {
    // Active-high contacts; the 51XX sees them inverted. Cocktail player 2 is unmapped.
    const down = (k) => keys[k];
    const pad = padBits();
    let n0 = 0;
    let n1 = 0;
    if (down("ArrowLeft") || down("KeyA") || pad.left) n0 |= 0x08;
    if (down("ArrowRight") || down("KeyD") || pad.right) n0 |= 0x02;
    if (down("Space") || down("KeyZ") || pad.fire) n1 |= 0x01;
    if (down("Digit5") || down("Numpad5") || pad.coin) n1 |= 0x10;
    if (down("Digit6") || down("Numpad6")) n1 |= 0x20;
    if (down("Digit1") || down("Numpad1") || pad.start) n1 |= 0x04;
    if (down("Digit2") || down("Numpad2")) n1 |= 0x08;
    machine.setIn0(n0);
    machine.setIn1(n1);
    // DIP 6J switch 7 is open, reads 1. The sub ROM tests $6806 bit 1 at $0ECA.
    const a = (Number(dipCredits.value) & 0x01) |
            (Number(dipDifficulty.value) & 0x06) |
            (Number(dipDemo.value) & 0x08) |
            (Number(dipFreeze.value) & 0x10) |
            (Number(dipRack.value) & 0x20) |
            0x40 |
            (Number(dipCabinet.value) & 0x80);
    const b = (Number(dipCoinage.value) & 0x07) |
            (Number(dipBonus.value) & 0x38) |
            (Number(dipLives.value) & 0xC0);
    machine.setDswA(a);
    machine.setDswB(b);
  }

  function insertCoin(which) {
    const code = which === "2" ? "Digit6" : "Digit5";
    keys[code] = true;
    applyKeys();
    window.setTimeout(() => {
      keys[code] = false;
      applyKeys();
    }, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
    audio.ensure().catch(() => {});
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", () => insertCoin(el.getAttribute("data-coin")));
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
    for (const r of HISCORE) {
      for (let i = 0; i < r.len; i++) b.push(machine.ramByte(r.addr + i) & 0xff);
    }
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    let o = 0;
    for (const r of HISCORE) {
      for (let i = 0; i < r.len; i++) machine.setRamByte(r.addr + i, bytes[o++] & 0xff);
    }
  }

  function hiscoreIsFactory(b) {
    if (b.every((x) => !x)) return true;
    return FACTORY_PREFIX.every((x, i) => b[i] === x);
  }

  // POST leaves junk at $8A20 until it writes the 20000 table, so wait for that before poking IndexedDB.
  function factoryTableReady() {
    for (let i = 0; i < FACTORY_PREFIX.length; i++) {
      if ((machine.ramByte(0x8A20 + i) & 0xff) !== FACTORY_PREFIX[i]) return false;
    }
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
    ready: factoryTableReady,
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
      credits: Number(dipCredits.value),
      difficulty: Number(dipDifficulty.value),
      demo: Number(dipDemo.value),
      freeze: Number(dipFreeze.value),
      rack: Number(dipRack.value),
      cabinet: Number(dipCabinet.value),
      coinage: Number(dipCoinage.value),
      bonus: Number(dipBonus.value),
      lives: Number(dipLives.value),
    });
    applyKeys();
  }

  function loadDips() {
    const s = dips.load();
    if (s) {
      const map = {
        credits: dipCredits, difficulty: dipDifficulty, demo: dipDemo,
        freeze: dipFreeze, rack: dipRack, cabinet: dipCabinet,
        coinage: dipCoinage, bonus: dipBonus, lives: dipLives,
      };
      for (const [k, el] of Object.entries(map)) {
        if (s[k] !== undefined) el.value = String(s[k]);
      }
    }
    applyKeys();
  }

  dipEls.forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function orEmpty(bytes) { return bytes && bytes.length ? bytes : new Uint8Array(0); }

  function applySet(set, stored) {
    if (!set || !set.main || !set.sub || !set.sound || !set.tiles || !set.sprites ||
        !set.palette || !set.charLut || !set.spriteLut || !set.wave) {
      throw new Error("incomplete ROM set");
    }
    machine.loadRomSet(set.main, set.sub, set.sound, set.tiles, set.sprites,
      set.palette, set.charLut, set.spriteLut, set.wave,
      orEmpty(set.mcu51), orEmpty(set.mcu54));
    usingUserRom = set.kind !== "hwtest";
    programCrc = arcadeCrc32(set.main);
    hiscore.rearm();
    syncHiscoreResetBtn();
    setStatus(statusFor(set, stored));
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
    apply: applySet,
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
    beforeRun: applyKeys,
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
