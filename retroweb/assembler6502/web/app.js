// 6502 Assembler front end (real CG-OAC-6502 W65C02S hardware). Structure
// mirrors retroweb/altair8800/web/app.js
// (terminal profile system, per-frame CPU/serial pump) but this board has
// no front panel -- the left column is the board's actual controls
// (reset, LEDs, LCD, jumpers) instead.

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

  // ---- terminal ---------------------------------------------------------
  const term = new Terminal({ cursorBlink: true });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  const screenEl = document.getElementById("screen");
  const bezelEl = document.getElementById("bezel");
  const monitorEl = document.getElementById("monitor");
  const REF_W = 760, REF_H = 420;
  term.open(screenEl);
  fit.fit();

  screenEl.addEventListener("wheel", (e) => {
    if (!monitorEl.classList.contains("scrolls")) e.stopImmediatePropagation();
  }, { capture: true });

  function sizeScreen() {
    try {
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
    } catch {}
  }
  addEventListener("resize", sizeScreen);

  // ---- terminal profiles (ported from retroweb/altair8800/web/app.js) --
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
    // Returns a promise resolving once the resize this triggers has
    // actually run -- the initial boot call below awaits it so the
    // machine never starts writing real ROM output at a stale, wrong
    // column/row count from before the real font/size took effect (see
    // that call site's own comment for the race this prevents).
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

  // CAPS LOCK: vintage terminals were commonly uppercase-only, and this
  // board's own firmware (rom/bios.s's FORCE_UPPER) already expects it --
  // default on, like the Altair front end's identical control. The ASR-33
  // profile is mechanically incapable of lowercase, so it forces this on
  // and disables the checkbox rather than merely defaulting it. Declared
  // before the first applyProfile() call below (which reads `caps`) --
  // that call is now awaited at top level (see its own comment), which
  // pauses boot() right there until fonts settle, so anything applyProfile
  // touches has to already exist textually above this point, not below.
  const caps = document.getElementById("caps");
  try { caps.checked = localStorage.getItem("cgoac6502.caps") !== "0"; } catch {}
  caps.addEventListener("change", () => {
    try { localStorage.setItem("cgoac6502.caps", caps.checked ? "1" : "0"); } catch {}
  });

  // the period profiles' VT323/Courier Prime faces are @font-face'd locally
  // (vendor/fonts/) -- load them before the first paint of a retro profile
  // so it doesn't flash in the browser's fallback monospace first. Awaited
  // (not fire-and-forget) -- boot() below must not create the machine and
  // pressReset() until the terminal is sized against its real, final font,
  // or a slow font/network fetch can lose the race: the ROM's first output
  // (Wozmon's own "\" banner) would get written using column/row counts
  // measured against the browser's fallback font, then get corrupted when
  // the real font swap resizes/refreshes the terminal underneath already-
  // written content -- exactly the "have to refresh a few times before the
  // prompt shows up" symptom this fixes, since a warm font cache on repeat
  // loads wins the race by accident, masking it as intermittent.
  await Promise.all([
    document.fonts?.load('20px "VT323"'),
    document.fonts?.load('15px "Courier Prime"'),
    /* v8 ignore next -- font-load rejection is swallowed */
  ].filter(Boolean)).catch(() => {});
  await applyProfile(termSelect.value);
  termSelect.addEventListener("change", () => applyProfile(termSelect.value));

  // ---- floating popups (Save/Load, Help) ---------------------------------
  // Same draggable-by-title-bar / remembered-position mechanism as
  // retroweb/altair8800/web/app.js's front-panel bootstrap guide (pgDrag/
  // pgFloat) -- unlike that guide's dynamically-rebuilt content, these two
  // are static markup (index.html), so this is just show/hide/drag wiring,
  // generalized over any number of trigger buttons per popup (Save and
  // Load both open the one shared Save/Load popup).
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

  // ---- page theme (Win95 / mid-90s Mosaic web / Modern / Dark Modern) ---
  // Shared with every other page on the site via the retro8080.theme
  // localStorage key -- a theme picked here or on the landing page carries
  // across. "moderndark" is Modern's layout with data-mode="dark" bolted on.
  const pageTheme = document.getElementById("pageTheme");
  const root = document.documentElement;
  const THEME_VALUES = ["win", "web94", "modern", "moderndark"];
  const applyTheme = (v) => {
    if (!THEME_VALUES.includes(v)) v = "win";
    if (v === "moderndark") { root.dataset.theme = "modern"; root.dataset.mode = "dark"; }
    else { root.dataset.theme = v; delete root.dataset.mode; }
    return v;
  };
  let storedTheme; try { storedTheme = localStorage.getItem("retro8080.theme"); } catch {}
  let savedPageTheme = applyTheme(
    new URLSearchParams(location.search).get("theme") || storedTheme || root.dataset.theme || "win");
  pageTheme.value = savedPageTheme;
  pageTheme.addEventListener("change", () => {
    applyTheme(pageTheme.value);
    try { localStorage.setItem("retro8080.theme", pageTheme.value); } catch {}
    setTimeout(sizeScreen, 60);   // page width may have changed
  });

  // ---- wasm machine -----------------------------------------------------
  const Module = await CgOac6502({});
  const m = new Module.Machine();
  window.__machine = m;    // exposed for the Playwright suite / manual debugging
  window.__term = term;    // ditto -- lets tests read xterm's own line buffer directly
  term.focus();

  // default ROM: built from cpu6502/rom/ source at build time (make -C .. rom),
  // not fetched -- see Makefile. Already seated in the socket, exactly as
  // the real board would arrive. The one-time ROM-programmer UI (ZIF
  // socket, burn/verify, chip library) is retired -- m.burnRom() here is
  // all that's left of it, seating the compiled firmware at boot.
  try {
    const res = await fetch("roms/firmware.bin");
    if (res.ok) m.burnRom(new Uint8Array(await res.arrayBuffer()));
  } catch {}
  m.pressReset();

  // Tracks whether the terminal is currently sitting inside the command
  // shell (editor.s's SHELL_ENTRY) or at raw Wozmon's own "\" prompt, so
  // ensureShell() (below) knows whether it needs to send the entry
  // sequence before SAVE/LOAD. This can NOT be tracked just from app.js's
  // own sends: the Help panel explicitly tells a human to type "<addr>R"
  // and "QUIT" themselves, so real keystrokes -- not just this file's
  // synthetic ones -- move the machine between the two. Getting this
  // wrong is a real, silent-corruption bug, not just a UI nicety: if a
  // human enters the shell by hand, types a program, then clicks Save,
  // ensureShell() (believing it's still at Wozmon) would resend "<addr>R"
  // into the *already-open* shell prompt -- misparsed as a decimal line
  // number followed by a bad trailing letter (e.g. "8000R" -> line 8000,
  // text "R"), silently polluting the program with a bogus line right
  // before SAVE captures it. So both transitions are inferred from
  // real signals instead of only from this file's own sends: entering is
  // detected from the shell's own banner appearing in ROM *output*
  // (unambiguous -- it only ever prints on a real SHELL_ENTRY, human- or
  // script-triggered), and leaving is detected from the human's own typed
  // input completing a "QUIT" line (unambiguous the other way: it's
  // exactly what was typed, not an inference from echoed bytes). SW1
  // reset also forces it back to false, unconditionally.
  let inShell = false;
  const SHELL_BANNER = "ASSEMBLY CODER";
  let bannerTail = "";           // rolling window of recent output, for the banner check above
  function ensureShell() {
    if (inShell) return;
    queueInput(encoder.encode(hex(E.SHELL_ENTRY) + "R\r"));
    inShell = true;
  }

  // ---- terminal -> ACIA, paced like a real transfer ----------------------
  // Every typed/pasted/loaded byte is queued here and drained by
  // driveFrame() in the main loop below -- realistic-by-default (paced to
  // the ACIA's live baud), not the previous unmetered "push every byte
  // instantly" (see driveFrame's own header for the full writeup).
  const encoder = new TextEncoder();
  const inQ = [];
  function queueInput(bytes) { for (const b of bytes) inQ.push(b); }
  let typedLine = "";           // the human's own current unsent line, for the QUIT check above
  term.onData((data) => {
    if (!poweredOn) return;   // Power toggle (below) -- an unplugged board doesn't hear you type
    if (caps.checked) data = data.toUpperCase();
    for (const ch of data) {
      if (ch === "\r" || ch === "\n") {
        if (typedLine.trim().toUpperCase() === "QUIT") inShell = false;
        typedLine = "";
      } else if (ch === "\x7f" || ch === "\b") {
        typedLine = typedLine.slice(0, -1);
      } else {
        typedLine += ch;
      }
    }
    queueInput(encoder.encode(data));
  });

  // ---- ACIA -> terminal, metered at the ACIA's live configured baud ----
  // (a fidelity improvement over a fixed per-profile rate -- the real chip's
  // baud is a genuine register, see acia65c51.h)
  const outQ = [];
  let baudBudget = 0;
  const baudLabel = document.getElementById("baudLabel");
  let lastBaud = -1;

  // SAVE_ENTRY frames the source buffer in real STX ($02)/ETX ($03) control
  // bytes (see load.s) -- the Save panel below watches this same raw output
  // stream for that frame, independent of (and not consumed by) the terminal
  // display's own outQ, since SAVE's output is meant to be visible on screen
  // too, exactly like a human running SAVE_ENTRY by hand would see.
  let saveCapture = null;   // null, or { started, buf: number[], resolve }
  let pendingLf = false;    // display-only CR->CRLF state, carried across frames -- see pullSerial()

  function pullSerial() {
    if (outQ.length > 256) return;
    const out = m.readOutput();
    for (let i = 0; i < out.length; i++) {
      const b = out[i];
      // SAVE's wire format is deliberately bare-CR-separated between lines
      // (see load.s) -- correct for the actual transfer (round-trips with
      // LOAD, and is exactly what a real external listener should see, so
      // saveCapture below taps the real, untouched `b`), but a bare CR with
      // no LF behind it just returns a real terminal's cursor to column 0
      // without advancing a row, so each displayed line would overwrite
      // the previous one in place -- the same overwrite artifact DO_LOAD's
      // own incoming-echo already works around on the ROM side (its header
      // comment). This is the display-only fix for the outgoing direction:
      // outQ (this on-page terminal's own draw queue) gets a synthetic LF
      // appended whenever a CR isn't immediately followed by a real one,
      // so SAVE's dump reads one line per row and a following "Ok" doesn't
      // land on top of the last line -- nothing else here (saveCapture,
      // the banner check) ever sees this synthetic byte.
      if (pendingLf) {
        pendingLf = false;
        if (b !== 0x0a) outQ.push(0x0a);
      }
      outQ.push(b);
      if (b === 0x0d) pendingLf = true;
      if (!inShell && b >= 0x20 && b < 0x7f) {
        bannerTail = (bannerTail + String.fromCharCode(b)).slice(-SHELL_BANNER.length);
        if (bannerTail === SHELL_BANNER) inShell = true;
      }
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
    if (n === 0 && outQ.length && !cps) n = outQ.length;   // idle ACIA: drain immediately, nothing to meter against
    if (n > 0 && outQ.length) writeFiltered(outQ.splice(0, Math.min(n, outQ.length)));
  }

  // ---- ACIA <- terminal/paste/load, paced at the same live baud ---------
  // Previously term.onData pushed every byte to m.typeChar() with zero
  // pacing at all -- fine for a human's own keystrokes (naturally paced by
  // typing speed) but not for a paste or a Save/Load transfer. Mirrors the
  // Altair's LOAD SPEED convention (see retro/CLAUDE.md): realistic-by-
  // default (paced to m.aciaBaud(), same cps math as pumpTerminal above),
  // a labelled "instant transfer" opt-out (Save/Load panel), and the
  // standard ?test=1 carve-out.
  //
  // Critically, this can't just be "call m.typeChar() N times, then let
  // the frame's usual m.runCycles() catch up" -- the ACIA has only a
  // one-byte RX register (acia65c51.h), so N>1 typeChar() calls with zero
  // CPU cycles between them is a real overrun: byte 2 overwrites byte 1
  // before the NMI handler ever drains it, silently dropping data. Nor can
  // driveFrame() just inject extra m.runCycles() of its own -- that would
  // speed up the emulated CPU beyond real 1MHz, which retro/CLAUDE.md
  // never allows, "instant transfer" included (real-hardware overrides may
  // change *throughput*, never the clock itself -- see the Altair's own
  // LOAD SPEED, which stays at real 2MHz under every multiplier). So
  // driveFrame() below slices THIS frame's own real, dtMs-derived cycle
  // budget across however many characters are due out this frame, running
  // a fair share of real cycles between each -- the same total CPU time
  // as an ordinary frame, just distributed so the NMI handler gets a
  // genuine chance to drain each byte before the next one arrives.
  const TEST_MODE = new URLSearchParams(location.search).get("test") === "1";
  const instantXfer = document.getElementById("instantXfer");
  const MIN_CYCLES_PER_CHAR = 200;   // generous margin over the NMI handler's real drain cost
  let inBudget = 0;

  // Runs exactly `totalCycles` of real CPU time (same as a plain
  // m.runCycles(totalCycles) call) for this frame, but interleaves up to
  // `n` due characters from inQ across that budget instead of dumping
  // them all in before/after it.
  function driveFrame(totalCycles, dtMs) {
    if (!inQ.length) { m.runCycles(totalCycles); return; }

    const instant = TEST_MODE || instantXfer.checked;
    let n;
    if (instant) {
      // Not throttled to the ACIA's baud -- but still capped to what this
      // frame's own real cycle budget can safely interleave, so it's
      // faster than realistic pacing without ever dropping a byte or
      // running the CPU a single cycle ahead of real time.
      n = Math.floor(totalCycles / MIN_CYCLES_PER_CHAR);
    } else {
      const baud = m.aciaBaud();
      const cps = baud ? baud / 10 : 10;   // idle ACIA: a slow, non-stalling default
      inBudget += (dtMs / 1000) * cps;
      n = Math.floor(inBudget);
    }
    n = Math.max(0, Math.min(n, inQ.length));
    if (instant) { /* budget not tracked in this mode */ } else { inBudget -= n; }

    if (n === 0) { m.runCycles(totalCycles); return; }
    const slice = Math.floor(totalCycles / n);
    for (let i = 0; i < n; i++) {
      m.typeChar(inQ.shift());
      m.runCycles(i === n - 1 ? totalCycles - slice * (n - 1) : slice);
    }
  }

  // ---- LEDs / LCD ---------------------------------------------------
  // D1/D4/D7 render on the board graphic itself now, not a separate panel.
  const pcbLedD1 = document.getElementById("pcbLedD1");
  const pcbLedD4 = document.getElementById("pcbLedD4"), pcbLedD7 = document.getElementById("pcbLedD7");
  function flashLed(el) {
    el.classList.add("on");
    clearTimeout(el._t);
    el._t = setTimeout(() => el.classList.remove("on"), 90);
  }
  // ---- J3 LCD accessory: an actual HD44780-style 5x7 dot-matrix render,
  // not plain text -- see lcdfont.js. Each character cell shows its full
  // dot grid (lit and unlit dots both visible, like the real thing), drawn
  // to a <canvas> rather than 32 * 35 individual DOM nodes.
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

  // ---- reset / jumpers -------------------------------------------------
  // the PCB graphic's own SW1 (labelled RST) doubles as a real control,
  // same as the board panel. J7 (interrupt routing), J5 (BOOT select) and
  // J8 (RTS->CTS) aren't exposed as controls on this page -- see
  // index.html's own note by the board graphic -- so there's no listener
  // to wire up for any of them; bus.h's JumperState defaults already
  // match the shipped ROM's wiring.
  document.querySelector('#pcbSvg [data-ref="SW1"]').addEventListener("click", () => { m.pressReset(); inShell = false; bannerTail = ""; typedLine = ""; });

  // ---- power (J1) ---------------------------------------------------
  // The real board has no power switch -- J1 is just a barrel jack, live
  // whenever plugged in -- so this is a labelled UI convenience layered
  // onto that graphic, not a claim about real hardware. "Off" pauses the
  // main loop in place (frame() below skips CPU/serial/LCD work) rather
  // than resetting anything, so a typed-in-progress program survives a
  // power cycle -- closer to "the monitor's unplugged" than "the machine
  // lost its memory". D1 (real: hardwired straight to +5V, always lit
  // whenever the page is open -- see machine.h) gets its own dim/off look
  // here purely for this toggle's visual feedback.
  let poweredOn = true;
  document.querySelector('#pcbSvg [data-ref="J1"]').addEventListener("click", () => {
    poweredOn = !poweredOn;
    pcbLedD1.classList.toggle("led-power", poweredOn);
    monitorEl.classList.toggle("powered-off", !poweredOn);
  });

  // ---- Help panel: fill in this build's real, generated shell address --
  // (see gen_entrypoints.py/roms/entrypoints.js -- never hand-copied, so
  // this can't go stale the way a hardcoded address in this file would).
  // Only one real address to show now -- NEW/LIST/EDIT/ASM/RUN/LOAD/SAVE
  // are typed command words at the shell's own prompt, not separate
  // addresses, so the rest of the Help panel's command grammar is static
  // prose (index.html) rather than filled in here.
  const E = CGOAC_ENTRYPOINTS;
  function hex(n) { return n.toString(16).toUpperCase(); }
  for (const id of ["hShell", "hShell2"]) document.getElementById(id).textContent = hex(E.SHELL_ENTRY) + "R";
  document.getElementById("hResume").textContent = "JMP $" + hex(E.SHELL_PROMPT);
  // OS-call jump table (bios.s) -- same "never hand-copied" reasoning.
  const OS_CALL_IDS = {
    hPrintChar: "PRINT_CHAR", hPrintStr: "PRINT_STR",
    hLcdPutc: "LCD_PUTC", hLcdPuts: "LCD_PUTS", hLcdClear: "LCD_CLEAR",
    hLcdLine1: "LCD_LINE1", hLcdLine2: "LCD_LINE2",
  };
  for (const [id, name] of Object.entries(OS_CALL_IDS)) document.getElementById(id).textContent = "$" + hex(E[name]);

  // ---- Save / Load (the shell's own LOAD/SAVE commands) -----------------
  // Filename input, a named localStorage shelf (same pattern the retired
  // ROM programmer's chip library used, repurposed for saved programs), a
  // real file <a download>, a plain <input type=file> import. Every
  // transfer runs entirely over the simulated ACIA -- typing "LOAD"/"SAVE"
  // at the shell's own prompt, same as a human would by hand; nothing here
  // is a side channel into the emulated machine.
  const pgmName = document.getElementById("pgmName");
  const pgmSaveBtn = document.getElementById("pgmSave");
  const pgmDownload = document.getElementById("pgmDownload");
  const pgmFileBtn = document.getElementById("pgmFileBtn");
  const pgmFile = document.getElementById("pgmFile");
  const pgmLib = document.getElementById("pgmLib");
  const pgmStatus = document.getElementById("pgmStatus");

  function setPgmStatus(text, ok) {
    pgmStatus.className = ok === undefined ? "muted" : (ok ? "ok" : "err");
    pgmStatus.textContent = text;
  }
  function defaultPgmName() { return pgmName.value.trim() || "program.asm"; }

  // Enters the shell (if not already in it) and runs its SAVE command,
  // resolving with the captured source text -- the bytes between STX/ETX
  // (pullSerial()'s saveCapture hook above). A generous timeout guards
  // against the ROM never responding.
  function runSave() {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        saveCapture = null;
        reject(new Error("SAVE timed out -- no response from the ROM."));
      }, 8000);
      saveCapture = { started: false, buf: [], resolve: (bytes) => { clearTimeout(timer); resolve(bytes); } };
      ensureShell();
      queueInput(encoder.encode("SAVE\r"));
    });
  }
  // Enters the shell (if not already in it) and runs its LOAD command,
  // which clears the program itself (DO_LOAD's own DO_NEW) before
  // streaming `text` in -- queued as one sequence; the shell processes
  // each typed/received line in turn as driveFrame drains it, so no
  // explicit wait between commands is needed. DO_LOAD splits the incoming
  // stream on a bare CR ($0D, the same byte the shell's own line entry
  // and Wozmon's store syntax use) -- normalize LF/CRLF from a hand-
  // edited or OS-saved .asm/.txt file so an imported file loads correctly
  // regardless of which line endings its editor wrote. A numberless line
  // (a plain unnumbered file, or one hand-typed without line numbers)
  // auto-numbers on the way in -- see PROCESS_LINE/AUTO_NUMBER, editor.s.
  function runLoad(text) {
    text = text.replace(/\r\n|\n/g, "\r");
    ensureShell();
    queueInput(encoder.encode("LOAD\r" + text));
  }

  const PGM_LIB_KEY = "cgoac6502.programs";
  function loadPgmLib() { try { return JSON.parse(localStorage.getItem(PGM_LIB_KEY) || "{}"); } catch { return {}; } }
  function savePgmLib(lib) { try { localStorage.setItem(PGM_LIB_KEY, JSON.stringify(lib)); } catch {} }
  function renderPgmLib() {
    const lib = loadPgmLib();
    pgmLib.innerHTML = "";
    for (const name of Object.keys(lib)) {
      const b = document.createElement("button");
      b.className = "chip"; b.textContent = name;
      b.addEventListener("click", () => {
        runLoad(lib[name]);
        pgmName.value = name;
        setPgmStatus(`Loading "${name}" -- watch the terminal for the fresh prompt.`);
      });
      pgmLib.appendChild(b);
    }
  }
  renderPgmLib();

  function offerDownload(name, text) {
    const url = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
    if (pgmDownload._url) URL.revokeObjectURL(pgmDownload._url);
    pgmDownload._url = url;
    pgmDownload.href = url;
    pgmDownload.download = name;
    pgmDownload.hidden = false;
  }

  pgmSaveBtn.addEventListener("click", async () => {
    pgmSaveBtn.disabled = true;
    setPgmStatus("Saving (reading the source buffer over the ACIA)...");
    try {
      const bytes = await runSave();
      let text = "";
      for (let i = 0; i < bytes.length; i++) text += String.fromCharCode(bytes[i]);
      const name = defaultPgmName();
      const lib = loadPgmLib();
      lib[name] = text;
      savePgmLib(lib);
      renderPgmLib();
      offerDownload(name, text);
      setPgmStatus(`Saved ${bytes.length} byte(s) as "${name}".`, true);
    } catch (e) {
      setPgmStatus(e.message || String(e), false);
    } finally {
      pgmSaveBtn.disabled = false;
    }
  });

  pgmFileBtn.addEventListener("click", () => pgmFile.click());
  pgmFile.addEventListener("change", async () => {
    const f = pgmFile.files[0];
    if (!f) return;
    const text = await f.text();
    pgmName.value = f.name;
    runLoad(text);
    setPgmStatus(`Loading "${f.name}" (${text.length} byte(s)) -- watch the terminal for the fresh prompt.`);
  });

  // ---- main loop ------------------------------------------------------
  const CLOCK_HZ = 1_000_000;   // X1, real 1MHz -- never sped up (retro/CLAUDE.md)
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
