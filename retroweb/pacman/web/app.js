// Pac-Man arcade front end. Guest CPU is always 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-pacman";
const TEST = new URLSearchParams(location.search).has("test");
const MSPAC = new URLSearchParams(location.search).get("game") === "mspacman";
const DIP_KEY = MSPAC ? "retroweb.mspacman.dips" : "retroweb.pacman.dips";
const ROM_KEY = MSPAC ? "set-mspacman" : "set";
const DSW1_FACTORY = 0xC9;
const HISCORE_ADDR = 0x4E88;
const HISCORE_LEN = 3;

const SET_PACMAN = {
  "pacman.6e": 0xc1e6ab10,
  "pacman.6f": 0x1a6fb2d4,
  "pacman.6h": 0xbcdd1beb,
  "pacman.6j": 0x817d94e3,
  "pacman.5e": 0x0c944964,
  "pacman.5f": 0x958fedf9,
  "82s123.7f": 0x2fc650bd,
  "82s126.4a": 0x3eb3a8e4,
  "82s126.1m": 0xa9cc86bf,
};

const SET_MSPACMAN = {
  "5e": 0x5c281d01,
  "5f": 0x615af909,
  "u5": 0xf45fbbcd,
  "u6": 0xa90e7000,
  "u7": 0xc82cd714,
};

function concat4(parts) {
  const out = new Uint8Array(0x4000);
  for (let i = 0; i < 4; i++) out.set(parts[i], i * 4096);
  return out;
}

// Chips by board location. 82s126.3m is a timing PROM, ignore it.
const CHIP_RE = /(?:^|[._-])(6e|6f|6h|6j|5e|5f|7f|4a|1m|u5|u6|u7)(?:[^a-z0-9]|$)/i;
const IGNORE_RE = /(?:^|[._-])3m(?:[^a-z0-9]|$)/i;
const SIZE = {
  program: 0x4000, bank: 0x1000, tiles: 0x1000, sprites: 0x1000,
  color: 0x20, lookup: 0x100, wave: 0x100, u5: 0x0800, u6: 0x1000, u7: 0x1000,
};

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (IGNORE_RE.test(n)) return "ignore";
  const m = n.match(CHIP_RE);
  return m ? m[1].toLowerCase() : null;
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
    const banks = ["6e", "6f", "6h", "6j"].map((c) => clip(found[c], SIZE.bank));
    if (banks.every(Boolean)) program = concat4(banks);
  }
  const tiles = clip(found["5e"], SIZE.tiles);
  const sprites = clip(found["5f"], SIZE.sprites);
  const color = clip(found["7f"], SIZE.color);
  const lookup = clip(found["4a"], SIZE.lookup);
  const wave = clip(found["1m"], SIZE.wave);
  if (!program || !tiles || !sprites || !color || !lookup || !wave) return null;

  const u5 = clip(found.u5, SIZE.u5);
  const u6 = clip(found.u6, SIZE.u6);
  const u7 = clip(found.u7, SIZE.u7);
  const aux = !!(u5 && u6 && u7);

  let kind = "user";
  const progCrc = arcadeHexCrc(program);
  if (hwtestCrcs &&
      (progCrc === hwtestCrcs["program.bin"] || progCrc === hwtestCrcs["mspacman-program.bin"]) &&
      arcadeHexCrc(tiles) === hwtestCrcs["pacman.5e"] &&
      arcadeHexCrc(sprites) === hwtestCrcs["pacman.5f"] &&
      arcadeHexCrc(color) === hwtestCrcs["82s123.7f"] &&
      arcadeHexCrc(lookup) === hwtestCrcs["82s126.4a"] &&
      arcadeHexCrc(wave) === hwtestCrcs["82s126.1m"]) {
    kind = "hwtest";
  } else if (
    arcadeCrc32(color) === SET_PACMAN["82s123.7f"] &&
    arcadeCrc32(lookup) === SET_PACMAN["82s126.4a"] &&
    arcadeCrc32(wave) === SET_PACMAN["82s126.1m"] &&
    arcadeCrc32(program.subarray(0, 0x1000)) === SET_PACMAN["pacman.6e"] &&
    arcadeCrc32(program.subarray(0x1000, 0x2000)) === SET_PACMAN["pacman.6f"] &&
    arcadeCrc32(program.subarray(0x2000, 0x3000)) === SET_PACMAN["pacman.6h"] &&
    arcadeCrc32(program.subarray(0x3000, 0x4000)) === SET_PACMAN["pacman.6j"]
  ) {
    if (aux &&
        arcadeCrc32(tiles) === SET_MSPACMAN["5e"] &&
        arcadeCrc32(sprites) === SET_MSPACMAN["5f"] &&
        arcadeCrc32(u5) === SET_MSPACMAN.u5 &&
        arcadeCrc32(u6) === SET_MSPACMAN.u6 &&
        arcadeCrc32(u7) === SET_MSPACMAN.u7) {
      kind = "set-mspacman";
    } else if (
      arcadeCrc32(tiles) === SET_PACMAN["pacman.5e"] &&
      arcadeCrc32(sprites) === SET_PACMAN["pacman.5f"]
    ) {
      kind = "set-pacman";
    }
  }
  return { kind, program, tiles, sprites, color, lookup, wave, u5, u6, u7, aux };
}

function labelFor(kind) {
  if (kind === "hwtest") return "Hardware test ROM";
  if (kind === "set-pacman") return "Midway pacman ROM set";
  if (kind === "set-mspacman") return "Midway mspacman ROM set";
  return "Loaded ROM set";
}

function applyChrome() {
  if (!MSPAC) return;
  document.title = "Ms. Pac-Man Arcade";
  const name = document.querySelector(".pb-name");
  if (name) name.textContent = "Ms. Pac-Man Arcade";
  const h1 = document.querySelector("h1");
  if (h1) h1.textContent = "Ms. Pac-Man Arcade";
  const tag = document.querySelector(".tagline");
  if (tag) {
    tag.textContent = "Midway Ms. Pac-Man (1982) — Pac-Man PCB plus the GCC aux board in the Z80 socket.";
  }
  const pb = document.querySelector(".pb-title");
  if (pb) {
    const icon = pb.querySelector("img");
    pb.textContent = "";
    if (icon) pb.appendChild(icon);
    pb.appendChild(document.createTextNode(" MSPACMAN"));
  }
  const legal = document.getElementById("romLegal");
  if (legal) {
    legal.textContent = "This page ships a hardware self-test ROM, not Namco/Midway Ms. Pac-Man. Those ROMs are still under copyright and are not included or downloaded. Load the ROMs yourself: mspacman.zip, or the loose chips: Pac-Man program 6e, 6f, 6h, 6j, aux board u5, u6, u7, and Ms. Pac-Man 5e and 5f graphics. Each chip is identified by its usual name and checked by size, not a particular CRC. Extra files are ignored. The set stays in this browser.";
  }
  const err = document.getElementById("romErrorCopy");
  if (err) {
    err.textContent = "That isn't a complete set. mspacman.zip needs the Pac-Man program, aux board U5, U6, and U7, tiles, sprites, and the color and wave PROMs. Loose chips use names like pacman.6e, u5, 5e, or 82s123.7f.";
  }
  const ghosts = document.getElementById("dipGhostsRow");
  if (ghosts) ghosts.hidden = true;
}

applyChrome();

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

PacmanArcade().then(async (Module) => {
  const machine = new Module.Machine();
  if (MSPAC) machine.loadMsHwtest();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 288);
  const status = document.getElementById("romStatus");
  const store = arcadeStore(IDB_NAME);
  const audio = arcadeAudio(machine, document.getElementById("mute"), "wsg");

  let usingUserRom = false;
  let programCrc = 0;
  let restoredBytes = null;
  let in0 = 0xFF, in1 = 0xFF;
  let rackTest = false;
  let cocktail = false;
  const keys = {};
  let coinUntil = 0;
  const coinDoor = document.getElementById("coinDoor");

  function applyKeys() {
    let n0 = 0xFF, n1 = 0xFF;
    const down = (k) => keys[k];
    if (down("ArrowUp") || down("KeyW")) n0 &= ~0x01;
    if (down("ArrowLeft") || down("KeyA")) n0 &= ~0x02;
    if (down("ArrowRight") || down("KeyD")) n0 &= ~0x04;
    if (down("ArrowDown") || down("KeyS")) n0 &= ~0x08;
    if (rackTest) n0 &= ~0x10;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil) n0 &= ~0x20;
    if (down("Digit1") || down("Numpad1")) n1 &= ~0x20;
    if (down("Digit2") || down("Numpad2")) n1 &= ~0x40;
    if (cocktail) n1 &= ~0x80;
    in0 = n0; in1 = n1;
    machine.setIn0(in0);
    machine.setIn1(in1);
  }

  function insertCoin() {
    coinUntil = Date.now() + 120;
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
    stick: "4way",
    setKey: (code, down) => {
      keys[code] = down;
      applyKeys();
    },
  });

  window.addEventListener("keydown", (e) => {
    keys[e.code] = true;
    applyKeys();
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(e.code)) e.preventDefault();
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
    paintHiscoreTiles(bytes);
  }

  // Midway #2A9B/#2ABE (cubeman.org mspac.asm): HIGH SCORE digits are only copied
  // to tilemap $43F2 when a score beats TOP. Paint the 6 tiles and color RAM at +$400.
  function paintHiscoreTiles(bytes) {
    if (!machine.setMemByte) return;
    let color = machine.memRead(0x47F2) & 0x3f;
    if (!color) color = machine.memRead(0x47ED) & 0x3f;
    if (!color) color = 0x0f;
    let hl = 0x43F2;
    let c = 4;
    for (let i = 2; i >= 0; i--) {
      const b = bytes[i] & 0xff;
      for (const nib of [(b >> 4) & 0xf, b & 0xf]) {
        let tile;
        if (nib !== 0) { c = 0; tile = nib; }
        else if (c === 0) tile = 0;
        else { tile = 0x40; c--; }
        machine.setMemByte(hl, tile);
        machine.setMemByte(hl + 0x400, color);
        hl--;
      }
    }
  }

  function hiscoreIsZero(b) { return !b[0] && !b[1] && !b[2]; }

  // TOP is BCD in multiples of 10. Attract leftovers at $4E88 look like a counter.
  function isPlausibleTop(b) {
    if (!b || b.length < HISCORE_LEN || hiscoreIsZero(b)) return false;
    for (const x of b) {
      if (((x >> 4) & 0xf) > 9 || (x & 0xf) > 9) return false;
    }
    return (b[0] & 0xf) === 0;
  }

  function bcdScore(b) {
    let n = 0;
    for (let i = 2; i >= 0; i--) n = n * 100 + ((b[i] >> 4) & 0xf) * 10 + (b[i] & 0xf);
    return n;
  }

  // POST's RAM test walks $4E00==1 (Midway #03CE), so wait ~8 s of irq-on frames. Don't require attract.
  function postDone() {
    const st = machine.state();
    return st.irqEnable && st.frames >= 480;
  }

  // Attract re-asserts TOP every frame, mid-game only paint tiles.
  let attractStreak = 0;
  function attractReady() {
    if (!postDone()) {
      attractStreak = 0;
      return false;
    }
    if (machine.ramByte(0x4E00) === 1) attractStreak++;
    else attractStreak = 0;
    return attractStreak >= 30;
  }

  const hiscore = arcadeHiscore({
    store,
    len: HISCORE_LEN,
    active: () => usingUserRom,
    key: () => String(programCrc >>> 0),
    read: readHiscore,
    write: writeHiscore,
    ready: postDone,
    restore: (saved) => {
      if (saved && isPlausibleTop(saved)) {
        restoredBytes = Array.from(saved).slice(0, HISCORE_LEN);
        writeHiscore(restoredBytes);
      } else {
        const live = readHiscore();
        if (isPlausibleTop(live)) restoredBytes = live;
      }
      return restoredBytes;
    },
    shouldSave: (cur) => isPlausibleTop(cur) && !(restoredBytes && bcdScore(cur) <= bcdScore(restoredBytes)),
    onSaved: (bytes) => { restoredBytes = Array.from(bytes).slice(0, HISCORE_LEN).map((x) => x & 0xff); },
  });

  async function resetHiscoreNow() {
    if (!usingUserRom) return;
    restoredBytes = null;
    attractStreak = 0;
    await hiscore.forget();
    machine.reset();
    applyDips();
  }
  const syncHiscoreResetBtn = arcadeHiscoreReset(() => usingUserRom, resetHiscoreNow);

  const dipCoinage = document.getElementById("dipCoinage");
  const dipLives = document.getElementById("dipLives");
  const dipBonus = document.getElementById("dipBonus");
  const dipDifficulty = document.getElementById("dipDifficulty");
  const dipGhosts = document.getElementById("dipGhosts");
  const dipCabinet = document.getElementById("dipCabinet");
  const dipRack = document.getElementById("dipRack");

  function encodeDsw1() {
    let v = (Number(dipCoinage.value) | Number(dipLives.value) | Number(dipBonus.value) |
            Number(dipDifficulty.value) | Number(dipGhosts.value)) & 0xff;
    // Ms. Pac-Man has no ghost-names pad, DSW1 bit 7 reads 1.
    if (MSPAC) v |= 0x80;
    return v;
  }

  function applyDips() {
    machine.setDsw1(encodeDsw1());
    rackTest = dipRack.value === "1";
    cocktail = dipCabinet.value === "1";
    applyKeys();
  }

  const dips = arcadePrefs(DIP_KEY);
  function saveDips() {
    dips.save({
      dsw1: encodeDsw1(),
      rackTest: dipRack.value === "1",
      cocktail: dipCabinet.value === "1",
    });
    applyDips();
  }

  function loadDips() {
    let dsw1 = DSW1_FACTORY;
    const s = dips.load();
    if (s) {
      if (typeof s.dsw1 === "number") dsw1 = s.dsw1 & 0xff;
      dipRack.value = s.rackTest ? "1" : "0";
      dipCabinet.value = s.cocktail ? "1" : "0";
    }
    dipCoinage.value = String(dsw1 & 0x03);
    dipLives.value = String(dsw1 & 0x0c);
    dipBonus.value = String(dsw1 & 0x30);
    dipDifficulty.value = String(dsw1 & 0x40);
    dipGhosts.value = String(dsw1 & 0x80);
    applyDips();
  }

  [dipCoinage, dipLives, dipBonus, dipDifficulty, dipGhosts, dipCabinet, dipRack]
    .forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.tiles || !set.sprites || !set.color || !set.lookup || !set.wave)
      throw new Error("incomplete ROM set");
    if (MSPAC) {
      if (!set.aux) throw new Error("Ms. Pac-Man aux board needs U5, U6, and U7");
      machine.loadMsPacmanSet(set.program, set.tiles, set.sprites, set.color, set.lookup, set.wave,
        set.u5, set.u6, set.u7);
    } else {
      machine.loadRomSet(set.program, set.tiles, set.sprites, set.color, set.lookup, set.wave);
    }
    usingUserRom = set.kind !== "hwtest";
    programCrc = arcadeCrc32(set.program);
    if (set.aux) programCrc = arcadeCrc32(set.u5) ^ arcadeCrc32(set.u6) ^ arcadeCrc32(set.u7) ^ programCrc;
    hiscore.rearm();
    restoredBytes = null;
    attractStreak = 0;
    syncHiscoreResetBtn();
    setStatus(label);
  }

  let hwtestCrcs = null;
  try { hwtestCrcs = await (await fetch("roms/crc.json")).json(); } catch {}

  function loadBuiltInRom() {
    if (MSPAC) machine.loadMsHwtest();
    else machine.loadHwtest();
    usingUserRom = false;
    hiscore.rearm();
    restoredBytes = null;
    attractStreak = 0;
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
    afterRun: () => {
      const ready = attractReady();
      if (!usingUserRom || !hiscore.restored || !restoredBytes) return;
      const live = readHiscore();
      if (isPlausibleTop(live) && bcdScore(live) > bcdScore(restoredBytes)) {
        restoredBytes = live;
        paintHiscoreTiles(restoredBytes);
      } else if (ready) {
        writeHiscore(restoredBytes);
      } else {
        paintHiscoreTiles(restoredBytes);
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
      readHiscore,
      writeHiscore,
      paintHiscoreTiles,
      saveHiscoreNow: hiscore.saveNow,
      maybeSaveHiscore: hiscore.maybeSave,
      resetHiscoreNow,
      hiscoreTiles: () => {
        const t = [];
        for (let a = 0x43F2; a >= 0x43ED; a--) t.push(machine.memRead(a) & 0xff);
        return t;
      },
      get hiscoreRestored() { return hiscore.restored; },
      get mode() { return machine.ramByte(0x4E00); },
      get hiscoreKey() { return String(programCrc >>> 0); },
      restoreHiscoreNow: hiscore.restoreNow,
      hiscoreReady: hiscore.ready,
      savedHiscores: hiscore.loadAll,
      crc32: arcadeCrc32,
      identifySet,
      hwtestCrcs,
      applySet,
      MSPAC,
      ROM_KEY,
      loadSavedHiscores: hiscore.loadAll,
      wipeStorage: async () => {
        await store.del("set");
        await store.del("set-mspacman");
        await store.del("hiscores");
      },
    };
  }
});
