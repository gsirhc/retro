// Bridges the WebAssembly 8080 machine to an xterm.js terminal.
//
//   xterm keystrokes ──► machine.sendByte()      (serial channel A RX FIFO)
//   machine.readOutput() ──► xterm.write()       (serial channel A TX FIFO)
//
// The CPU is advanced once per animation frame by a fixed slice of cycles;
// the 2SIO ring buffers absorb the rate mismatch between the two clocks.

const CPU_HZ = 2_000_000;   // Altair 8080A clock; emulation is paced to real time

const hex = (n, w = 2) => n.toString(16).toUpperCase().padStart(w, "0");

// Surface any startup failure on the page itself (the console may be closed).
function fail(msg) {
  console.error(msg);
  const regs = document.getElementById("regs");
  if (regs) regs.textContent = "ERROR — see below";
  const screen = document.getElementById("screen");
  if (screen) {
    screen.innerHTML =
      '<pre style="color:#ff6b6b;white-space:pre-wrap;padding:12px;margin:0;' +
      'font:13px/1.5 ui-monospace,monospace">' +
      String(msg).replace(/[<>&]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;" }[c])) +
      "</pre>";
  }
}

window.addEventListener("error", (e) => fail(e.message + "\n" + (e.error?.stack || "")));
window.addEventListener("unhandledrejection", (e) => fail("promise rejected: " + (e.reason?.stack || e.reason)));

async function boot() {
  if (typeof Terminal !== "function") {
    return fail("xterm.js did not load (global `Terminal` missing).\n" +
                "Check the network tab for the cdnjs xterm request, or vendor it locally.");
  }
  if (typeof FitAddon === "undefined" || typeof FitAddon.FitAddon !== "function") {
    return fail("xterm-addon-fit did not load (global `FitAddon` missing).");
  }
  if (typeof Retro8080 !== "function") {
    return fail("retro8080.js did not load (global `Retro8080` missing).\n" +
                "Did `make` succeed? Is retro8080.js next to index.html?");
  }

  // --- terminal --------------------------------------------------------
  const term = new Terminal({
    fontFamily: 'ui-monospace, Menlo, Consolas, "DejaVu Sans Mono", monospace',
    fontSize: 15,
    cursorBlink: true,
    theme: { background: "#000000", foreground: "#33ff88", cursor: "#33ff88" },
  });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  const screenEl = document.getElementById("screen");
  const bezelEl = screenEl.closest(".bezel");
  const monitorEl = screenEl.closest(".monitor");
  // reference box; sizeScreen() measures the font here then locks to 80x24
  const REF_W = 792, REF_H = 460;
  term.open(screenEl);
  fit.fit();

  // On a CRT (no scrollback) xterm turns wheel events into arrow-key presses
  // sent to the program. Stop it before xterm sees them so the page just
  // scrolls; the Teletype (has scrollback) keeps normal wheel scrolling.
  screenEl.addEventListener("wheel", (e) => {
    if (!monitorEl.classList.contains("scrolls")) e.stopImmediatePropagation();
  }, { capture: true });

  // Fill the page column with the monitor: screen as wide as fits (minus the
  // monitor's own frame), locked to 24 rows; column count floats with the font.
  function sizeScreen() {
    try {
      screenEl.style.width = REF_W + "px";
      screenEl.style.height = REF_H + "px";
      fit.fit();
      if (!term.cols || !term.rows) return;
      const cw = REF_W / term.cols, ch = REF_H / term.rows;   // cell size for this font
      const inner = screenEl.closest(".inner");
      let avail = window.innerWidth;
      if (inner) {
        const cs = getComputedStyle(inner);
        avail = inner.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
      }
      const frame = Math.max(0, monitorEl.offsetWidth - screenEl.offsetWidth);
      // bare (Modern / Teletype) has almost no frame -> leave a gutter instead
      const gutter = monitorEl.classList.contains("bare") ? 48 : 4;
      const room = avail - frame - gutter;
      const cols = Math.max(40, Math.floor(room / cw));
      screenEl.style.width = Math.round(cols * cw) + "px";
      screenEl.style.height = Math.round(24 * ch) + "px";
      term.resize(cols, 24);
      term.refresh(0, term.rows - 1);
    } catch {}
  }
  addEventListener("resize", sizeScreen);
  // A real serial terminal shows nothing at power-on but a cursor — it has no
  // idea what (if anything) is on the other end of the wire. So no banner.

  // --- serial terminal profiles ----------------------------------
  // CAPS LOCK (the terminal's ALPHA-LOCK) is a plain manual toggle, default on
  // — most Altair-era software wants uppercase and many terminals were
  // uppercase-only anyway. Profiles don't touch it.
  const caps = document.getElementById("caps");

  const TERM_PROFILES = {
    modern: { label: "Modern (xterm)",
      font: 'ui-monospace, Menlo, Consolas, "DejaVu Sans Mono", monospace',
      size: 15, fg: "#33ff88", bg: "#000000", dim: "#1c8f52", br: "#8affc0",
      crt: "none", baud: 0, cursor: "block", blink: true, glow: 0 },
    vt100g: { label: "DEC VT100 · green",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#39ff41", bg: "#0a140a", dim: "#1c8c1c", br: "#a6ffa6",
      crt: "scan", baud: 9600, cursor: "block", blink: true, glow: 3 },
    vt100a: { label: "DEC VT100 · amber",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#ffb32b", bg: "#180d00", dim: "#a8701a", br: "#ffd77e",
      crt: "scan", baud: 9600, cursor: "block", blink: true, glow: 3 },
    vt52: { label: "DEC VT52",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#4dff4d", bg: "#061006", dim: "#1c8c1c", br: "#b6ffb6",
      crt: "scanheavy", baud: 4800, cursor: "block", blink: false, glow: 4 },
    adm3a: { label: "Lear Siegler ADM-3A",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#39ff9c", bg: "#03100b", dim: "#1c8c5c", br: "#a6ffce",
      crt: "scan", baud: 9600, cursor: "underline", blink: true, glow: 2 },
    glasstty: { label: "Glass TTY",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#c8ffc8", bg: "#020802", dim: "#5c9c5c", br: "#ecffec",
      crt: "scan", baud: 300, cursor: "block", blink: true, glow: 2 },
    tty33: { label: "Teletype ASR-33",
      font: '"Courier Prime", "Courier New", monospace', size: 15,
      fg: "#242424", bg: "#efe8d6", dim: "#7a7261", br: "#000000",
      crt: "paper", baud: 110, cursor: "underline", blink: false,
      glow: 0, bell: "ding", scrollback: 5000 },   // paper roll -> scroll back
  };

  let bellMode = "flash";

  // scanlines / glow / vignette / paper grain — toggleable, on by default
  const crtToggle = document.getElementById("crt");
  let crtOn = true;
  try {
    const q = new URLSearchParams(location.search).get("crt");
    crtOn = q != null ? q !== "0" : localStorage.getItem("retro8080.crt") !== "0";
  } catch {}
  crtToggle.checked = crtOn;
  crtToggle.addEventListener("change", () => {
    crtOn = crtToggle.checked;
    try { localStorage.setItem("retro8080.crt", crtOn ? "1" : "0"); } catch {}
    applyProfile(termSelect.value);
  });

  function applyProfile(key) {
    const p = TERM_PROFILES[key] || TERM_PROFILES.modern;
    // serial bits -> characters/sec: async framing is ~10 bits/char (start + 8
    // data + 1 stop); the ASR-33 used 2 stop bits, so 110 baud is exactly 10 cps.
    baudCps = p.baud ? p.baud / (key === "tty33" ? 11 : 10) : 0;
    bellMode = p.bell || "flash";
    term.options.fontFamily = p.font;
    term.options.fontSize = p.size;
    term.options.cursorStyle = p.cursor;
    term.options.cursorBlink = p.blink;
    // a CRT has no scrollback -- only a Teletype's paper does
    term.options.scrollback = p.scrollback || 0;
    term.options.theme = {
      background: p.bg, foreground: p.fg,
      cursor: p.fg, cursorAccent: p.bg, selectionBackground: p.fg + "44",
      black: p.bg, red: p.fg, green: p.fg, yellow: p.fg, blue: p.fg,
      magenta: p.fg, cyan: p.fg, white: p.fg,
      brightBlack: p.dim, brightRed: p.br, brightGreen: p.br, brightYellow: p.br,
      brightBlue: p.br, brightMagenta: p.br, brightCyan: p.br, brightWhite: p.br,
    };
    const noCrt = p.crt === "none";           // Modern profile has no CRT layer
    crtToggle.disabled = noCrt;
    crtToggle.checked = noCrt ? false : crtOn;
    const showCrt = crtOn && !noCrt;
    bezelEl.className = "bezel crt-" + p.crt + (showCrt ? "" : " crt-off");
    monitorEl.classList.toggle("amber", key === "vt100a");
    // Modern and Teletype aren't CRTs -> no monitor housing
    monitorEl.classList.toggle("bare", p.crt === "none" || p.crt === "paper");
    monitorEl.classList.toggle("scrolls", !!p.scrollback);
    screenEl.style.setProperty("--glow", (showCrt ? p.glow : 0) + "px");
    // wait for the bitmap face + let xterm re-measure the new cell size,
    // then size the screen
    (document.fonts ? document.fonts.ready : Promise.resolve())
      .then(() => setTimeout(sizeScreen, 30));
    try { localStorage.setItem("retro8080.term", key); } catch {}
  }

  // phosphor / mechanical bell
  function bell() {
    bezelEl.classList.add("bell-flash");
    setTimeout(() => bezelEl.classList.remove("bell-flash"), 90);
    if (bellMode === "flash") return;
    try {
      const ac = bell._ac || (bell._ac = new (window.AudioContext || window.webkitAudioContext)());
      const o = ac.createOscillator(), g = ac.createGain();
      o.connect(g); g.connect(ac.destination);
      o.type = bellMode === "ding" ? "triangle" : "square";
      o.frequency.value = bellMode === "ding" ? 1760 : 800;
      g.gain.setValueAtTime(0.06, ac.currentTime);
      g.gain.exponentialRampToValueAtTime(0.0008, ac.currentTime + (bellMode === "ding" ? 0.35 : 0.12));
      o.start();
      o.stop(ac.currentTime + 0.4);
    } catch {}
  }
  term.onBell(bell);

  // --- wasm machine ---------------------------------------------------
  const V = window.__V;
  let Module;
  try {
    Module = await Retro8080(V ? { locateFile: (p) => p + "?v=" + V } : {});
  } catch (err) {
    return fail("wasm module failed to instantiate:\n" + (err?.stack || err));
  }
  const machine = new Module.Machine();
  machine.reset();
  term.focus();

  // --- terminal -> CPU ----------------------------------------------
  const encoder = new TextEncoder();
  term.onData((data) => {
    // a keypress skips to the end of a slow how-to printout
    if (printingManual) {
      term.write(new Uint8Array(outQ.splice(0)));
      printingManual = false;
      baudCps = baudSaved;
    }
    // Vintage terminals were uppercase-only; BASIC / CP/M expect it.
    if (caps.checked) data = data.toUpperCase();
    const bytes = encoder.encode(data);
    for (const b of bytes) machine.sendByte(b);
  });

  // --- CPU -> terminal ------------------------------------------
  // Bytes the CPU transmits are pulled off the 2SIO into `outQ`, then metered
  // onto the screen at the selected terminal's baud rate. Keeping outQ short
  // lets the 2SIO's 512-byte FIFO fill, which drops its TDRE bit, which makes
  // an output-bound program (BASIC LISTing, say) actually wait for the
  // terminal — exactly how a 110-baud Teletype throttled the machine.
  const crlf = document.getElementById("crlf");
  const outQ = [];
  let baudCps = 0;          // 0 = unthrottled; else characters/second
  let baudBudget = 0;
  let printingManual = false;   // the how-to is metered at the terminal's baud
  let baudSaved = 0;            // real baud, restored when the how-to finishes

  function pullSerial() {
    // stay mostly drained so the 2SIO FIFO does the buffering + TDRE backpressure
    if (outQ.length > 128) return;
    const out = machine.readOutput();
    for (let i = 0; i < out.length; i++) {
      // 1970s serial terminals are 7-bit ASCII; the 8th bit is parity/ignored.
      // Altair BASIC's LIST relies on this — it sets bit 7 on the first letter
      // of every tokenised keyword (PRINT -> 0xD0,'RINT'), so without the mask
      // that leading letter renders as a stray C1 control and vanishes.
      const b = out[i] & 0x7f;
      outQ.push(b);
      if (crlf.checked && b === 0x0d && (out[i + 1] & 0x7f) !== 0x0a) outQ.push(0x0a);
    }
  }

  function pumpTerminal(dtMs) {
    if (printingManual && outQ.length === 0) {
      printingManual = false;
      baudCps = baudSaved;
    }
    pullSerial();
    if (outQ.length === 0) return;
    if (baudCps === 0) { term.write(new Uint8Array(outQ.splice(0))); return; }
    baudBudget += (baudCps * dtMs) / 1000;
    baudBudget = Math.min(baudBudget, baudCps);   // cap catch-up after a stall
    const n = Math.min(outQ.length, Math.floor(baudBudget));
    if (n > 0) { baudBudget -= n; term.write(new Uint8Array(outQ.splice(0, n))); }
  }

  function flushTerminal() {           // used by SINGLE STEP — show it now
    pullSerial();
    if (outQ.length) term.write(new Uint8Array(outQ.splice(0)));
  }
  const drainToTerminal = flushTerminal;   // back-compat name for panel STEP

  // wipe the screen and print a one-page how-to, wrapped to the terminal
  function printManual() {
    outQ.length = 0;
    baudBudget = 0;
    term.write("\r\x1b[0m\x1b[2J\x1b[3J\x1b[H");   // wipe screen + scrollback + any half-typed line
    const w = Math.max(46, Math.min((term.cols || 80) - 2, 92));
    const bar = "=".repeat(w);
    const mid = (s) => " ".repeat(Math.max(0, (w - s.length) >> 1)) + s;
    const wrap = (s, indent) => {
      const out = [];
      let line = indent;
      for (const word of s.split(" ")) {
        if (line.length + word.length + 1 > w && line.trim()) {
          out.push(line);
          line = indent + word;
        } else line += (line === indent ? "" : " ") + word;
      }
      if (line.trim()) out.push(line);
      return out;
    };
    const lines = [bar, mid("ALTAIR 8800  --  HOW TO USE"), bar];
    const para = (s, indent) => wrap(s, indent).forEach((l) => lines.push(l));
    para("THE MACHINE IS BARE -- nothing is loaded, and the default program " +
         "just echoes what you type. Load a program with [Load Program...], " +
         "or key one in on the front panel.", " ");
    lines.push("");
    para("THE TERMINAL -- these are real 1970s terminals, limits and all. " +
         "The screen is a fixed 24 lines with no scrollback: text off the top " +
         "is gone. Uppercase only, too -- the Altair's monitor and BASIC need " +
         "caps, and most terminals of the day couldn't show lowercase, so " +
         "CAPS LOCK stays on.", " ");
    lines.push("");
    para("THE TELETYPE (ASR-33) -- a printer, not a screen: 10 characters a " +
         "second onto a paper roll. Cheap, and many hobbyists already had " +
         "one, so it was the common console. Because it's paper, it does " +
         "scroll back.", " ");
    lines.push("", " HISTORY");
    para("1974 -- Ed Roberts's MITS of Albuquerque bets the company on " +
         "Intel's 8080. It hits the Jan 1975 cover of Popular Electronics: " +
         "the Altair 8800, a $439 kit -- 8080 at 2 MHz, 256 bytes of RAM, no " +
         "keyboard, no screen. Gates & Allen wrote Altair BASIC and started " +
         "Micro-Soft to sell it.", "   ");
    lines.push(bar);
    // centre the block in the screen: pad rows above, indent columns left
    const rows = term.rows || 24;
    const padTop = Math.max(0, (rows - lines.length) >> 1);
    const indent = " ".repeat(Math.max(0, ((term.cols || 80) - w) >> 1));
    const body =
      "\r\n".repeat(padTop) +
      lines.map((l) => indent + l).join("\r\n") +
      "\x1b[" + rows + ";1H";   // park the cursor on a clean line below the block
    // meter it onto the screen at the terminal's baud (instant on Modern) --
    // but floor it so a 110-baud Teletype prints the page in ~8 s, not ~2 min.
    // Press a key to skip to the end.
    const text = body.toUpperCase();
    for (let i = 0; i < text.length; i++) outQ.push(text.charCodeAt(i));
    if (baudCps > 0) {
      baudSaved = baudCps;
      baudCps = Math.max(baudCps, Math.ceil(text.length / 8));
      printingManual = true;
    }
  }

  // --- pick the terminal ---------------------------------------
  const termSelect = document.getElementById("termProfile");
  let savedTerm = new URLSearchParams(location.search).get("term") || "vt100g";
  try { savedTerm = new URLSearchParams(location.search).get("term")
                 || localStorage.getItem("retro8080.term") || "vt100g"; } catch {}
  if (!TERM_PROFILES[savedTerm]) savedTerm = "vt100g";
  termSelect.value = savedTerm;
  termSelect.addEventListener("change", () => applyProfile(termSelect.value));

  // load the bitmap faces, then apply (so xterm measures the right cell size)
  Promise.all([
    document.fonts?.load('20px "VT323"'),
    document.fonts?.load('15px "Courier Prime"'),
  ].filter(Boolean)).catch(() => {}).finally(() => applyProfile(savedTerm));

  // --- page theme (Windows chrome vs '94 gray web) ----------------
  const pageTheme = document.getElementById("pageTheme");
  const root = document.documentElement;
  let savedPageTheme =
    new URLSearchParams(location.search).get("theme") || root.dataset.theme || "win";
  if (!["win", "web94", "modern"].includes(savedPageTheme)) savedPageTheme = "win";
  root.dataset.theme = savedPageTheme;
  pageTheme.value = savedPageTheme;
  pageTheme.addEventListener("change", () => {
    root.dataset.theme = pageTheme.value;
    try { localStorage.setItem("retro8080.theme", pageTheme.value); } catch {}
    setTimeout(sizeScreen, 60);  // page width changed
  });

  // "Last built" = the wasm's own mtime on the server
  (async () => {
    const el = document.getElementById("buildDate");
    for (const url of ["retro8080.wasm", "retro8080.js", "app.js"]) {
      try {
        const r = await fetch(url, { method: "HEAD", cache: "no-store" });
        const lm = r.headers.get("Last-Modified");
        if (lm) {
          el.textContent = new Date(lm).toLocaleDateString(undefined,
            { year: "numeric", month: "long", day: "numeric" });
          return;
        }
      } catch {}
    }
    el.textContent = "unknown";
  })();

  // --- main loop ----------------------------------------------
  let running = true;    // front-panel STOP/RUN
  let powered = true;    // front-panel OFF/ON
  let runSwitchEl = null;
  const setRunning = (v) => {
    running = v && powered;
    if (runSwitchEl) runSwitchEl.classList.toggle("down", running);  // down = RUN
    if (running) term.focus();
  };
  let lastFrame = performance.now();
  function frame(now) {
    const dt = Math.max(0, Math.min(now - lastFrame, 100));   // clamp after a tab-away
    lastFrame = now;
    if (running && powered) {
      tape.tick();
      // cycles paced by real elapsed time, so speed is 2 MHz on any display
      machine.runCycles(Math.round(CPU_HZ * dt / 1000));
    }
    pumpTerminal(dt);          // keep typing out buffered text even when stopped
    updatePanel();
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);

  // --- register readout -------------------------------------
  const regs = document.getElementById("regs");
  let lastCyc = 0, lastT = performance.now();
  setInterval(() => {
    const s = machine.state();
    const now = performance.now();
    const mhz = (s.cycles - lastCyc) / (now - lastT) / 1000;   // cycles/ms -> MHz
    lastCyc = s.cycles;
    lastT = now;
    regs.textContent =
      `A ${hex(s.a)}  BC ${hex(s.b)}${hex(s.c)}  DE ${hex(s.d)}${hex(s.e)}  ` +
      `HL ${hex(s.h)}${hex(s.l)}  PC ${hex(s.pc, 4)}  SP ${hex(s.sp, 4)}  ` +
      `${s.halted ? "HALT" : running ? "RUN " : "STOP"}  ${mhz.toFixed(1)} MHz`;
  }, 250);

  // --- Altair 8800 front panel -----------------------------
  const dataLeds = [];   // index = bit (0..15), address + data share the low 16/8
  const addrLeds = [];
  const statusLeds = {};
  const switchState = new Array(16).fill(0);   // A0..A15 toggle switches
  const switchEls = new Array(16).fill(null);
  let panelIntercept = null;   // set by the tape-load guide to capture paddles

  // drive the low `n` toggle switches from a value
  const setSwitchWord = (value, n) => {
    for (let b = 0; b < n; b++) {
      switchState[b] = (value >> b) & 1;
      if (switchEls[b]) switchEls[b].classList.toggle("down", !switchState[b]);
    }
  };

  // reflect a sense byte onto the A8..A15 paddles and the machine
  const applySense = (byte) => {
    for (let bit = 8; bit < 16; bit++) {
      switchState[bit] = (byte >> (bit - 8)) & 1;
      if (switchEls[bit]) switchEls[bit].classList.toggle("down", !switchState[bit]);
    }
    machine.setSenseSwitches?.(byte & 0xff);
  };

  const switchWord = () => switchState.reduce((w, on, i) => w | (on << i), 0);
  const senseByte  = () => (switchWord() >> 8) & 0xff;

  // small DOM helpers
  const el = (cls, html) => {
    const d = document.createElement("div");
    d.className = cls;
    if (html != null) d.innerHTML = html;
    return d;
  };
  // octal grouping (address + switches): bit 15 alone, then five groups of 3.
  // Grid column for a bit — must match the .fp-cols template in the CSS.
  const GROUPS = [[15], [14, 13, 12], [11, 10, 9], [8, 7, 6], [5, 4, 3], [2, 1, 0]];
  const bitCol = (b) => {
    let col = 2;                       // track 1 is the lead
    for (const g of GROUPS) {
      col++;                           // the gap track before each group
      const i = g.indexOf(b);
      if (i >= 0) return col + i;
      col += g.length;
    }
    return col;
  };
  const gridLed = (label, b, store, row) => {
    const c = el("fp-cell", `<div class="fp-cap">${label}</div>`);
    c.style.gridArea = `${row} / ${bitCol(b)}`;
    const l = el("led");
    store[b] = l;
    c.appendChild(l);
    return c;
  };

  function buildPanel() {
    const root = document.getElementById("altair");
    const kase = el("fp-case");
    const face = el("fp-face");
    kase.appendChild(face);
    root.appendChild(kase);

    // ---- LED grid: row 1 status + data, row 2 WAIT/HLDA + address ----
    const leds = el("fp-leds fp-cols");
    face.appendChild(leds);

    const statusNames = ["INTE", "PROT", "MEMR", "INP", "M1", "OUT", "HLTA", "STACK", "WO", "INT"];
    const statusWrap = el("fp-status");
    const statusRow = el("fp-statusleds");
    statusNames.forEach((n) => {
      const c = el("fp-cell", `<div class="fp-cap">${n}</div>`);
      const l = el("led");
      statusLeds[n] = l;
      c.appendChild(l);
      statusRow.appendChild(c);
    });
    statusWrap.appendChild(statusRow);
    statusWrap.appendChild(el("fp-statuslabel", "STATUS"));
    leds.appendChild(statusWrap);

    for (let b = 7; b >= 0; b--) leds.appendChild(gridLed("D" + b, b, dataLeds, 1));

    const wh = el("fp-wh");
    ["WAIT", "HLDA"].forEach((n) => {
      const c = el("fp-cell", `<div class="fp-cap">${n}</div>`);
      const l = el("led");
      statusLeds[n] = l;
      c.appendChild(l);
      wh.appendChild(c);
    });
    leds.appendChild(wh);

    for (let b = 15; b >= 0; b--) leds.appendChild(gridLed("A" + b, b, addrLeds, 2));

    // ---- switch grid: power + 16 A-switches, same columns as the LEDs ----
    const sw = el("fp-switches");
    sw.appendChild(el("fp-tag sense", "SENSE&nbsp;SW.&nbsp;&#9656;"));
    sw.appendChild(el("fp-tag data", "&#9666;&nbsp;DATA / ADDRESS"));
    face.appendChild(sw);

    const swgrid = el("fp-swgrid fp-cols");
    sw.appendChild(swgrid);

    const powerCell = el("fp-power paddle", `<div class="fp-cap">OFF</div>`);
    const powerBat = el("bat down");   // down = ON
    powerCell.title = "power OFF / ON";
    powerCell.addEventListener("click", () => {
      powered = !powered;
      powerBat.classList.toggle("down", powered);
      if (!powered) setRunning(false);
    });
    powerCell.appendChild(powerBat);
    powerCell.insertAdjacentHTML("beforeend", `<div class="fp-num">ON</div>`);
    swgrid.appendChild(powerCell);

    for (let bit = 15; bit >= 0; bit--) {
      const c = el("fp-cell paddle");
      c.style.gridArea = `1 / ${bitCol(bit)}`;
      const b = el("bat down");
      switchEls[bit] = b;
      c.title = `A${bit}` + (bit >= 8 ? ` — sense bit ${bit - 8}` : "");
      c.addEventListener("click", () => {           // any click on the column toggles
        switchState[bit] ^= 1;
        b.classList.toggle("down", !switchState[bit]);
        machine.setSenseSwitches?.(senseByte());
      });
      c.appendChild(b);
      c.insertAdjacentHTML("beforeend", `<div class="fp-num">${bit}</div>`);
      swgrid.appendChild(c);
    }

    // ---- control paddle row (own flex row, aligned under the switches) ----
    const cr = el("fp-ctl");

    // label above (upper fn, red) and below (lower fn, grey) the paddle.
    // The tape-load guide can intercept every control paddle via panelIntercept.
    const paddle = (up, dn, name, upFn, dnFn, latching) => {
      const c = el("fp-cell paddle", `<div class="fp-cap up"><b>${up}</b></div>`);
      const b = el("bat" + (latching ? "" : " momentary"));
      const twoWay = !!(dn && dnFn);
      // Whole cell is the hit target. A two-function paddle still needs a
      // direction, so it splits at the lever's midline (top label -> up,
      // bottom label -> down); everything else just fires on any click.
      c.addEventListener("click", (e) => {
        let upper = true;
        if (latching) {
          upper = b.classList.contains("down");     // currently RUN -> flip to STOP
        } else if (twoWay) {
          const r = b.getBoundingClientRect();
          upper = e.clientY < r.top + r.height / 2;
        }
        if (!latching) {
          b.classList.add(upper ? "flick" : "flick-dn");
          setTimeout(() => b.classList.remove("flick", "flick-dn"), 140);
        }
        if (panelIntercept) { panelIntercept(name, upper); return; }
        (upper ? upFn : dnFn || upFn)();
      });
      c.appendChild(b);
      c.insertAdjacentHTML("beforeend", `<div class="fp-cap dn">${dn || "&nbsp;"}</div>`);
      return { cell: c, bat: b };
    };

    const run = paddle("STOP", "RUN", "run",
      () => setRunning(false), () => setRunning(true), true);
    runSwitchEl = run.bat;
    runSwitchEl.classList.toggle("down", running);   // down = RUN
    cr.appendChild(run.cell);

    const step = () => {
      if (running || !powered) return;
      machine.stepOne?.();
      drainToTerminal();
    };
    cr.appendChild(paddle("SINGLE STEP", "", "step", step, step).cell);
    cr.appendChild(paddle("EXAMINE", "EXAMINE NEXT", "examine",
      () => machine.setPC?.(switchWord() & 0xffff),
      () => machine.setPC?.((machine.state().pc + 1) & 0xffff)).cell);
    cr.appendChild(paddle("DEPOSIT", "DEPOSIT NEXT", "deposit",
      () => machine.writeByte?.(machine.state().pc, switchWord() & 0xff),
      () => {
        const p = (machine.state().pc + 1) & 0xffff;
        machine.setPC?.(p);
        machine.writeByte?.(p, switchWord() & 0xff);
      }).cell);
    cr.appendChild(paddle("RESET", "CLR", "reset",
      () => { machine.reboot?.(); machine.setPC?.(0); },
      () => machine.reboot?.()).cell);
    cr.appendChild(paddle("PROTECT", "UNPROTECT", "protect", () => {}, () => {}).cell);
    cr.appendChild(paddle("AUX", "", "aux1", () => {}, () => {}).cell);
    cr.appendChild(paddle("AUX", "", "aux2", () => {}, () => {}).cell);
    sw.appendChild(cr);

    // ---- bottom badge -------------------------------------------
    face.insertAdjacentHTML("beforeend",
      `<div class="fp-badge">
         <span class="mits">mits</span>
         <span class="name">ALTAIR&nbsp;<span>8800</span>&nbsp;<span class="lite">COMPUTER</span></span>
       </div>`);

    root.insertAdjacentHTML("beforeend",
      `<div class="hint">Not a photo-accurate panel &mdash; but the lights and switches behave exactly as they would on a real Altair 8800.</div>`);
  }

  function setBits(leds, value, n) {
    for (let b = 0; b < n; b++) {
      if (leds[b]) leds[b].classList.toggle("on", !!((value >> b) & 1));
    }
  }

  function updatePanel() {
    if (!powered) {
      for (const l of addrLeds) l && l.classList.remove("on");
      for (const l of dataLeds) l && l.classList.remove("on");
      for (const k in statusLeds) statusLeds[k].classList.remove("on");
      return;
    }
    const s = machine.state();
    // while a tape is reading in, the address bus tracks the loader's write
    // pointer (HL) climbing through memory, not the tight poll loop's PC
    const loading = tape.phase === "loading";
    const abus = loading ? ((s.h << 8) | s.l) : s.pc;
    setBits(addrLeds, abus, 16);
    setBits(dataLeds, loading ? tape.lastByte
                              : machine.readByte ? machine.readByte(s.pc) : 0, 8);
    const set = (name, on) => statusLeds[name] && statusLeds[name].classList.toggle("on", !!on);
    const active = running && !s.halted;
    set("INP", loading);
    set("WAIT", !running || s.halted);
    set("HLTA", s.halted);
    set("INTE", s.intEnabled);
    set("MEMR", active);
    set("M1", active);
    set("MI", active);          // (legacy key, harmless)
    set("WO", true);            // WO is lit except during a memory write
    set("PROT", false);
    set("OUT", false);
    set("STACK", false);
    set("INT", false);
    set("HLDA", false);
  }

  buildPanel();

  // --- paper-tape load ------------------------------------
  // The user keys a 25-byte serial bootstrap into memory on the front panel,
  // then RUN starts it and we trickle the program image through the 2SIO's
  // receive line — exactly how a MITS paper tape or cassette loaded, only the
  // reader runs at a few KB/s instead of 10 bytes/s.
  const TAPE_SECONDS = 7;

  const oct = (n, w) =>
    n.toString(8).padStart(w, "0").replace(/(\d{3})(?=\d)/g, "$1 ");

  // bootstrap: init nothing, poll the 2SIO, store `count` bytes at `dest`,
  // jump to `start`. Lives at 0x0000 (clear of a 0xE000-area load target).
  function makeBootstrap(dest, count, start) {
    const org = dest >= 0x0100 ? 0x0000 : 0xdf00;
    const L = org + 6;
    const lo = (n) => n & 0xff, hi = (n) => (n >> 8) & 0xff;
    const bytes = [
      0x21, lo(dest), hi(dest),        //      LXI  H,dest
      0x11, lo(count), hi(count),      //      LXI  D,count
      0xdb, 0x10,                      // L:   IN   10h      ; 2SIO status
      0x0f,                            //      RRC           ; RDRF -> carry
      0xd2, lo(L), hi(L),              //      JNC  L        ; wait for a byte
      0xdb, 0x11,                      //      IN   11h      ; read it
      0x77,                            //      MOV  M,A      ; store
      0x23,                            //      INX  H
      0x1b,                            //      DCX  D
      0x7a, 0xb3,                      //      MOV  A,D / ORA E
      0xc2, lo(L), hi(L),              //      JNZ  L
      0xc3, lo(start), hi(start),      //      JMP  start
    ];
    return { org, bytes };
  }

  const tape = {
    phase: "idle",        // idle | entering | loading | done
    steps: [], idx: 0,
    image: null, pos: 0, lastByte: 0,
    root: document.getElementById("tape-guide"),

    begin(image, dest, start) {
      this.stale = !machine.writeByte || !machine.setPC || !machine.rxPending;
      machine.clearMemory?.();
      machine.reboot?.();
      setRunning(false);
      const boot = makeBootstrap(dest, image.length, start);
      this.image = image;
      this.dest = dest;
      this.start = start;
      this.steps = [
        { kind: "examine", addr: boot.org, key: "examine", up: true,
          text: `EXAMINE ${oct(boot.org, 6)}`, hint: "set the start address, press EXAMINE" },
        ...boot.bytes.map((b, i) => ({
          kind: i === 0 ? "deposit" : "depositNext",
          addr: boot.org + i, value: b, key: "deposit", up: i === 0,
          text: `${i === 0 ? "DEPOSIT" : "DEP NEXT"}  ${oct(b, 3)}  (${hex(b)})`,
          hint: i === 0 ? "press DEPOSIT" : "press DEPOSIT NEXT",
        })),
        { kind: "examine", addr: boot.org, key: "examine", up: true,
          text: `EXAMINE ${oct(boot.org, 6)}`, hint: "back to the start, press EXAMINE" },
        { kind: "run", key: "run", up: false,
          text: "RUN", hint: "press RUN to start the tape reader" },
      ];
      this.idx = 0;
      this.turbo = false;
      this.phase = "entering";
      panelIntercept = (name, upper) => this.onPanel(name, upper);
      this.render();
      this.arm();
      if (this.stale) this.flash("rebuild the wasm (make wasm) for this to actually load");
      this.root.hidden = false;
      this.root.scrollIntoView({ behavior: "smooth", block: "nearest" });
    },

    arm() {
      const s = this.steps[this.idx];
      if (!s) return;
      if (s.kind === "deposit" || s.kind === "depositNext") setSwitchWord(s.value, 8);
      else if (s.kind === "examine") setSwitchWord(s.addr, 16);
      this.rows.forEach((r, i) => {
        r.classList.toggle("done", i < this.idx);
        r.classList.toggle("now", i === this.idx);
      });
      const cur = this.rows[this.idx];
      if (cur) cur.scrollIntoView({ block: "nearest" });
      this.msg.textContent = `Step ${this.idx + 1} / ${this.steps.length}  —  ${s.hint}`;
    },

    onPanel(name /*, upper */) {
      const s = this.steps[this.idx];
      if (!s) return;
      // during the guided sequence the guide is authoritative about the
      // up/down half — you only have to press the right paddle
      if (name !== s.key) {
        this.flash(`not now — ${s.hint}`);
        return;
      }
      if (s.kind === "examine") {
        machine.setPC?.(s.addr);
      } else if (s.kind === "run") {
        // fall through — advance() will start the load
      } else {
        const sw = switchWord() & 0xff;
        if (sw !== s.value) {
          this.flash(`switches show ${oct(sw, 3)}, need ${oct(s.value, 3)}`);
          return;
        }
        machine.writeByte?.(s.addr, s.value);
        machine.setPC?.(s.addr);          // so the address LEDs show the deposit
      }
      this.advance();
    },

    advance() {
      this.idx++;
      if (this.idx >= this.steps.length) return this.startLoad();
      this.arm();
    },

    async express() {
      if (this.phase !== "entering") return;
      while (this.idx < this.steps.length) {
        const s = this.steps[this.idx];
        this.arm();
        await new Promise((r) => setTimeout(r, 40));
        if (s.kind === "examine") machine.setPC?.(s.addr);
        else if (s.kind !== "run") { machine.writeByte?.(s.addr, s.value); machine.setPC?.(s.addr); }
        this.idx++;
      }
      this.startLoad();
    },

    startLoad() {
      this.phase = "loading";
      this.pos = 0;
      this.loadStart = performance.now();
      panelIntercept = null;
      setRunning(true);                   // the bootstrap begins polling
      this.root.classList.add("loading");
      this.render();
    },

    tick() {
      if (this.phase !== "loading") return;
      const speed = this.turbo ? 24 : 1;
      const elapsed = (performance.now() - this.loadStart) * speed;
      const target = Math.min(
        this.image.length,
        Math.ceil((this.image.length * elapsed) / (TAPE_SECONDS * 1000))
      );
      while (this.pos < target) {
        if (machine.rxPending && machine.rxPending() > 200) break;
        this.lastByte = this.image[this.pos++];
        machine.sendByte(this.lastByte);
      }
      const frac = this.pos / this.image.length;
      if (this.bar) this.bar.style.width = (frac * 100).toFixed(1) + "%";
      if (this.stat) {
        const left = Math.max(0, TAPE_SECONDS * (1 - frac));
        this.stat.textContent =
          `${this.pos.toLocaleString()} / ${this.image.length.toLocaleString()} bytes` +
          (frac < 1 ? `   ·   ~${left.toFixed(0)}s` : "   ·   done");
      }
      if (this.pos >= this.image.length && this.phase === "loading") {
        this.phase = "done";
        setTimeout(() => this.finish(), 1400);
      }
    },

    finish() {
      this.phase = "idle";
      this.root.hidden = true;
      this.root.classList.remove("loading");
      lastLoad = { bytes: this.image, load: this.dest, start: this.start,
                   label: "tape image", sense: 0 };
      term.focus();
    },

    cancel() {
      this.phase = "idle";
      panelIntercept = null;
      this.root.hidden = true;
      this.root.classList.remove("loading");
      machine.reset?.();
      setRunning(true);
    },

    flash(text) {
      this.msg.textContent = text;
      this.msg.classList.add("err");
      setTimeout(() => this.msg.classList.remove("err"), 900);
    },

    render() {
      const r = this.root;
      if (this.phase === "loading" || this.phase === "done") {
        r.innerHTML =
          `<div class="tg-head">READING TAPE</div>
           <div class="tg-barwrap"><div class="tg-bar"></div></div>
           <div class="tg-stat"></div>
           <div class="tg-actions"><button class="tg-skip">Skip</button></div>`;
        this.bar = r.querySelector(".tg-bar");
        this.stat = r.querySelector(".tg-stat");
        r.querySelector(".tg-skip").addEventListener("click", () => { this.turbo = true; });
        return;
      }
      r.innerHTML =
        `<div class="tg-head">PAPER-TAPE BOOTSTRAP <span>&mdash; key this into the panel</span></div>
         <div class="tg-msg"></div>
         <ol class="tg-list"></ol>
         <div class="tg-actions">
           <button class="tg-express">Enter it for me</button>
           <button class="tg-cancel">Cancel</button>
         </div>`;
      this.msg = r.querySelector(".tg-msg");
      const ol = r.querySelector(".tg-list");
      this.rows = this.steps.map((s) => {
        const li = document.createElement("li");
        li.textContent = s.text;
        ol.appendChild(li);
        return li;
      });
      r.querySelector(".tg-express").addEventListener("click", () => this.express());
      r.querySelector(".tg-cancel").addEventListener("click", () => this.cancel());
    },
  };

  // --- program loader --------------------------------------
  const dialog      = document.getElementById("romDialog");
  const romList     = document.getElementById("romList");
  const romDesc     = document.getElementById("romDesc");
  const romFile     = document.getElementById("romFile");
  const romFileName = document.getElementById("romFileName");
  const romAddr     = document.getElementById("romAddr");
  const romStart    = document.getElementById("romStart");

  const parseHex = (str, fallback = 0) => {
    const n = parseInt(String(str).trim().replace(/^0x/i, ""), 16);
    return Number.isFinite(n) ? n & 0xffff : fallback;
  };

  let catalog = [];       // manifest entries, each gains a `.bytes` (or null)
  let catalogLoaded = false;
  let lastLoad = null;    // remembered so RESET re-applies the same program

  async function loadCatalog() {
    if (catalogLoaded) return;
    catalogLoaded = true;
    romList.innerHTML = "";

    let manifest;
    try {
      const resp = await fetch("roms/manifest.json");
      if (!resp.ok) throw new Error("HTTP " + resp.status);
      manifest = await resp.json();
    } catch (err) {
      const opt = document.createElement("option");
      opt.textContent = "(roms/manifest.json not found)";
      opt.disabled = true;
      romList.appendChild(opt);
      console.warn("rom catalog:", err);
      return;
    }

    // A catalog entry names either one `file` or an ordered `files` list that
    // is fetched and concatenated (e.g. a ROM split across 2K PROM images).
    async function fetchImage(rom) {
      const names = rom.files || [rom.file];
      const parts = [];
      for (const name of names) {
        const r = await fetch("roms/" + name);
        if (!r.ok) return null;
        parts.push(new Uint8Array(await r.arrayBuffer()));
      }
      const total = parts.reduce((n, p) => n + p.length, 0);
      const out = new Uint8Array(total);
      let off = 0;
      for (const p of parts) { out.set(p, off); off += p.length; }
      return out;
    }

    catalog = manifest.roms || [];
    for (let i = 0; i < catalog.length; i++) {
      const rom = catalog[i];
      try {
        rom.bytes = await fetchImage(rom);
      } catch {
        rom.bytes = null;
      }
      const opt = document.createElement("option");
      opt.value = String(i);
      opt.textContent = rom.bytes
        ? `${rom.name}  —  ${rom.bytes.length} bytes`
        : `${rom.name}  —  (not installed)`;
      opt.disabled = !rom.bytes;
      romList.appendChild(opt);
    }

    const firstAvailable = catalog.findIndex((r) => r.bytes);
    if (firstAvailable >= 0) {
      romList.value = String(firstAvailable);
      syncSelection();
    }
  }

  function syncSelection() {
    const rom = catalog[Number(romList.value)];
    if (!rom) return;
    romDesc.textContent = rom.description || "";
    romAddr.value  = rom.load || "0x0000";
    romStart.value = rom.start || rom.load || "0x0000";
  }

  function applyProgram(bytes, load, start, label, sense = 0) {
    machine.clearMemory();
    machine.loadBytes(bytes, load);
    machine.reboot();
    applySense(sense);
    if (typeof machine.setPC === "function") machine.setPC(start);
    setRunning(true);
    lastLoad = { bytes, load, start, label, sense };
    term.clear();           // fresh screen for the new program; its output (if
    outQ.length = 0;        // any) is all that will appear
    term.focus();
  }

  const openDialog  = () => { dialog.hidden = false; loadCatalog(); };
  const closeDialog = () => { dialog.hidden = true; term.focus(); };

  document.getElementById("loadBtn").addEventListener("click", openDialog);

  // the how-to button shouts until it's clicked (or ~2 min pass), then it
  // settles down — and stays settled on later visits
  const helpBtn = document.getElementById("helpBtn");
  let helpTimer = null;
  function calmHelp() {
    if (helpTimer) { clearTimeout(helpTimer); helpTimer = null; }
    helpBtn.classList.remove("clickme");
    helpBtn.textContent = "How-To";
    try { localStorage.setItem("retro8080.helpseen", "1"); } catch {}
  }
  helpBtn.addEventListener("click", () => { printManual(); calmHelp(); term.focus(); });
  let helpSeen = false;
  try { helpSeen = localStorage.getItem("retro8080.helpseen") === "1"; } catch {}
  if (helpSeen) calmHelp();
  else helpTimer = setTimeout(calmHelp, 120000);
  document.getElementById("romCancel").addEventListener("click", closeDialog);
  document.getElementById("romClose").addEventListener("click", closeDialog);
  dialog.addEventListener("click", (e) => { if (e.target === dialog) closeDialog(); });
  romList.addEventListener("change", syncSelection);
  romFile.addEventListener("change", () => {
    romFileName.textContent = romFile.files[0] ? romFile.files[0].name : "no file selected";
  });

  document.getElementById("romLoad").addEventListener("click", async () => {
    const load   = parseHex(romAddr.value);
    const start  = parseHex(romStart.value);
    const method = document.querySelector('input[name="loadmethod"]:checked')?.value || "instant";
    const file   = romFile.files[0];

    let bytes, label, sense = 0;
    if (file) {
      bytes = new Uint8Array(await file.arrayBuffer());
      label = file.name;
    } else {
      const rom = catalog[Number(romList.value)];
      if (!rom || !rom.bytes) { romDesc.textContent = "That program is not installed."; return; }
      bytes = rom.bytes;
      label = rom.name;
      sense = parseHex(rom.sense, 0);
    }

    romFile.value = "";
    romFileName.textContent = "no file selected";
    closeDialog();

    if (method === "tape") {
      applySense(sense);
      tape.begin(bytes, load, start);
    } else {
      applyProgram(bytes, load, start, label, sense);
    }
  });

  // dev: ?tapedemo pre-opens the bootstrap guide for a fake 8 KB image;
  //      ?tapedemo=load also runs the express entry so you see the load bar
  if (location.search.includes("tapedemo")) {
    tape.begin(new Uint8Array(8192).fill(0x76), 0xe000, 0xe000);
    if (location.search.includes("load")) tape.express();
  }
  if (location.search.includes("help")) setTimeout(printManual, 300);

  // --- reset ----------------------------------------------
  document.getElementById("reset").addEventListener("click", () => {
    if (lastLoad) {
      applyProgram(lastLoad.bytes, lastLoad.load, lastLoad.start,
                   lastLoad.label, lastLoad.sense);
    } else {
      machine.reset();
      term.clear();
      outQ.length = 0;
      term.focus();
    }
  });
}

boot();
