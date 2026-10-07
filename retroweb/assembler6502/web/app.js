// 6502 Assembler front end. Terminal profiles and the per-frame CPU/serial
// pump follow altair8800/web/app.js; the left column holds the board's controls.

function fail(msg) {
  console.error(msg);
  const screen = document.getElementById("screen");
  if (screen) {
    screen.innerHTML =
      '<pre style="color:#ff6b6b;white-space:pre-wrap;padding:12px;margin:0;' +
      'font:13px/1.5 ui-monospace,monospace">' +
      String(msg).replace(/[<>&]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;" }[c])) +
      "</pre>";
  }
}

/* v8 ignore start -- last-resort global error nets */
window.addEventListener("error", (e) => fail(e.message + "\n" + (e.error?.stack || "")));
window.addEventListener("unhandledrejection", (e) => fail("promise rejected: " + (e.reason?.stack || e.reason)));
/* v8 ignore stop */

async function boot() {
  if (typeof Terminal !== "function") return fail("xterm.js did not load.");
  if (typeof FitAddon === "undefined") return fail("xterm-addon-fit did not load.");
  if (typeof CgOac6502 !== "function") return fail("cgoac6502.js did not load. Run `make wasm` in web/.");
  if (typeof CGOAC_ENTRYPOINTS === "undefined") return fail("roms/entrypoints.js did not load. Run `make roms` in web/.");

  // ---- terminal ----
  const term = new Terminal({ cursorBlink: true });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  const screenEl = document.getElementById("screen");
  const bezelEl = document.getElementById("bezel");
  const monitorEl = document.getElementById("monitor");
  // declared early: term.focus() fires focusin synchronously and isRunning reads poweredOn
  let poweredOn = true;
  const REF_W = 760, REF_H = 420;
  term.open(screenEl);
  fit.fit();

  // Web-UI only (no hardware state): focus hint and fullscreen. poweredOn is
  // passed as a predicate so it's read live.
  const isRunning = () => poweredOn;
  const updateFocusHint = initFocusHint(screenEl, isRunning);
  // #screen has inline pixel sizes that beat fullscreen.css, so sizeScreen()
  // grows the font to fill the bezel on each fullscreen transition
  initFullscreen({
    bezelEl,
    screenEl,
    fullscreenBtn: document.getElementById("fullscreenBtn"),
    fsEscHint: document.getElementById("fsEscHint"),
    fsEscHintOkBtn: document.getElementById("fsEscHintOk"),
    escBtn: document.getElementById("escBtn"),
    // no scancode keyboard: Escape takes the same handleTermData() path as a keypress
    sendEscape: () => handleTermData("\x1b"),
    isRunning,
    onFullscreenChange: () => requestAnimationFrame(sizeScreen),
  });

  screenEl.addEventListener("wheel", (e) => {
    if (!monitorEl.classList.contains("scrolls")) e.stopImmediatePropagation();
  }, { capture: true });

  // base font size while fullscreen, so enlargement doesn't compound on resize
  let fsBaseFontSize = null;
  function sizeScreen() {
    try {
      // undo fullscreen enlargement before measuring the natural box
      if (fsBaseFontSize != null) term.options.fontSize = fsBaseFontSize;

      screenEl.style.width = REF_W + "px";
      screenEl.style.height = REF_H + "px";
      fit.fit();
      if (!term.cols || !term.rows) return;
      const cw = REF_W / term.cols, ch = REF_H / term.rows;
      const panel = screenEl.closest(".panel");
      const avail = panel ? panel.clientWidth - 32 : REF_W;
      const cols = Math.max(40, Math.floor(avail / cw));
      screenEl.style.width = Math.round(cols * cw) + "px";
      screenEl.style.height = Math.round(24 * ch) + "px";
      term.resize(cols, 24);
      term.refresh(0, term.rows - 1);

      // 4. fullscreen: grow the font (a CSS transform would blur), then refit.
      // #bezel's box is the available size, since .panel sits outside the fullscreen subtree.
      const fs = (document.fullscreenElement || document.webkitFullscreenElement) === bezelEl;
      if (fs) {
        if (fsBaseFontSize == null) fsBaseFontSize = term.options.fontSize;
        const naturalW = screenEl.offsetWidth, naturalH = screenEl.offsetHeight;
        const bs = getComputedStyle(bezelEl);
        const availW = bezelEl.clientWidth - parseFloat(bs.paddingLeft) - parseFloat(bs.paddingRight);
        const availH = bezelEl.clientHeight - parseFloat(bs.paddingTop) - parseFloat(bs.paddingBottom);
        const scale = naturalW && naturalH ? Math.min(availW / naturalW, availH / naturalH) : 1;
        if (scale > 1) {
          screenEl.style.width = availW + "px";
          screenEl.style.height = availH + "px";
          term.options.fontSize = Math.round(fsBaseFontSize * scale);
          fit.fit();
          term.refresh(0, term.rows - 1);
        }
      } else {
        fsBaseFontSize = null;
      }
    } catch {}
  }
  addEventListener("resize", sizeScreen);

  // ---- terminal profiles ----
  function dumbFilter() {
    let state = "ground";
    const KEEP_C0 = new Set([0x07, 0x08, 0x09, 0x0a, 0x0d]);
    return (bytes) => {
      const out = [];
      for (const b of bytes) {
        if (state === "esc") { state = (b === 0x5b) ? "csi" : "ground"; continue; }
        if (state === "csi") { if (b >= 0x40 && b <= 0x7e) state = "ground"; continue; }
        if (b === 0x1b) { state = "esc"; continue; }
        if (b < 0x20 && !KEEP_C0.has(b)) continue;
        out.push(b);
      }
      return out;
    };
  }
  function vt52Filter() {
    let state = "ground", row = 0;
    const CSI_LETTER = new Set(["A", "B", "C", "D", "H", "J", "K"]);
    return (bytes) => {
      const out = [];
      const push = (s) => { for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i)); };
      for (const b of bytes) {
        const c = String.fromCharCode(b);
        if (state === "Y1") { row = b - 0x20; state = "Y2"; continue; }
        if (state === "Y2") { push(`\x1b[${row + 1};${b - 0x20 + 1}H`); state = "ground"; continue; }
        if (state === "esc") {
          state = "ground";
          if (CSI_LETTER.has(c)) { push("\x1b[" + c); continue; }
          if (c === "I") { push("\x1bM"); continue; }
          if (c === "Y") { state = "Y1"; continue; }
          continue;
        }
        if (b === 0x1b) { state = "esc"; continue; }
        out.push(b);
      }
      return out;
    };
  }
  function adm3aFilter() {
    let state = "ground", row = 0;
    return (bytes) => {
      const out = [];
      const push = (s) => { for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i)); };
      for (const b of bytes) {
        if (state === "eq1") { row = b - 0x20; state = "eq2"; continue; }
        if (state === "eq2") { push(`\x1b[${row + 1};${b - 0x20 + 1}H`); state = "ground"; continue; }
        if (state === "esc") { state = (b === 0x3d) ? "eq1" : "ground"; continue; }
        if (b === 0x1b) { state = "esc"; continue; }
        if (b === 0x0b) { push("\x1b[A"); continue; }
        if (b === 0x0c) { push("\x1b[C"); continue; }
        if (b === 0x1a) { push("\x1b[H\x1b[2J"); continue; }
        out.push(b);
      }
      return out;
    };
  }

  const TERM_PROFILES = {
    modern: { label: "Modern (xterm)",
      font: 'ui-monospace, Menlo, Consolas, "DejaVu Sans Mono", monospace',
      size: 15, fg: "#33ff88", bg: "#000000", dim: "#1c8f52", br: "#8affc0",
      crt: "none", cursor: "block", blink: true, glow: 0 },
    vt100g: { label: "DEC VT100 · green",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#39ff41", bg: "#0a140a", dim: "#1c8c1c", br: "#a6ffa6",
      crt: "scan", cursor: "block", blink: true, glow: 2 },
    vt100a: { label: "DEC VT100 · amber",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#ffb32b", bg: "#180d00", dim: "#a8701a", br: "#ffd77e",
      crt: "scan", cursor: "block", blink: true, glow: 2 },
    vt52: { label: "DEC VT52",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#4dff4d", bg: "#061006", dim: "#1c8c1c", br: "#b6ffb6",
      crt: "scanheavy", cursor: "block", blink: false, glow: 3, filter: vt52Filter },
    adm3a: { label: "Lear Siegler ADM-3A",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#39ff9c", bg: "#03100b", dim: "#1c8c5c", br: "#a6ffce",
      crt: "scan", cursor: "underline", blink: true, glow: 2, filter: adm3aFilter },
    glasstty: { label: "Glass TTY",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#c8ffc8", bg: "#020802", dim: "#5c9c5c", br: "#ecffec",
      crt: "scan", cursor: "block", blink: true, glow: 2, filter: dumbFilter },
    tty33: { label: "Teletype ASR-33",
      font: '"Courier Prime", "Courier New", monospace', size: 15,
      fg: "#242424", bg: "#efe8d6", dim: "#7a7261", br: "#000000",
      crt: "paper", cursor: "underline", blink: false, glow: 0,
      scrollback: 5000, filter: dumbFilter, forceCaps: true },
  };

  let termFilter = null;
  function applyProfile(key) {
    const p = TERM_PROFILES[key] || TERM_PROFILES.modern;
    termFilter = p.filter ? p.filter() : null;
    // the ASR-33 is mechanically incapable of lowercase -- not the user's choice
    caps.disabled = !!p.forceCaps;
    if (p.forceCaps) caps.checked = true;
    term.options.fontFamily = p.font;
    term.options.fontSize = p.size;
    term.options.cursorStyle = p.cursor;
    term.options.cursorBlink = p.blink;
    term.options.scrollback = p.scrollback || 0;
    term.options.theme = {
      background: p.bg, foreground: p.fg, cursor: p.fg, cursorAccent: p.bg,
      selectionBackground: p.fg + "44",
      black: p.bg, red: p.fg, green: p.fg, yellow: p.fg, blue: p.fg,
      magenta: p.fg, cyan: p.fg, white: p.fg,
      brightBlack: p.dim, brightRed: p.br, brightGreen: p.br, brightYellow: p.br,
      brightBlue: p.br, brightMagenta: p.br, brightCyan: p.br, brightWhite: p.br,
    };
    const noCrt = p.crt === "none";
    monitorEl.classList.toggle("crt", !noCrt);
    bezelEl.className = "bezel" + (noCrt ? "" : " crt-" + p.crt);
    monitorEl.classList.toggle("amber", key === "vt100a");
    monitorEl.classList.toggle("scrolls", !!p.scrollback);
    screenEl.style.setProperty("--glow", (noCrt ? 0 : p.glow) + "px");
    try { localStorage.setItem("cgoac6502.term", key); } catch {}
    // resolves after the resize, so boot() can await it before the ROM writes output
    return (document.fonts ? document.fonts.ready : Promise.resolve())
      .then(() => new Promise((resolve) => setTimeout(() => { sizeScreen(); resolve(); }, 30)));
  }
  function bell() {
    bezelEl.classList.add("bell-flash");
    setTimeout(() => bezelEl.classList.remove("bell-flash"), 90);
  }
  term.onBell(bell);

  const termSelect = document.getElementById("termProfile");
  let savedTerm = "modern";
  try { savedTerm = localStorage.getItem("cgoac6502.term") || "modern"; } catch {}
  termSelect.value = savedTerm in TERM_PROFILES ? savedTerm : "modern";

  // CAPS LOCK defaults on since bios.s FORCE_UPPER expects uppercase; the
  // ASR-33 forces it. Declared before the first applyProfile(), which reads `caps`.
  const caps = document.getElementById("caps");
  try { caps.checked = localStorage.getItem("cgoac6502.caps") !== "0"; } catch {}
  caps.addEventListener("change", () => {
    try { localStorage.setItem("cgoac6502.caps", caps.checked ? "1" : "0"); } catch {}
  });

  // Load the local VT323/Courier Prime faces before first paint and await it:
  // boot() must not start the ROM until the terminal is sized against the final
  // font, or Wozmon's banner is written at stale cols/rows and corrupted by the
  // font swap.
  await Promise.all([
    document.fonts?.load('20px "VT323"'),
    document.fonts?.load('15px "Courier Prime"'),
    /* v8 ignore next -- font-load rejection is swallowed */
  ].filter(Boolean)).catch(() => {});
  await applyProfile(termSelect.value);
  termSelect.addEventListener("change", () => applyProfile(termSelect.value));

  // ---- floating popups (Save/Load, Help) ----
  // Draggable like altair8800's pgDrag/pgFloat, over static markup (index.html)
  // with any number of trigger buttons per popup.
  function wireFloatPopup(popupId, posKey, triggerIds) {
    const popup = document.getElementById(popupId);
    const drag = popup.querySelector(".fp-drag");
    let float = { left: null, top: null };
    try {
      const s = JSON.parse(localStorage.getItem(posKey) || "null");
      if (s && Number.isFinite(s.left)) float = s;
    } catch {}
    function place() {
      if (float.left == null) return;
      popup.style.left = float.left + "px";
      popup.style.top = float.top + "px";
      popup.style.right = "auto";
    }
    place();
    for (const id of triggerIds) {
      document.getElementById(id).addEventListener("click", () => { popup.hidden = false; place(); });
    }
    popup.querySelector(".fp-x").addEventListener("click", () => { popup.hidden = true; });
    /* v8 ignore start -- drag body runs inside synthesized mouse events, not attributed by coverage */
    drag.addEventListener("mousedown", (e) => {
      if (e.target.closest(".fp-x")) return;
      e.preventDefault();
      const r = popup.getBoundingClientRect();
      const ox = e.clientX - r.left, oy = e.clientY - r.top;
      const move = (ev) => {
        float.left = Math.max(4, Math.min(window.innerWidth - 64, ev.clientX - ox));
        float.top = Math.max(4, Math.min(window.innerHeight - 36, ev.clientY - oy));
        place();
      };
      const up = () => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
        try { localStorage.setItem(posKey, JSON.stringify(float)); } catch {}
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
    });
    /* v8 ignore stop */
  }
  wireFloatPopup("savePopup", "cgoac6502.savepos", ["saveBtn", "loadBtn"]);
  wireFloatPopup("helpPopup", "cgoac6502.helppos", ["helpBtn"]);

  // Save and Load share one popup; pgmMode records which button opened it so
  // shelf chips load or overwrite (renderPgmLib())
  let pgmMode = "save";
  function setPgmMode(mode) {
    pgmMode = mode;
    document.getElementById("pgmLibHint").textContent =
      mode === "load" ? "(click a name to load it)" : "(click a name to overwrite it with the current program)";
  }
  document.getElementById("saveBtn").addEventListener("click", () => setPgmMode("save"));
  document.getElementById("loadBtn").addEventListener("click", () => setPgmMode("load"));
  setPgmMode("save");

  // ---- page theme ----
  initThemePicker(() => setTimeout(sizeScreen, 60));   // page width may have changed

  // ---- wasm machine ----
  const Module = await CgOac6502({});
  const m = new Module.Machine();
  window.__machine = m;    // exposed for the Playwright suite / manual debugging
  window.__term = term;    // ditto
  term.focus();

  // default ROM, built from cpu6502/rom/ (make -C .. rom) and seated at boot
  try {
    const res = await fetch("roms/firmware.bin");
    if (res.ok) m.burnRom(new Uint8Array(await res.arrayBuffer()));
  } catch {}

  // Fetch the Example .bin files before the machine runs. A fetch resolving
  // during the rAF CPU loop wedged the CPU in bios.s's IRQ_HANDLER stub (cause
  // not found), so all fetches finish before m.pressReset().
  //
  // Each .bin holds source captured by gen_example_bin.cpp, poked into RAM by
  // pokeExample() instead of the serial LOAD path (CGOAC6502_REVIEW.md). The
  // visitor still types ASM.
  const EXAMPLE_PROGRAMS = [
    { name: "hello.asm", label: "Hello, World!", file: "hello.bin" },
    { name: "primes.asm", label: "Prime numbers (perf demo)", file: "primes.bin" },
    { name: "rps.asm", label: "Rock-Paper-Scissors", file: "rps.bin" },
    { name: "lcd_demo.asm", label: "LCD demo", file: "lcd_demo.bin" },
  ];
  const EXAMPLE_BIN = {};
  await Promise.all(EXAMPLE_PROGRAMS.map(async (ex) => {
    try {
      const res = await fetch(`examples/${ex.file}`);
      if (res.ok) EXAMPLE_BIN[ex.file] = new Uint8Array(await res.arrayBuffer());
    } catch {}
  }));
  // gen_example_bin.cpp output: [2] srcLen (little-endian), then the source buffer
  function parseExampleBin(bytes) {
    const srcLen = bytes[0] | (bytes[1] << 8);
    return bytes.subarray(2, 2 + srcLen);
  }
  // editor.s constant, not in entrypoints.js (see gen_example_bin.cpp)
  const SRC_START = 0x3000;
  // Pokes an Example's source into the editor buffer and prints a LOAD/Ok echo
  // via injectOutput, since DO_LOAD never runs. The shell must already be entered.
  // The trailing ">" mirrors shell_loop (editor.s): the CPU is still idle at the
  // old prompt, so without it the shell looks inert until Enter.
  function pokeExample(bin) {
    m.pokeRam(SRC_START, parseExampleBin(bin));
    m.injectOutput(encoder.encode("LOAD\r\nOk\r\n>"));
  }

  m.pressReset();

  // ---- terminal -> ACIA, paced by the live baud ----
  // Typed, pasted and loaded bytes queue here and driveFrame() drains them.
  const encoder = new TextEncoder();
  const inQ = [];
  function queueInput(bytes) { for (const b of bytes) inQ.push(b); }
  // named so the fullscreen escBtn can feed a synthetic "\x1b" through the same
  // path (the browser eats Escape in fullscreen)
  function handleTermData(data) {
    if (!poweredOn) return;   // unpowered board ignores input
    if (caps.checked) data = data.toUpperCase();
    queueInput(encoder.encode(data));
  }
  term.onData(handleTermData);

  // ---- ACIA -> terminal, metered at the live baud ----
  const outQ = [];
  let baudBudget = 0;
  const baudLabel = document.getElementById("baudLabel");
  let lastBaud = -1;

  // SAVE frames the source in STX ($02)/ETX ($03) (load.s). This taps the raw
  // output without consuming it, so SAVE still shows on screen.
  let saveCapture = null;   // null, or { started, buf: number[], resolve }
  let pendingLf = false;    // display-only CR->CRLF state across frames

  function pullSerial() {
    if (outQ.length > 256) return;
    const out = m.readOutput();
    for (let i = 0; i < out.length; i++) {
      const b = out[i];
      // SAVE's wire format is bare-CR separated (load.s). saveCapture taps the
      // untouched byte, but the display queue gets a synthetic LF after a lone CR so
      // lines don't overwrite each other.
      if (pendingLf) {
        pendingLf = false;
        if (b !== 0x0a) outQ.push(0x0a);
      }
      outQ.push(b);
      if (b === 0x0d) pendingLf = true;
      if (!saveCapture) continue;
      if (!saveCapture.started) { if (b === 0x02) saveCapture.started = true; }
      else if (b === 0x03) { saveCapture.resolve(saveCapture.buf); saveCapture = null; }
      else saveCapture.buf.push(b);
    }
  }
  function writeFiltered(bytes) {
    const filtered = termFilter ? termFilter(bytes) : bytes;
    if (filtered.length) term.write(Uint8Array.from(filtered));
  }
  function pumpTerminal(dtMs) {
    const baud = m.aciaBaud();
    if (baud !== lastBaud) { baudLabel.textContent = baud ? baud + " baud" : "idle"; lastBaud = baud; }
    const cps = baud ? baud / 10 : 0;   // 10 bits/char: start + 8 data + stop, this board's 8-N-1
    baudBudget += cps ? (dtMs / 1000) * cps : outQ.length;
    let n = Math.max(0, Math.floor(baudBudget));
    baudBudget -= n;
    if (n === 0 && outQ.length && !cps) n = outQ.length;   // idle ACIA: drain immediately
    if (n > 0 && outQ.length) writeFiltered(outQ.splice(0, Math.min(n, outQ.length)));
  }

  // ---- ACIA <- terminal/paste/load, paced at the live baud ----
  // Pastes and Save/Load transfers are metered by m.aciaBaud() (?test=1 skips it).
  // pokeExample() bypasses this.
  //
  // Bytes can't go in back to back: the one-byte ACIA RX register (acia65c51.h)
  // would drop all but the last. The CPU clock can't be sped up either, so
  // driveFrame() slices this frame's real cycle budget across the due characters,
  // letting the NMI handler drain each one.
  const TEST_MODE = new URLSearchParams(location.search).get("test") === "1";
  const MIN_CYCLES_PER_CHAR = 200;   // generous margin over the NMI handler's real drain cost
  let inBudget = 0;

  // Runs `totalCycles` of real CPU time, interleaving up to `n` due characters from inQ
  function driveFrame(totalCycles, dtMs) {
    if (!inQ.length) { m.runCycles(totalCycles); return; }

    const instant = TEST_MODE;
    let n;
    if (instant) {
      // unthrottled, but capped by what the frame's cycle budget can interleave
      n = Math.floor(totalCycles / MIN_CYCLES_PER_CHAR);
    } else {
      const baud = m.aciaBaud();
      const cps = baud ? baud / 10 : 10;   // idle ACIA: slow default
      inBudget += (dtMs / 1000) * cps;
      n = Math.floor(inBudget);
    }
    n = Math.max(0, Math.min(n, inQ.length));
    if (instant) { /* budget not tracked in this mode */ } else { inBudget -= n; }

    if (n === 0) { m.runCycles(totalCycles); return; }
    const slice = Math.floor(totalCycles / n);
    let consumed = 0;
    for (let i = 0; i < n; i++) {
      // Backpressure: stop injecting once the 256-byte SERIAL_BUFFER ring holds more
      // than a few bytes, since STORE_LINE's scan slows as the program grows. A looser
      // threshold (200) still wedged the ROM; Rock-Paper-Scissors (~3.2K) is still
      // open (CGOAC6502_REVIEW.md).
      if (m.serialPending() > 8) break;
      m.typeChar(inQ.shift());
      const c = i === n - 1 ? totalCycles - slice * (n - 1) : slice;
      m.runCycles(c);
      consumed += c;
    }
    if (consumed < totalCycles) m.runCycles(totalCycles - consumed);
  }

  // ---- LEDs / LCD ----
  const pcbLedD1 = document.getElementById("pcbLedD1");
  const pcbLedD4 = document.getElementById("pcbLedD4"), pcbLedD7 = document.getElementById("pcbLedD7");
  function flashLed(el) {
    el.classList.add("on");
    clearTimeout(el._t);
    el._t = setTimeout(() => el.classList.remove("on"), 90);
  }
  // ---- J3 LCD: 5x7 dot-matrix render on a <canvas> (lcdfont.js) ----
  const lcdCanvas = document.getElementById("lcdCanvas");
  const lcdCtx = lcdCanvas.getContext("2d");
  const LCD_COLS = 16, LCD_ROWS = 2;
  const DOT = 4, GAP = 1.2, CHAR_GAP = 3, ROW_GAP = 6, PAD = 8;
  const cellW = LcdFont.cols * DOT + (LcdFont.cols - 1) * GAP;
  const cellH = LcdFont.rows * DOT + (LcdFont.rows - 1) * GAP;
  const lcdW = PAD * 2 + LCD_COLS * cellW + (LCD_COLS - 1) * CHAR_GAP;
  const lcdH = PAD * 2 + LCD_ROWS * cellH + (LCD_ROWS - 1) * ROW_GAP;
  const dpr = window.devicePixelRatio || 1;
  lcdCanvas.width = lcdW * dpr; lcdCanvas.height = lcdH * dpr;
  lcdCanvas.style.width = lcdW + "px"; lcdCanvas.style.height = lcdH + "px";
  lcdCtx.scale(dpr, dpr);

  function drawLcd(text32) {
    lcdCtx.fillStyle = "#8fae2f";
    lcdCtx.fillRect(0, 0, lcdW, lcdH);
    for (let row = 0; row < LCD_ROWS; row++) {
      for (let col = 0; col < LCD_COLS; col++) {
        const ch = text32[row * LCD_COLS + col].toUpperCase();
        const glyph = LcdFont.glyph(ch);
        const ox = PAD + col * (cellW + CHAR_GAP);
        const oy = PAD + row * (cellH + ROW_GAP);
        for (let r = 0; r < LcdFont.rows; r++) {
          for (let c = 0; c < LcdFont.cols; c++) {
            const on = glyph[r][c] === "1";
            lcdCtx.fillStyle = on ? "#16290a" : "#84a32a";
            lcdCtx.fillRect(ox + c * (DOT + GAP), oy + r * (DOT + GAP), DOT, DOT);
          }
        }
      }
    }
  }
  let lastLcdText = null;
  drawLcd(" ".repeat(32));

  const lcdAttachedBox = document.getElementById("lcdAttached");
  lcdAttachedBox.addEventListener("change", () => {
    m.setLcdAttached(lcdAttachedBox.checked);
    lcdCanvas.classList.toggle("detached", !lcdAttachedBox.checked);
  });

  // ---- reset / jumpers ----
  // SW1 on the PCB graphic is the reset control. J7, J5 and J8 have no page
  // controls; bus.h defaults match the shipped ROM.
  document.querySelector('#pcbSvg [data-ref="SW1"]').addEventListener("click", () => { m.pressReset(); });

  // ---- power (J1) ----
  // UI convenience: the real board has no power switch. Off pauses the main loop
  // in place (no reset), so a program mid-edit survives. D1 is hardwired to +5V
  // on the real board; its dimming here is feedback only.
  document.querySelector('#pcbSvg [data-ref="J1"]').addEventListener("click", () => {
    poweredOn = !poweredOn;
    pcbLedD1.classList.toggle("led-power", poweredOn);
    monitorEl.classList.toggle("powered-off", !poweredOn);
    updateFocusHint();   // nothing to type into once powered off -- hide it
  });

  // ---- Help panel: shell address from roms/entrypoints.js (generated) ----
  const E = CGOAC_ENTRYPOINTS;
  function hex(n) { return n.toString(16).toUpperCase(); }
  for (const id of ["hShell", "hShell2", "hShell3"]) document.getElementById(id).textContent = hex(E.SHELL_ENTRY) + "R";
  document.getElementById("hResume").textContent = "JMP $" + hex(E.SHELL_PROMPT);
  // OS-call jump table (bios.s)
  const OS_CALL_IDS = {
    hPrintChar: "PRINT_CHAR", hPrintStr: "PRINT_STR",
    hLcdPutc: "LCD_PUTC", hLcdPuts: "LCD_PUTS", hLcdClear: "LCD_CLEAR",
    hLcdLine1: "LCD_LINE1", hLcdLine2: "LCD_LINE2",
    hReadKey: "READ_KEY",
  };
  for (const [id, name] of Object.entries(OS_CALL_IDS)) document.getElementById(id).textContent = "$" + hex(E[name]);

  // ---- Save / Load (the shell's LOAD/SAVE commands) ----
  // Filename input, a localStorage shelf, a download link and a file import.
  // Every transfer runs over the simulated ACIA.
  const pgmName = document.getElementById("pgmName");
  const pgmSaveBtn = document.getElementById("pgmSave");
  const pgmDownload = document.getElementById("pgmDownload");
  const pgmFileBtn = document.getElementById("pgmFileBtn");
  const pgmFile = document.getElementById("pgmFile");
  const pgmLib = document.getElementById("pgmLib");
  const pgmExamples = document.getElementById("pgmExamples");
  const pgmStatus = document.getElementById("pgmStatus");

  function setPgmStatus(text, ok) {
    pgmStatus.className = ok === undefined ? "muted" : (ok ? "ok" : "err");
    pgmStatus.textContent = text;
  }
  function defaultPgmName() { return pgmName.value.trim() || "program.asm"; }

  // Runs SAVE and resolves with the text between STX/ETX (pullSerial()'s
  // saveCapture). Times out if the ROM never answers. The shell must already be
  // entered: auto-detecting it could resend "<addr>R" into an open prompt and
  // store a bogus line.
  function runSave() {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        saveCapture = null;
        reject(new Error("SAVE timed out -- no response from the ROM."));
      }, 8000);
      saveCapture = { started: false, buf: [], resolve: (bytes) => { clearTimeout(timer); resolve(bytes); } };
      queueInput(encoder.encode("SAVE\r"));
    });
  }
  // Runs LOAD (DO_LOAD clears the program first) and queues the text. Normalizes
  // LF/CRLF to the bare CR DO_LOAD splits on; unnumbered lines auto-number
  // (PROCESS_LINE/AUTO_NUMBER). The shell must already be entered.
  function runLoad(text) {
    text = text.replace(/\r\n|\n/g, "\r");
    queueInput(encoder.encode("LOAD\r" + text));
  }
  window.__testQueueLoad = runLoad; // TEMP DEBUG

  const PGM_LIB_KEY = "cgoac6502.programs";
  function loadPgmLib() { try { return JSON.parse(localStorage.getItem(PGM_LIB_KEY) || "{}"); } catch { return {}; } }
  function savePgmLib(lib) { try { localStorage.setItem(PGM_LIB_KEY, JSON.stringify(lib)); } catch {} }

  function offerDownload(name, text) {
    const url = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
    if (pgmDownload._url) URL.revokeObjectURL(pgmDownload._url);
    pgmDownload._url = url;
    pgmDownload.href = url;
    pgmDownload.download = name;
    pgmDownload.hidden = false;
  }

  // Shared by the Save button and chips clicked in save mode: reads the source
  // via runSave() and writes it to the named slot
  async function doSaveAs(name) {
    const bytes = await runSave();
    let text = "";
    for (let i = 0; i < bytes.length; i++) text += String.fromCharCode(bytes[i]);
    const lib = loadPgmLib();
    const isNew = !(name in lib);
    lib[name] = text;
    savePgmLib(lib);
    if (isNew) renderPgmLib();
    pgmName.value = name;
    offerDownload(name, text);
    setPgmStatus(`Saved ${bytes.length} byte(s)${isNew ? " as" : ", overwriting"} "${name}".`, true);
  }

  function renderPgmLib() {
    const lib = loadPgmLib();
    pgmLib.innerHTML = "";
    for (const name of Object.keys(lib)) {
      const b = document.createElement("button");
      b.className = "chip"; b.textContent = name;
      // load mode loads this program; save mode (default) overwrites the slot
      b.addEventListener("click", async () => {
        if (pgmMode === "load") {
          runLoad(lib[name]);
          pgmName.value = name;
          setPgmStatus(`Loading "${name}" -- watch the terminal for the fresh prompt.`);
          return;
        }
        b.disabled = true;
        setPgmStatus("Saving (reading the source buffer over the ACIA)...");
        try {
          await doSaveAs(name);
        } catch (e) {
          setPgmStatus(e.message || String(e), false);
        } finally {
          b.disabled = false;
        }
      });
      pgmLib.appendChild(b);
    }
  }
  renderPgmLib();

  pgmSaveBtn.addEventListener("click", async () => {
    pgmSaveBtn.disabled = true;
    setPgmStatus("Saving (reading the source buffer over the ACIA)...");
    try {
      await doSaveAs(defaultPgmName());
    } catch (e) {
      setPgmStatus(e.message || String(e), false);
    } finally {
      pgmSaveBtn.disabled = false;
    }
  });

  // ---- Example programs (read-only) ----
  // Fetched at boot, see above. They never touch the localStorage shelf; a
  // visitor Saves a copy to keep edits.
  function renderExamples() {
    pgmExamples.innerHTML = "";
    for (const ex of EXAMPLE_PROGRAMS) {
      const b = document.createElement("button");
      b.className = "chip"; b.textContent = ex.label;
      b.addEventListener("click", () => {
        try {
          if (!(ex.file in EXAMPLE_BIN)) throw new Error(`"${ex.file}" hasn't finished loading yet -- try again in a moment.`);
          pokeExample(EXAMPLE_BIN[ex.file]);
          pgmName.value = ex.name;
          setPgmStatus(`Loaded "${ex.label}" into memory -- type ASM, then RUN.`);
        } catch (e) {
          setPgmStatus(e.message || String(e), false);
        }
      });
      pgmExamples.appendChild(b);
    }
  }
  renderExamples();

  pgmFileBtn.addEventListener("click", () => pgmFile.click());
  pgmFile.addEventListener("change", async () => {
    const f = pgmFile.files[0];
    if (!f) return;
    const text = await f.text();
    pgmName.value = f.name;
    runLoad(text);
    setPgmStatus(`Loading "${f.name}" (${text.length} byte(s)) -- watch the terminal for the fresh prompt.`);
  });

  // ---- main loop ----
  const CLOCK_HZ = 1_000_000;   // X1
  let last = performance.now();
  function frame(now) {
    const dtMs = Math.min(now - last, 50);
    last = now;
    if (!poweredOn) { requestAnimationFrame(frame); return; }

    driveFrame(Math.round((dtMs / 1000) * CLOCK_HZ), dtMs);
    pullSerial();
    pumpTerminal(dtMs);

    if (m.rxLedPulse()) flashLed(pcbLedD4);
    if (m.txLedPulse()) flashLed(pcbLedD7);

    const lcd = m.lcdText();
    if (lcd !== lastLcdText) { drawLcd(lcd); lastLcdText = lcd; }

    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
  sizeScreen();
}

boot();
