// Bridges the WebAssembly 8080 machine to an xterm.js terminal.
//
//   xterm keystrokes ──► machine.sendByte()      (serial channel A RX FIFO)
//   machine.readOutput() ──► xterm.write()       (serial channel A TX FIFO)
//
// The CPU is advanced once per animation frame by a fixed slice of cycles;
// the 2SIO ring buffers absorb the rate mismatch between the two clocks.

const CPU_HZ = 2_000_000;   // Altair 8080A clock; emulation is paced to real time

const hex = (n, w = 2) => n.toString(16).toUpperCase().padStart(w, "0");

// Shown by the disk cabinet's "?" button under the disk-specific notes.
const CPM_PRIMER =
`The A> prompt is the console. Type a command and press Enter;
case doesn't matter. "A>" means drive A is current.

FILES
  DIR              list files on the current drive
  DIR B:           list files on drive B:
  DIR *.COM        list only the .COM files
  TYPE READ.ME     show a text file  (Ctrl-S pause, Ctrl-Q resume)
  ERA JUNK.TXT     erase a file      (ERA *.BAK  erases every .BAK)
  REN NEW=OLD      rename OLD to NEW
  B:               switch to drive B:   (A: switches back)

RUN A PROGRAM
  Type its name without ".COM":
      STAT               disk / file sizes and free space
      PIP B:=A:*.*        copy every file from A: to B:
  When a program finishes it drops you back at A> on its own.

STOP A PROGRAM
  Ctrl-C  at the A> prompt      warm boot (re-reads the disk)
  Ctrl-C  during a program      usually aborts it back to A>
  otherwise use the program's own quit command (see above)

There is no shut-down -- just stop, or eject the disk.`;

const DOS_PRIMER =
`Altair DOS is not CP/M. The prompt is a period:  .

  .DIR             list files
  .MNT 0           mount the disk in drive 0
  .NAME            run the program called NAME

MITS shipped this before CP/M and it shows -- boot a CP/M disk
instead unless you're here for the history.`;

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
      /* v8 ignore next -- html-escape, only exercised if a fail() message has <>& */
      String(msg).replace(/[<>&]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;" }[c])) +
      "</pre>";
  }
}

/* v8 ignore start -- last-resort global error nets */
window.addEventListener("error", (e) => fail(e.message + "\n" + (e.error?.stack || "")));
window.addEventListener("unhandledrejection", (e) => fail("promise rejected: " + (e.reason?.stack || e.reason)));
/* v8 ignore stop */

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
  /* v8 ignore start -- scroll-lock guard, only runs on a real wheel event */
  screenEl.addEventListener("wheel", (e) => {
    if (!monitorEl.classList.contains("scrolls")) e.stopImmediatePropagation();
  }, { capture: true });
  /* v8 ignore stop */

  // Fill the terminal's column with the monitor: screen as wide as fits (minus
  // the monitor's own frame), locked to 24 rows; column count floats with the
  // font. The measured box is .ws-terminal — the full content column in the
  // single-column layout, or just the left grid track when Modern goes
  // side-by-side on a wide screen.
  const termBox = screenEl.closest(".ws-terminal") || screenEl.closest(".inner");
  function sizeScreen() {
    try {
      // 1. measure the font at a reference size
      screenEl.style.width = REF_W + "px";
      screenEl.style.height = REF_H + "px";
      fit.fit();
      if (!term.cols || !term.rows) return;
      const cw = REF_W / term.cols, ch = REF_H / term.rows;   // cell size for this font

      // 2. the monitor's own frame, measured with the screen collapsed so a
      //    max-width clamp on the monitor (narrow column) can't corrupt the delta
      screenEl.style.width = "0px";
      const frame = Math.max(0, monitorEl.offsetWidth - screenEl.offsetWidth);

      // 3. room available in the terminal's column
      let avail = window.innerWidth;
      if (termBox) {
        const cs = getComputedStyle(termBox);
        avail = termBox.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
      }
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
  // the terminal's column can also change width without a window resize — the
  // Modern side-by-side breakpoint, or the panel column reflowing — so watch it
  /* v8 ignore start -- ResizeObserver callback, layout-driven */
  if (termBox && window.ResizeObserver) {
    let pending = false;
    new ResizeObserver(() => {
      if (pending) return;
      pending = true;
      requestAnimationFrame(() => { pending = false; sizeScreen(); });
    }).observe(termBox);
  }
  /* v8 ignore stop */

  // Modern side-by-side folds the TERMINAL settings into the PRESET bar; every
  // other layout keeps that bar directly above the monitor.
  const toolbarsWrap = document.querySelector(".toolbars");
  const termBar = document.querySelector(".term-bar");
  const wideLayout = window.matchMedia("(min-width: 1560px)");
  function placeTermBar() {
    if (!toolbarsWrap || !termBar || !termBox) return;
    const merged = document.documentElement.dataset.theme === "modern" && wideLayout.matches;
    if (merged) toolbarsWrap.appendChild(termBar);
    else termBox.insertBefore(termBar, termBox.firstChild);
  }
  placeTermBar();
  wideLayout.addEventListener("change", () => { placeTermBar(); sizeScreen(); });
  // A real serial terminal shows nothing at power-on but a cursor — it has no
  // idea what (if anything) is on the other end of the wire. So no banner.

  // --- serial terminal profiles ----------------------------------
  // CAPS LOCK (the terminal's ALPHA-LOCK) is a plain manual toggle, default on
  // — most Altair-era software wants uppercase and many terminals were
  // uppercase-only anyway. Profiles don't touch it.
  const caps = document.getElementById("caps");

  // --- per-profile output realism ---------------------------------------
  // xterm.js is always a full VT100+/xterm parser. A real period terminal
  // wasn't: an ASR-33 has no video memory to address, a VT52 speaks a
  // different escape set than ANSI, an ADM-3A addresses the cursor with its
  // own encoding and no ANSI at all. Each profile's `filter` factory returns
  // a stateful byte-stream transform (state must survive across metered
  // chunks -- a slow baud rate can split an escape sequence over many calls)
  // that runs just before bytes reach xterm; profiles without one (the VT100s,
  // Modern) are genuinely ANSI/xterm-compatible and pass through unchanged.
  // See ALTAIR_REVIEW.md §4.

  // ASR-33 / Glass TTY: no cursor addressing, no clear screen, no reverse
  // video -- these were printing (ASR-33) or minimal glass (early VDT)
  // terminals with no escape-sequence handling at all. Only CR, LF, BS, BEL
  // and HT are real controls; anything that looks like ESC or ESC[...final
  // is swallowed rather than reaching xterm's parser. BS itself needs no
  // translation: xterm's default backspace already just moves the cursor
  // left without erasing, exactly like a print head backing up to overstrike.
  function dumbFilter() {
    let state = "ground";                 // ground | esc | csi
    const KEEP_C0 = new Set([0x07, 0x08, 0x09, 0x0a, 0x0d]); // BEL BS HT LF CR
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

  // DEC VT52: cursor moves / home / erase share ANSI's final letter but need
  // the CSI "[" xterm requires; direct addressing (ESC Y row col, each +32)
  // becomes an ANSI CUP; keypad-mode and identify sequences have no xterm
  // equivalent and are dropped rather than mistranslated.
  function vt52Filter() {
    let state = "ground";                 // ground | esc | Y1 (row byte pending)
    let row = 0;
    const CSI_LETTER = new Set(["A", "B", "C", "D", "H", "J", "K"]); // same letter in both sets
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
          if (c === "I") { push("\x1bM"); continue; }        // reverse line feed
          if (c === "Y") { state = "Y1"; continue; }          // direct cursor address
          continue;                                            // Z / = / > / F / G: drop
        }
        if (b === 0x1b) { state = "esc"; continue; }
        out.push(b);
      }
      return out;
    };
  }

  // Lear Siegler ADM-3A: cursor moves are bare control codes (no ESC), not
  // ANSI's CSI letters -- ^H left (needs no translation, same as BS), ^J down
  // (needs no translation, same as LF), ^K up, ^L right, ^Z clear+home. Direct
  // addressing is ESC = row col (each +32, matching VT52's Y). This is
  // exactly the terminal driver WordStar's ADM-3A mode emits.
  function adm3aFilter() {
    let state = "ground";                 // ground | esc | eq1
    let row = 0;
    return (bytes) => {
      const out = [];
      const push = (s) => { for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i)); };
      for (const b of bytes) {
        if (state === "eq1") { row = b - 0x20; state = "eq2"; continue; }
        if (state === "eq2") { push(`\x1b[${row + 1};${b - 0x20 + 1}H`); state = "ground"; continue; }
        if (state === "esc") { state = (b === 0x3d) ? "eq1" : "ground"; continue; }  // only ESC =
        if (b === 0x1b) { state = "esc"; continue; }
        if (b === 0x0b) { push("\x1b[A"); continue; }   // ^K: cursor up
        if (b === 0x0c) { push("\x1b[C"); continue; }   // ^L: cursor right (not form-feed here)
        if (b === 0x1a) { push("\x1b[H\x1b[2J"); continue; } // ^Z: clear screen, home
        out.push(b);
      }
      return out;
    };
  }

  const TERM_PROFILES = {
    modern: { label: "Modern (xterm)",
      font: 'ui-monospace, Menlo, Consolas, "DejaVu Sans Mono", monospace',
      size: 15, fg: "#33ff88", bg: "#000000", dim: "#1c8f52", br: "#8affc0",
      crt: "none", baud: 0, cursor: "block", blink: true, glow: 0 },
    vt100g: { label: "DEC VT100 · green",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#39ff41", bg: "#0a140a", dim: "#1c8c1c", br: "#a6ffa6",
      crt: "scan", baud: 9600, cursor: "block", blink: true, glow: 2 },
    vt100a: { label: "DEC VT100 · amber",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#ffb32b", bg: "#180d00", dim: "#a8701a", br: "#ffd77e",
      crt: "scan", baud: 9600, cursor: "block", blink: true, glow: 2 },
    vt52: { label: "DEC VT52",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#4dff4d", bg: "#061006", dim: "#1c8c1c", br: "#b6ffb6",
      crt: "scanheavy", baud: 4800, cursor: "block", blink: false, glow: 3,
      filter: vt52Filter },
    adm3a: { label: "Lear Siegler ADM-3A",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#39ff9c", bg: "#03100b", dim: "#1c8c5c", br: "#a6ffce",
      crt: "scan", baud: 9600, cursor: "underline", blink: true, glow: 2,
      filter: adm3aFilter },
    glasstty: { label: "Glass TTY",
      font: '"VT323", "Courier New", monospace', size: 19,
      fg: "#c8ffc8", bg: "#020802", dim: "#5c9c5c", br: "#ecffec",
      crt: "scan", baud: 300, cursor: "block", blink: true, glow: 2,
      filter: dumbFilter },
    tty33: { label: "Teletype ASR-33",
      font: '"Courier Prime", "Courier New", monospace', size: 15,
      fg: "#242424", bg: "#efe8d6", dim: "#7a7261", br: "#000000",
      crt: "paper", baud: 110, cursor: "underline", blink: false,
      glow: 0, bell: "ding", scrollback: 5000,   // paper roll -> scroll back
      filter: dumbFilter, forceCaps: true },
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
    termFilter = p.filter ? p.filter() : null;   // fresh state -- a mid-escape switch shouldn't leak
    // the ASR-33 is mechanically incapable of lowercase; not the user's choice
    caps.disabled = !!p.forceCaps;
    if (p.forceCaps) caps.checked = true;
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
    /* v8 ignore next -- wasm instantiation failure path */
    return fail("wasm module failed to instantiate:\n" + (err?.stack || err));
  }
  const machine = new Module.Machine();
  machine.reset();
  term.focus();

  // --- terminal -> CPU ----------------------------------------------
  const encoder = new TextEncoder();
  term.onData((data) => {
    turbo = false;            // the player is here now — back to authentic 2 MHz
    // a keypress skips to the end of a slow how-to printout
    if (printingManual) {
      writeFiltered(outQ.splice(0));
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

  // `rxWatch` is a rolling copy of recent CPU output the loaders poll to sync
  // with BASIC's cold-start prompts; `loaderToken` is bumped on every new load
  // so a stale async loader can tell it has been superseded and bail.
  let rxWatch = "";
  let loaderToken = 0;
  let turbo = false;       // run the CPU faster than 2 MHz: during a load, and
                           // briefly after, so a program's one-time setup (the
                           // galaxy in Star Trek is ~20 s of real 8080 time)
                           // doesn't look like a hang. Ends on the first keypress.

  let baudCps = 0;          // 0 = unthrottled; else characters/second
  let baudBudget = 0;
  let termFilter = null;    // current profile's output filter, or null (pass-through)
  let printingManual = false;   // the how-to is metered at the terminal's baud
  let baudSaved = 0;            // real baud, restored when the how-to finishes
  let crlfPending = false;      // a CR ended the last readOutput() batch; the LF
                                // decision waits for the next batch (see below)
  let coldStartWatch = false;   // capture rxWatch during a cold start, but the output
                                // still reaches the terminal (4K BASIC's prompts)

  function pullSerial() {
    // stay mostly drained so the 2SIO FIFO does the buffering + TDRE backpressure
    if (outQ.length > 128) return;
    const out = machine.readOutput();
    const n = out.length;
    if (n === 0) return;

    // "CR -> CR/LF" adds a line feed after a bare carriage return -- but only a
    // *bare* one. Skip it when an LF already follows, and when another CR follows
    // (4K BASIC ends lines with "CR CR LF" -- Teletype head-return padding -- and
    // an injected LF between the two CRs would double-space every line). The
    // decision can straddle two readOutput() batches, hence crlfPending.
    const notBareCr = (c) => c === 0x0a || c === 0x0d;
    /* v8 ignore next 4 -- a CR landing exactly on a readOutput() batch boundary */
    if (crlfPending) {
      if (crlf.checked && !notBareCr(out[0] & 0x7f)) outQ.push(0x0a);
      crlfPending = false;
    }

    for (let i = 0; i < n; i++) {
      // 1970s serial terminals are 7-bit ASCII; the 8th bit is parity/ignored.
      // Altair BASIC's LIST relies on this — it sets bit 7 on the first letter
      // of every tokenised keyword (PRINT -> 0xD0,'RINT'), so without the mask
      // that leading letter renders as a stray C1 control and vanishes.
      const b = out[i] & 0x7f;
      outQ.push(b);
      if (crlf.checked && b === 0x0d) {
        if (i + 1 < n) { if (!notBareCr(out[i + 1] & 0x7f)) outQ.push(0x0a); }
        else crlfPending = true;         // decide when the next batch lands
      }
    }
    if (coldStartWatch) {
      for (let i = 0; i < n; i++) rxWatch += String.fromCharCode(out[i] & 0x7f);
      if (rxWatch.length > 4096) rxWatch = rxWatch.slice(-2048);
    }
  }

  function pumpTerminal(dtMs) {
    /* v8 ignore next 4 -- baud restored when a metered how-to finishes on its own */
    if (printingManual && outQ.length === 0) {
      printingManual = false;
      baudCps = baudSaved;
    }
    pullSerial();
    if (outQ.length === 0) return;
    if (baudCps === 0) { writeFiltered(outQ.splice(0)); return; }
    baudBudget += (baudCps * dtMs) / 1000;
    baudBudget = Math.min(baudBudget, baudCps);   // cap catch-up after a stall
    const n = Math.min(outQ.length, Math.floor(baudBudget));
    if (n > 0) { baudBudget -= n; writeFiltered(outQ.splice(0, n)); }
  }

  // the selected profile's escape-handling filter, if it has one (§ above) --
  // ASR-33/Glass TTY strip escapes entirely, VT52/ADM-3A translate their own
  // codes to what xterm understands. CRT profiles that really speak ANSI pass
  // straight through.
  function writeFiltered(bytes) {
    const out = termFilter ? termFilter(bytes) : bytes;
    if (out.length) term.write(new Uint8Array(out));
  }

  function flushTerminal() {           // used by SINGLE STEP — show it now
    pullSerial();
    if (outQ.length) writeFiltered(outQ.splice(0));
  }
  const drainToTerminal = flushTerminal;   // back-compat name for panel STEP

  // wipe the screen and print a one-page how-to, wrapped to the terminal
  function printManual() {
    outQ.length = 0;
    crlfPending = false;
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
    para("THE MACHINE IS BARE -- nothing is loaded; the default program just " +
         "echoes what you type. Pick a PRESET up top for a period-correct " +
         "build, thread a tape into the paper-tape reader, or key one in on " +
         "the panel.", " ");
    lines.push("");
    para("THE TERMINAL -- real 1970s terminals, limits and all: a fixed 24 " +
         "lines, no scrollback (text off the top is gone), uppercase only.", " ");
    lines.push("");
    para("THE TELETYPE (ASR-33) -- a printer, not a screen: 10 chars/sec onto " +
         "a paper roll. Cheap and common, and being paper it scrolls back.", " ");
    lines.push("");
    para("DISKS / CP/M -- the cabinet below is a MITS 88-DCDD: two 8-inch " +
         "drives. Click a drive to insert a diskette and press BOOT; [?] on a " +
         "drive explains that disk. CP/M uses the full 64 KB of RAM.", " ");
    lines.push("", " HISTORY");
    para("1974 -- MITS bets the company on Intel's 8080 and makes the Jan " +
         "1975 cover of Popular Electronics: the Altair 8800, a $439 kit -- " +
         "2 MHz, 256 bytes of RAM, no keyboard or screen. Gates & Allen " +
         "wrote its BASIC.", "   ");
    lines.push(bar);
    // centre the block in the screen: pad rows above, indent columns left
    const rows = term.rows || 24;
    // on a narrow terminal the paragraphs wrap longer -- drop the blank
    // separator lines until the page fits (there's no scrollback to recover
    // what runs off the top)
    /* v8 ignore next 5 -- only runs when the manual is taller than the screen */
    while (lines.length > rows) {
      const i = lines.indexOf("");
      if (i < 0) break;
      lines.splice(i, 1);
    }
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
  termSelect.addEventListener("change", () => { markCustom(); applyProfile(termSelect.value); });

  // era-preset controls (declared early so markCustom() is safe from any handler)
  const presetSelect = document.getElementById("preset");
  const autoloadChk  = document.getElementById("autoload");
  try { autoloadChk.checked = localStorage.getItem("retro8080.autoload") === "1"; } catch {}
  const presetNote   = document.getElementById("presetNote");
  const backplaneEl  = document.getElementById("backplane");
  let   applyingPreset = false;

  // load the bitmap faces, then apply (so xterm measures the right cell size)
  Promise.all([
    document.fonts?.load('20px "VT323"'),
    document.fonts?.load('15px "Courier Prime"'),
    /* v8 ignore next -- font-load rejection is swallowed */
  ].filter(Boolean)).catch(() => {}).finally(() => applyProfile(savedTerm));

  // --- page theme (Win95 / mid-90s Mosaic web / Modern / Dark Modern) ---
  // "moderndark" is Modern's layout with data-mode="dark" bolted on, so the
  // <select> value and the stored key differ from the data-theme attribute.
  const pageTheme = document.getElementById("pageTheme");
  const root = document.documentElement;
  const THEME_VALUES = ["win", "web94", "modern", "moderndark"];
  const applyTheme = (v) => {
    if (!THEME_VALUES.includes(v)) v = "win";
    if (v === "moderndark") { root.dataset.theme = "modern"; root.dataset.mode = "dark"; }
    else { root.dataset.theme = v; delete root.dataset.mode; }
    return v;
  };
  let stored; try { stored = localStorage.getItem("retro8080.theme"); } catch {}
  let savedPageTheme = applyTheme(
    new URLSearchParams(location.search).get("theme")
    || stored || root.dataset.theme || "win");
  pageTheme.value = savedPageTheme;
  placeTermBar();   // ?theme= may have resolved to Modern after the first pass
  pageTheme.addEventListener("change", () => {
    applyTheme(pageTheme.value);
    try { localStorage.setItem("retro8080.theme", pageTheme.value); } catch {}
    placeTermBar();                // layout may have gained/lost side-by-side
    setTimeout(sizeScreen, 60);    // page width changed
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
    /* v8 ignore next -- only if every HEAD request fails / lacks Last-Modified */
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
  let frameCount = 0;
  function frame(now) {
    const dt = Math.max(0, Math.min(now - lastFrame, 100));   // clamp after a tab-away
    lastFrame = now;
    if (powered) tape.tick();     // the reader feeds while the CPU is stopped
    if (running && powered) {
      // cycles paced by real elapsed time, so speed is 2 MHz on any display —
      // except while a loader is streaming BASIC in / answering its cold-start
      // prompts, when we let the CPU sprint (ends on the first keypress)
      const boost = turbo ? 12 : 1;
      machine.runCycles(Math.round(CPU_HZ * dt / 1000) * boost);
    }
    // the cassette deck isn't on the S-100 bus -- PLAY/FF/REW keep winding in
    // real time even with the panel on STOP or the machine powered off
    machine.tickCassette?.(dt, running);
    pumpTerminal(dt);          // keep typing out buffered text even when stopped
    updatePanel();
    if ((frameCount++ & 3) === 0) { disk.poll(); cassette.poll(); }   // peripheral lamps ~15 Hz
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

  // drive the low `n` toggle switches from a value (used by the panel guide's
  // per-row "set switches" buttons)
  const setSwitchWord = (value, n) => {
    for (let b = 0; b < n; b++) {
      switchState[b] = (value >> b) & 1;
      if (switchEls[b]) switchEls[b].classList.toggle("down", !switchState[b]);
    }
    machine.setSenseSwitches?.(senseByte());
  };

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
    /* v8 ignore next -- unreachable: every bit 0..15 is in GROUPS */
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
    root.prepend(kase);   // the panel, above the static .statusline readout

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
    const powerBat = el("bat");         // up = ON like every Altair paddle (boots powered)
    powerCell.title = "power OFF / ON";
    powerCell.addEventListener("click", () => {
      powered = !powered;
      powerBat.classList.toggle("down", !powered);   // down = OFF
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
    cr.appendChild(paddle("SINGLE STEP", "", "step", step).cell);
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
    const noop = () => {};   // PROTECT / AUX: present on the real panel, inert here
    cr.appendChild(paddle("PROTECT", "UNPROTECT", "protect", noop, noop).cell);
    cr.appendChild(paddle("AUX", "", "aux1", noop).cell);
    cr.appendChild(paddle("AUX", "", "aux2", noop).cell);
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
      if (!leds[b]) continue;
      leds[b].classList.toggle("on", !!((value >> b) & 1));
      leds[b].style.opacity = "";   // exact readout, not a brightness blur
    }
  }

  // The address lamps while the CPU runs: not one address but a per-bit touch
  // count over the frame, like the real incandescent lamps integrating many
  // bus cycles. A bit driven almost every cycle (Kill the Bit's D, via four
  // LDAX D per loop) should read brighter than one a slow counter only sweeps
  // through once (H) -- see ALTAIR_REVIEW.md §3.6b. counts[] is 16 uint16s.
  function setAddrBrightness(leds, counts) {
    let peak = 0;
    for (let b = 0; b < 16; b++) if (counts[b] > peak) peak = counts[b];
    for (let b = 0; b < 16; b++) {
      const l = leds[b];
      if (!l) continue;
      const c = counts[b];
      l.classList.toggle("on", c > 0);
      // floor a touched lamp at 40% so a single hit still reads as lit, then
      // scale up toward the frame's busiest bit
      l.style.opacity = c > 0 ? String(0.4 + 0.6 * (c / peak)) : "";
    }
  }

  function updatePanel() {
    if (!powered) {
      for (const l of addrLeds) if (l) { l.classList.remove("on"); l.style.opacity = ""; }
      for (const l of dataLeds) l && l.classList.remove("on");
      for (const k in statusLeds) statusLeds[k].classList.remove("on");
      return;
    }
    const s = machine.state();
    // while a tape is reading in, the address lamps climb with the reader's
    // write pointer. While the CPU runs, the address lamps show per-bit
    // brightness -- how often the bus touched each bit this frame, exactly as
    // the real incandescent lamps integrate (that's how Kill the Bit's moving
    // bit rides A8..A15, brighter than H's slow sweep). STOP freezes them at
    // PC. The data lamps follow the last real byte on the bus.
    const loading = tape.phase === "feeding" || tape.phase === "draining";
    const runningNow = running && !s.halted;
    const dAddr = loading ? 0
                : runningNow && machine.lastAddr ? machine.lastAddr()
                : s.pc;
    if (loading) setBits(addrLeds, tape.addr, 16);
    else if (runningNow && machine.busActivityCounts) setAddrBrightness(addrLeds, machine.busActivityCounts());
    else setBits(addrLeds, s.pc, 16);
    setBits(dataLeds, loading ? tape.lastByte
                              : machine.readByte ? machine.readByte(dAddr & 0xffff) : 0, 8);
    const set = (name, on) => statusLeds[name] && statusLeds[name].classList.toggle("on", !!on);
    const active = running && !s.halted;
    // MEMR/WO/INP/OUT reflect the actual last bus access (Machine::state(),
    // wasm_machine.cpp) rather than "the CPU is running" for all four alike --
    // a real IN, not just "the reader happens to be feeding". M1 and STACK
    // still can't be told apart from a plain read/write without the CPU core
    // itself saying what an access is *for*, so they stay approximated;
    // HLDA/PROT are correctly always off (no DMA peripheral ever asserts
    // HOLD; PROTECT is a documented no-op -- panel.spec.ts). See
    // ALTAIR_REVIEW.md §3.6a.
    set("INP", s.inp);
    set("WAIT", !running || s.halted);
    set("HLTA", s.halted);
    set("INTE", s.intEnabled);
    set("MEMR", s.memr);
    set("M1", active);
    set("MI", active);          // (legacy key, harmless)
    set("WO", s.wo);
    set("PROT", false);
    set("OUT", s.out);
    set("STACK", false);
    set("INT", s.intAck);
    set("HLDA", false);
  }

  buildPanel();

  // --- front-panel bootstrap guide -----------------------
  // A collapsible "here is how you'd hand-load this from the panel" reference,
  // preset-aware, with the actual octal for the current machine and buttons
  // that key the loader in and spool the tape so it really works.
  const oct = (n, w) => n.toString(8).padStart(w, "0").replace(/(\d{3})(?=\d)/g, "$1 ");

  // one entry per bootstrap byte; first byte of each instruction carries the text
  const BOOT_ASM = [
    "LXI H,<dest>      ; store pointer", "", "",
    "LXI D,<count>     ; byte count", "", "",
    "L1: IN 020        ; 2SIO status", "",
    "RRC              ; RDRF -> carry",
    "JNC L1           ; wait for a byte", "", "",
    "IN 021           ; read it", "",
    "ORA A            ; blank leader?",
    "JZ L1            ; yes, keep spooling", "", "",
    "L2: MOV M,A       ; store the byte",
    "INX H",
    "DCX D",
    "MOV A,D", "ORA E",
    "JZ DONE          ; count exhausted", "", "",
    "L3: IN 020", "",
    "RRC",
    "JNC L3", "", "",
    "IN 021", "",
    "JMP L2", "", "",
    "DONE: JMP <start> ; run the program", "", "",
  ];

  // panel-guide checklist + floating-panel position, kept across guide rebuilds
  const pgDone = new Set();      // finished step keys: byte index, or "taddr"/"daddr"
  let pgDoneKey = null;          // model signature; a change clears the checklist
  let pgWired = false;           // the #panelGuide listeners are attached once
  const pgFloat = { left: null, top: null };
  try {
    const s = JSON.parse(localStorage.getItem("retro8080.pgpos") || "null");
    if (s && Number.isFinite(s.left)) { pgFloat.left = s.left; pgFloat.top = s.top; }
  } catch {}

  const PG_LABEL = " Load it yourself";

  function panelGuideModel() {
    const id = presetSelect ? presetSelect.value : "";
    const p = PRESETS[id];
    const ramKb = (machine.ramKb ? machine.ramKb() : 64);
    const org = ramKb * 1024 - 0x40;
    // what a hand-load would pull in: the threaded paper tape, else 8K BASIC
    const e = paperTape.entry;
    let dest = 0, count = 8192, start = 0, what = "8K BASIC (8192 bytes)";
    if (e && e.bytes && !e.basicRom) {
      dest = parseHex(e.load, 0); start = parseHex(e.start, 0);
      count = e.bytes.length; what = `${e.name} (${count} bytes)`;
    }
    const hasDisk = !!(p ? (p.devices || []).includes("disk") : true);
    const hasTape = !p || (p.devices || []).includes("papertape");
    return { org, dest, count, start, what, ramKb, disk: hasDisk, tape: hasTape,
             boot: makeBootstrap(org, dest, count, start) };
  }

  function buildPanelGuide(open) {
    const root = document.getElementById("panelGuide");
    if (!root) return;
    const wasOpen = open != null ? open
      : !!(root.querySelector(".pg-body") && !root.querySelector(".pg-body").hidden);
    const m = panelGuideModel();

    // a different machine / loader (usually: a new tape threaded) -> start the
    // checklist over. If a loader was already keyed into RAM for the old tape,
    // re-key it for the new one so "keyin -> thread -> Feed / START" works in
    // any order -- the baked-in byte count has to match the tape being fed.
    const key = [m.org, m.dest, m.start, m.count].join("/");
    if (key !== pgDoneKey) {
      const wasKeyed = pgDoneKey != null && m.boot.every((_, i) => pgDone.has(i));
      pgDone.clear();
      pgDoneKey = key;
      if (wasKeyed) {
        machine.clearMemory?.();
        for (let i = 0; i < m.boot.length; i++) { machine.writeByte?.(m.org + i, m.boot[i]); pgDone.add(i); }
        pgDone.add("taddr");
        machine.setPC?.(m.org);
      }
    }
    const isDone = (k) => pgDone.has(k);

    // a row of little toggles, grouped the way the panel is (A15 alone, then 3s;
    // data D7 D6 | D5-D3 | D2-D0)
    const swRow = (value, nbits) => {
      let s = '<span class="pg-swrow">';
      for (let b = nbits - 1; b >= 0; b--) {
        s += `<span class="pg-sw${(value >> b) & 1 ? " on" : ""}" title="${nbits > 8 ? "A" : "D"}${b}"></span>`;
        if (b > 0 && (b % 3 === 0 || (nbits > 8 && b === 15))) s += '<span class="pg-swgap"></span>';
      }
      return s + "</span>";
    };

    // the three little buttons beside a switch graphic: set the panel switches
    // to this value; set them and do the step's paddle (EXAMINE for an address,
    // DEPOSIT for a data byte); or a checkbox to tick the step off by hand.
    // `k` is the checklist key -- a byte index, or "taddr" / "daddr".
    const swDo = (act, value, bits, k) => {
      const isAddr = bits > 8;
      const done = isDone(k);
      const dis = done ? " disabled" : "";
      const t2 = act === "dep" ? "set the switches and flip DEPOSIT"
                               : "set the switches and flip EXAMINE";
      return `<span class="pg-do${done ? " done" : ""}">` +
        `<button data-act="set" data-k="${k}" data-val="${value}" data-bits="${bits}"${dis} ` +
          `title="set these ${isAddr ? "address" : "data"} switches on the panel">↕</button>` +
        `<button data-act="${act}" data-k="${k}" data-val="${value}" data-bits="${bits}"${dis} ` +
          `title="${t2}">▸</button>` +
        `<button data-act="check" data-k="${k}" class="pg-checkbtn${done ? " on" : ""}" ` +
          `title="mark this step done (you keyed it in by hand)">&check;</button>` +
        "</span>";
    };

    const rows = [];
    for (let i = 0; i < m.boot.length; i++) {
      const asm = (BOOT_ASM[i] || "")
        .replace("<dest>", hex(m.dest, 4) + "h")
        .replace("<count>", String(m.count))
        .replace("<start>", hex(m.start, 4) + "h");
      rows.push(
        `<tr class="${asm ? "op" : ""}${isDone(i) ? " done" : ""}">` +
        `<td>${swRow(m.boot[i], 8)}${swDo("dep", m.boot[i], 8, i)}</td>` +
        `<td class="oct">${oct(m.boot[i], 3)}</td>` +
        `<td class="asm">${asm.replace(/</g, "&lt;")}</td></tr>`);
    }

    const diskSection = m.disk ? `
      <h4>&mdash; boot the floppy (CP/M) &mdash;</h4>
      <ol>
        <li>flip <span class="k">STOP</span> up</li>
        <li class="${isDone("daddr") ? "done" : ""}">set the address switches:&nbsp; ${swRow(0xff00, 16)}${swDo("exam", 0xff00, 16, "daddr")} <span class="note">&nbsp;(${oct(0xff00, 6)} = 0FF00h)</span></li>
        <li>flip <span class="k">EXAMINE</span> up</li>
        <li>flip <span class="k">RUN</span> down</li>
      </ol>
      <p>The 88-DCDD's boot PROM at 0FF00h reads track&nbsp;0 and CP/M comes up
      at <b>A&gt;</b>. The cabinet's <b>BOOT</b> button does exactly this.</p>
      <div><button class="pg-feed" id="pgBoot">Boot drive A</button></div>` : "";

    const tapeSection = m.tape ? `
      <h4>&mdash; load from paper tape / cassette &mdash;</h4>
      <ol>
        <li class="${isDone("taddr") ? "done" : ""}">flip <span class="k">STOP</span> up, then set the address switches to
            the loader's start:&nbsp; ${swRow(m.org, 16)}${swDo("exam", m.org, 16, "taddr")}
            <span class="note">&nbsp;(${oct(m.org, 6)})</span> &mdash; flip <span class="k">EXAMINE</span> up</li>
        <li>key the ${m.boot.length}-byte loader in: for each row set the 8
            <b>DATA</b> switches, flip <span class="k">DEPOSIT</span> for the
            first, <span class="k">DEP&nbsp;NEXT</span> after. Per row:
            <b>↕</b> sets the switches, <b>▸</b> sets them and deposits,
            <b>&check;</b> ticks it off once you've keyed it by hand
          <div class="pg-listing"><table>
            <tr><th>DATA SWITCHES</th><th>OCT</th><th>8080</th></tr>
            ${rows.join("")}
          </table></div></li>
        <li>thread a tape and press <b>Feed&nbsp;tape</b> below (or <span class="k">PLAY</span> on the deck)</li>
        <li>flip <span class="k">RUN</span> down &mdash; the address lamps climb;
            at the end the loader jumps to <span class="note">${oct(m.start, 6)}</span> and the program runs</li>
      </ol>
      <div>
        <button class="pg-feed" id="pgKeyin">Key the loader in for me</button>
        <button class="pg-feed" id="pgFeed">Feed tape &amp; run</button>
      </div>
      <p class="note">Loading ${m.what} into ${m.ramKb} KB. The reader's
      <b>LOAD SPEED</b> paces the feed &mdash; Realistic &asymp; 14&nbsp;min for
      8K BASIC.</p>` : "";

    root.innerHTML =
      `<button class="pg-toggle" id="pgToggle">${wasOpen ? "&#9662;" : "&#9656;"}${PG_LABEL}</button>
       <div class="pg-body"${wasOpen ? "" : " hidden"}>
         <div class="pg-drag" id="pgDrag"><span>&#9635; front-panel bootstrap</span>
           <button class="pg-x" id="pgX" title="close (drag the bar to move)">&times;</button></div>
         <div class="pg-doc">
           ${diskSection}
           ${tapeSection || (m.disk ? "" : "<p>This build has no bootable device.</p>")}
         </div>
       </div>`;

    // restore a dragged-to position
    const panel = root.querySelector(".pg-body");
    if (pgFloat.left != null) {
      panel.style.left = pgFloat.left + "px";
      panel.style.top = pgFloat.top + "px";
      panel.style.right = "auto";
    }

    // Listeners are delegated on #panelGuide and attached exactly once -- the
    // innerHTML above is replaced on every rebuild but `root` itself is not.
    if (pgWired) return;
    pgWired = true;

    const setToggle = () => {
      const p = root.querySelector(".pg-body");
      root.querySelector("#pgToggle").innerHTML =
        (p.hidden ? "&#9656;" : "&#9662;") + PG_LABEL;
    };
    const markRow = (btn, done) => {   // grey a finished row, lock its ↕ / ▸
      const grp = btn.closest(".pg-do");
      grp.classList.toggle("done", done);
      btn.closest("tr, li")?.classList.toggle("done", done);
      grp.querySelectorAll('button[data-act="set"],button[data-act="dep"],button[data-act="exam"]')
        .forEach((b) => { b.disabled = done; });
      grp.querySelector('button[data-act="check"]')?.classList.toggle("on", done);
    };

    root.addEventListener("click", (e) => {
      const p = root.querySelector(".pg-body");
      if (e.target.closest("#pgToggle")) { p.hidden = !p.hidden; setToggle(); return; }
      if (e.target.closest("#pgX"))      { p.hidden = true; setToggle(); return; }
      if (e.target.closest("#pgKeyin")) {
        const mm = panelGuideModel();
        setRunning(false);
        machine.clearMemory?.();
        for (let i = 0; i < mm.boot.length; i++) { machine.writeByte?.(mm.org + i, mm.boot[i]); pgDone.add(i); }
        pgDone.add("taddr");
        machine.setPC?.(mm.org);
        buildPanelGuide(true);            // refresh -- the whole checklist is now ticked
        flashPanelGuide("loader keyed in at " + oct(mm.org, 6) + " -- thread a tape, then Feed tape & run");
        return;
      }
      if (e.target.closest("#pgFeed")) {
        machine.setPC?.(panelGuideModel().org & 0xffff);   // hand-keyed loader runs from its start
        if (paperTape.feedRaw()) { setRunning(true); flashPanelGuide("reader feeding -- watch the address lamps climb"); }
        else flashPanelGuide("thread a tape in the reader first");
        return;
      }
      if (e.target.closest("#pgBoot")) {
        if (machine.diskPresent && machine.diskPresent(0)) { disk.boot(); flashPanelGuide("booting drive A..."); }
        else flashPanelGuide("insert a diskette in drive A first");
        return;
      }
      const btn = e.target.closest("button[data-act]");
      if (!btn) return;
      const act = btn.dataset.act;
      const raw = btn.dataset.k;
      const k = /^\d+$/.test(raw) ? Number(raw) : raw;
      if (act === "check") {
        const now = !pgDone.has(k);
        if (now) pgDone.add(k); else pgDone.delete(k);
        markRow(btn, now);
        return;
      }
      setRunning(false);
      setSwitchWord(Number(btn.dataset.val) & 0xffff, Number(btn.dataset.bits) || 8);
      if (act === "exam") {
        machine.setPC?.(Number(btn.dataset.val) & 0xffff);
        flashPanelGuide("EXAMINE " + oct(Number(btn.dataset.val), 6) + " -- address set");
      } else if (act === "dep") {
        const mm = panelGuideModel();
        machine.setPC?.((mm.org + k) & 0xffff);
        machine.writeByte?.((mm.org + k) & 0xffff, mm.boot[k]);
        machine.setPC?.((mm.org + k + 1) & 0xffff);
        flashPanelGuide("DEPOSIT " + oct(mm.boot[k], 3) + " at " + oct(mm.org + k, 6));
      }
      pgDone.add(k);
      markRow(btn, true);
    });

    root.addEventListener("mousedown", pgDragStart);   // drag the title bar
  }

  // drag the floating guide by its title bar. Exercised by panelguide.spec.ts
  // "...can be dragged", but V8 coverage doesn't attribute code that runs inside
  // Playwright-synthesised mouse events, so the body is marked ignored.
  /* v8 ignore start */
  function pgDragStart(e) {
    if (!e.target.closest("#pgDrag") || e.target.closest("#pgX")) return;
    e.preventDefault();
    const p = document.querySelector("#panelGuide .pg-body");
    const r = p.getBoundingClientRect();
    const ox = e.clientX - r.left, oy = e.clientY - r.top;
    const move = (ev) => {
      pgFloat.left = Math.max(4, Math.min(window.innerWidth - 64, ev.clientX - ox));
      pgFloat.top = Math.max(4, Math.min(window.innerHeight - 36, ev.clientY - oy));
      p.style.left = pgFloat.left + "px";
      p.style.top = pgFloat.top + "px";
      p.style.right = "auto";
    };
    const up = () => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
      try { localStorage.setItem("retro8080.pgpos", JSON.stringify(pgFloat)); } catch {}
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  }
  /* v8 ignore stop */

  function flashPanelGuide(msg) {
    const b = document.querySelector("#panelGuide .pg-doc");
    if (!b) return;
    let n = b.querySelector(".pg-flash");
    if (!n) { n = el("pg-flash"); n.style.cssText = "color:#8affb9;margin-top:6px"; b.appendChild(n); }
    n.textContent = "> " + msg;
  }

  // --- MITS 88-DCDD disk drive cabinet --------------------
  // Rotation IS timed (ALTAIR_REVIEW.md §3.2d): a real 88-DCDD spins at
  // ~166 ms/revolution over 32 sectors ~= 193 sectors/sec -- "Realistic".
  // Same LOAD SPEED pattern as the paper-tape reader / cassette deck.
  const DISK_SPEEDS = { realistic: 193, x5: 965, x25: 4825, x50: 9650, max: 0 };
  const DISK_SPEED_KEYS = ["realistic", "x5", "x25", "x50"];

  const disk = {
    catalog: [],
    catalogLoaded: false,
    names: [null, null],       // label shown on each drive's diskette
    entries: [null, null],     // the manifest entry mounted in each drive (or null)
    bays: [],                  // per-drive DOM refs
    lampHold: [0, 0],
    lastTick: [0, 0],
    pickerDrive: 0,

    build() {
      const root = document.getElementById("dcdd");
      const kase = el("dcdd-case");
      const top = el("dcdd-top",
        `<span class="dcdd-plate">MITS 88-DCDD <span>&nbsp;DISK DRIVES</span></span>`);
      const spdWrap = document.createElement("span");
      spdWrap.className = "dev-speedwrap";
      spdWrap.innerHTML = '<span class="dev-speedlabel">LOAD SPEED</span>';
      const spd = document.createElement("select");
      spd.className = "dcdd-speed";
      spd.innerHTML =
        '<option value="realistic">Realistic (~166 ms/rev)</option>' +
        '<option value="x5">5&times;</option>' +
        '<option value="x25">25&times;</option>' +
        '<option value="x50">50&times;</option>';
      try { this.speed = localStorage.getItem("retro8080.diskspeed") || "realistic"; } catch {}
      if (!DISK_SPEED_KEYS.includes(this.speed)) this.speed = "realistic";   // drop a stale value
      spd.value = this.speed;
      spd.addEventListener("change", () => this.setSpeed(spd.value));
      spdWrap.appendChild(spd);
      top.appendChild(spdWrap);
      const boot = document.createElement("button");
      boot.className = "dcdd-boot";
      boot.textContent = "BOOT";
      boot.title = "load the 88-DCDD bootstrap PROM at 0FF00h and run (turnkey disk boot)";
      boot.addEventListener("click", () => this.boot());
      top.appendChild(boot);
      kase.appendChild(top);

      const bays = el("dcdd-bays");
      for (let d = 0; d < 2; d++) {
        const bay = el("dcdd-bay empty");
        const letter = el("dcdd-letter", d === 0 ? "A" : "B");
        const slot = el("dcdd-slot");
        slot.dataset.label = "";
        slot.addEventListener("click", () => { if (bay.classList.contains("empty")) this.openPicker(d); });
        const ctl = el("dcdd-ctl");
        const lamp = el("dcdd-lamp");
        const eject = document.createElement("button");
        eject.className = "dcdd-eject hidden";
        eject.textContent = "EJECT";
        eject.addEventListener("click", () => this.eject(d));
        const helpBtn = document.createElement("button");
        helpBtn.className = "dcdd-help hidden";
        helpBtn.textContent = "?";
        helpBtn.title = "how to use this disk";
        helpBtn.addEventListener("click", () => this.showHelp(d));
        const save = document.createElement("button");
        save.className = "dcdd-save hidden";
        save.textContent = "SAVE";
        save.title = "download this diskette image with your changes";
        save.addEventListener("click", () => this.save(d));
        ctl.append(lamp, helpBtn, eject, save);
        bay.append(letter, slot, ctl);
        bays.appendChild(bay);
        this.bays[d] = { bay, slot, lamp, help: helpBtn, eject, save };
      }
      kase.appendChild(bays);
      kase.appendChild(el("dcdd-hint",
        "Click a drive to insert a diskette, then BOOT. CP/M wants all 64 KB of " +
        "RAM (4&times; 88-16MCD boards)."));
      root.appendChild(kase);
      this.bootBtn = boot;
      this.speedSel = spd;
      this.setSpeed(this.speed);
    },

    setSpeed(key) {
      this.speed = DISK_SPEEDS[key] != null ? key : "realistic";
      if (this.speedSel) this.speedSel.value = this.speed;
      if (typeof machine.setDiskSpeed === "function") machine.setDiskSpeed(DISK_SPEEDS[this.speed]);
      // don't persist the internal "max" (?test / Auto-load) as a user choice
      if (DISK_SPEED_KEYS.includes(this.speed))
        try { localStorage.setItem("retro8080.diskspeed", this.speed); } catch {}
    },

    async loadCatalog() {
      if (this.catalogLoaded) return;
      this.catalogLoaded = true;
      diskList.innerHTML = "";
      let manifest;
      try {
        const r = await fetch("disks/manifest.json");
        if (!r.ok) throw new Error("HTTP " + r.status);
        manifest = await r.json();
      } catch (err) {
        const o = document.createElement("option");
        o.textContent = "(disks/manifest.json not found)";
        o.disabled = true;
        diskList.appendChild(o);
        console.warn("disk catalog:", err);
        return;
      }
      this.catalog = manifest.disks || [];
      for (let i = 0; i < this.catalog.length; i++) {
        const dsk = this.catalog[i];
        try {
          const r = await fetch("disks/" + dsk.file);
          dsk.bytes = r.ok ? new Uint8Array(await r.arrayBuffer()) : null;
        } catch { dsk.bytes = null; }
        const o = document.createElement("option");
        o.value = String(i);
        o.textContent = dsk.bytes
          ? `${dsk.name}  —  ${(dsk.bytes.length / 1024).toFixed(0)} KB`
          : `${dsk.name}  —  (unavailable)`;
        o.disabled = !dsk.bytes;
        diskList.appendChild(o);
      }
      const first = this.catalog.findIndex((d) => d.bytes);
      if (first >= 0) { diskList.value = String(first); this.syncPicker(); }
    },

    syncPicker() {
      const dsk = this.catalog[Number(diskList.value)];
      diskDesc.textContent = dsk ? (dsk.description || "") : "";
    },

    openPicker(drive) {
      this.pickerDrive = drive;
      document.getElementById("diskDrive").textContent = drive === 0 ? "A" : "B";
      diskDialog.hidden = false;
      this.loadCatalog();
    },
    closePicker() { diskDialog.hidden = true; term.focus(); },

    setVisible(v) { document.getElementById("dcdd").classList.toggle("empty", !v); },

    // A brand-new diskette: 0xE5-filled and unformatted, the way real 8" media
    // shipped. CP/M can't read it until FORMAT (the DISK command) has laid down
    // the sector framing and an empty directory from a booted drive.
    blank(drive) {
      this.insert(drive, new Uint8Array(337568).fill(0xe5), "BLANK");
    },

    // A blank diskette that has already been through FORMAT: real MITS sector
    // framing (borrowed from the bundled CP/M image) with the directory --
    // track 2, the whole track so the sector skew doesn't matter -- wiped to
    // 0xE5 and its checksum fixed up, so CP/M sees it as empty right away.
    async blankFormatted(drive) {
      await this.loadCatalog();
      const src = this.catalog.find((d) => d.os === "cpm" && d.bytes);
      if (!src) { this.blank(drive); return; }
      const img = src.bytes.slice();
      const SEC = 137;
      for (let s = 0; s < 32; s++) {
        const b = (2 * 32 + s) * SEC;               // track 2 = the CP/M directory
        for (let i = 3; i <= 130; i++) img[b + i] = 0xe5;
        img[b + 132] = (128 * 0xe5) & 0xff;          // tracks 0-5: checksum = sum of data
      }
      this.insert(drive, img, "BLANK (FORMATTED)");
    },

    insert(drive, bytes, name, entry) {
      if (!bytes || !bytes.length) return;
      // swapping a diskette is not a hardware change -- keep the era preset
      // (same as the paper-tape reader and the cassette deck)
      machine.mountDisk(drive, bytes);
      this.names[drive] = name;
      this.entries[drive] = entry || null;
      const b = this.bays[drive];
      b.bay.classList.remove("empty");
      b.bay.classList.add("loaded");
      b.slot.dataset.label = (name || "DISKETTE").toUpperCase().slice(0, 30);
      b.eject.classList.remove("hidden");
      b.help.classList.remove("hidden");
    },

    eject(drive) {
      if (machine.diskDirty && machine.diskDirty(drive) &&
          !confirm(`Drive ${drive === 0 ? "A" : "B"} has unsaved changes. Eject anyway?`))
        return;
      machine.unmountDisk(drive);
      this.names[drive] = null;
      this.entries[drive] = null;
      const b = this.bays[drive];
      b.bay.classList.remove("loaded");
      b.bay.classList.add("empty");
      b.slot.dataset.label = "";
      b.eject.classList.add("hidden");
      b.help.classList.add("hidden");
      b.save.classList.add("hidden");
    },

    showHelp(drive) {
      const entry = this.entries[drive];
      const name = this.names[drive] || "diskette";
      document.getElementById("diskHelpName").textContent = name;
      const body = document.getElementById("diskHelpBody");
      const mk = (tag, txt, cls) => {
        const e = document.createElement(tag);
        if (cls) e.className = cls;
        if (txt != null) e.textContent = txt;
        return e;
      };
      body.innerHTML = "";
      if (entry && entry.description) body.appendChild(mk("p", entry.description));
      body.appendChild(mk("h4", "THIS DISK"));
      body.appendChild(mk("pre", entry && entry.help
        ? entry.help
        : "No notes for this image. The basics below still apply once it boots."));
      body.appendChild(mk("div", null, "divider"));
      const dos = entry && entry.os === "dos";
      body.appendChild(mk("h4", dos ? "USING ALTAIR DOS" : "USING CP/M"));
      body.appendChild(mk("pre", dos ? DOS_PRIMER : CPM_PRIMER));
      diskHelpDialog.hidden = false;
    },
    closeHelp() { diskHelpDialog.hidden = true; term.focus(); },

    boot() {
      if (!machine.diskPresent || !machine.diskPresent(0)) {
        this.flashBoot("insert a diskette in drive A first");
        return;
      }
      machine.bootDisk();
      turbo = false;
      term.clear();
      outQ.length = 0;
      crlfPending = false;
      setRunning(true);
      term.focus();
    },
    flashBoot(msg) {
      const hint = document.querySelector("#dcdd .dcdd-hint");
      if (!hint) return;
      const prev = hint.textContent;
      hint.textContent = "— " + msg + " —";
      hint.style.color = "#ff8a6a";
      /* v8 ignore next -- flash text restores itself after 1.6 s */
      setTimeout(() => { hint.textContent = prev; hint.style.color = ""; }, 1600);
    },

    save(drive) {
      const bytes = machine.diskImage(drive);
      const blob = new Blob([bytes], { type: "application/octet-stream" });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = (this.names[drive] || `drive-${drive === 0 ? "A" : "B"}`).replace(/\.dsk$/i, "") + ".dsk";
      a.click();
      URL.revokeObjectURL(a.href);
      machine.clearDiskDirty(drive);
      this.bays[drive].save.classList.add("hidden");
    },

    poll() {
      if (typeof machine.diskStatus !== "function") return;
      const st = machine.diskStatus();
      for (let d = 0; d < 2; d++) {
        const b = this.bays[d];
        if (!b) continue;
        // io/step are shared counters -- only the selected drive is the one
        // actually moving, so don't flash the idle drive's lamp along with it
        const tick = (d === 0 ? st.track0 : st.track1) + st.io + st.step;
        const moved = tick !== this.lastTick[d];
        this.lastTick[d] = tick;
        if (moved && st.selected === d) this.lampHold[d] = 6;
        const busy = this.lampHold[d] > 0;
        if (busy) this.lampHold[d]--;
        b.lamp.classList.toggle("busy", busy);
        b.lamp.classList.toggle("sel", !busy && st.selected === d && st.headLoaded);
        if (machine.diskDirty && machine.diskPresent && machine.diskPresent(d))
          b.save.classList.toggle("hidden", !machine.diskDirty(d));
      }
    },
  };

  const diskDialog     = document.getElementById("diskDialog");
  const diskList       = document.getElementById("diskList");
  const diskDesc       = document.getElementById("diskDesc");
  const diskFile       = document.getElementById("diskFile");
  const diskFileName   = document.getElementById("diskFileName");
  const diskHelpDialog = document.getElementById("diskHelpDialog");

  disk.build();

  document.getElementById("diskHelpClose").addEventListener("click", () => disk.closeHelp());
  document.getElementById("diskHelpOk").addEventListener("click", () => disk.closeHelp());
  diskHelpDialog.addEventListener("click", (e) => { if (e.target === diskHelpDialog) disk.closeHelp(); });

  diskList.addEventListener("change", () => disk.syncPicker());
  diskFile.addEventListener("change", () => {
    diskFileName.textContent = diskFile.files[0] ? diskFile.files[0].name : "no file selected";
  });
  document.getElementById("diskCancel").addEventListener("click", () => disk.closePicker());
  document.getElementById("diskBlank").addEventListener("click", () => {
    const drive = disk.pickerDrive;
    disk.closePicker();
    disk.blank(drive);
  });
  document.getElementById("diskBlankFmt").addEventListener("click", () => {
    const drive = disk.pickerDrive;
    disk.closePicker();
    disk.blankFormatted(drive);
  });
  document.getElementById("diskClose").addEventListener("click", () => disk.closePicker());
  diskDialog.addEventListener("click", (e) => { if (e.target === diskDialog) disk.closePicker(); });
  document.getElementById("diskInsert").addEventListener("click", async () => {
    const drive = disk.pickerDrive;
    const file = diskFile.files[0];
    let bytes, name, entry = null;
    if (file) {
      bytes = new Uint8Array(await file.arrayBuffer());
      name = file.name;
    } else {
      const dsk = disk.catalog[Number(diskList.value)];
      if (!dsk || !dsk.bytes) { diskDesc.textContent = "That diskette is unavailable."; return; }
      bytes = dsk.bytes;
      name = dsk.name;
      entry = dsk;
    }
    if (bytes.length !== 337568)
      console.warn(`disk image is ${bytes.length} bytes; MITS 8" is 337568`);
    diskFile.value = "";
    diskFileName.textContent = "no file selected";
    disk.closePicker();
    disk.insert(drive, bytes, name, entry);
  });

  // --- MITS 88-ACR cassette deck --------------------------
  // bytes/sec. Realistic is the real 300 baud; `max` (0 = unlimited) isn't in
  // the picker -- it's what ?test and preset Auto-load use.
  const TAPE_SPEEDS = { realistic: 30, x5: 150, x25: 750, x50: 1500, max: 0 };
  const TAPE_SPEED_KEYS = ["realistic", "x5", "x25", "x50"];

  const cassette = {
    name: null,
    catalog: [],
    catalogLoaded: false,
    speed: "realistic",
    counterZero: 0,        // byte pos the counter was last zeroed at

    build() {
      const root = document.getElementById("acr");
      const kase = el("acr-case");
      const top = el("acr-top",
        `<span class="acr-plate">RADIO SHACK <span>&nbsp;CASSETTE RECORDER</span></span>`);
      const spdWrap = document.createElement("span");
      spdWrap.className = "dev-speedwrap";
      spdWrap.innerHTML = '<span class="dev-speedlabel">TAPE SPEED</span>';
      const spd = document.createElement("select");
      spd.className = "acr-speed";
      spd.innerHTML =
        '<option value="realistic">Realistic (300 baud)</option>' +
        '<option value="x5">5&times;</option>' +
        '<option value="x25">25&times;</option>' +
        '<option value="x50">50&times;</option>';
      try { this.speed = localStorage.getItem("retro8080.tapespeed") || "realistic"; } catch {}
      if (!TAPE_SPEED_KEYS.includes(this.speed)) this.speed = "realistic";   // drop a stale value
      spd.value = this.speed;
      spd.addEventListener("change", () => this.setSpeed(spd.value));
      spdWrap.appendChild(spd);
      top.appendChild(spdWrap);
      kase.appendChild(top);

      const body = el("acr-body");
      const win = el("acr-window",
        `<span class="acr-shell">
           <span class="acr-hub h1"><span class="acr-reel"></span></span>
           <span class="acr-hub h2"><span class="acr-reel"></span></span>
           <span class="acr-shell-label"></span>
         </span>
         <span class="acr-tapeprog"></span>`);
      win.addEventListener("click", () => this.openPicker());

      const ctl = el("acr-ctl");
      const meter = el("acr-meter");
      const lamp = el("acr-lamp");
      const counter = document.createElement("button");
      counter.className = "acr-counter";
      counter.type = "button";
      counter.textContent = "000";
      counter.title = "tape counter — click to zero it";
      counter.addEventListener("click", (e) => {
        e.stopPropagation();
        this.counterZero = this.pos();
        this.poll();
      });
      meter.append(lamp, counter);

      const transport = el("acr-transport");
      const key = (cls, glyph, label, fn) => {
        const b = document.createElement("button");
        b.className = "acr-key " + cls;
        b.type = "button";
        b.innerHTML = `<span class="ico">${glyph}</span>${label}`;
        b.addEventListener("click", (e) => { e.stopPropagation(); fn(); });
        return b;
      };
      this.kRew  = key("rew",  "◀◀", "REW",   () => this.transport("rew"));
      this.kPlay = key("play", "▶",       "PLAY",  () => this.transport("play"));
      this.kFF   = key("ff",   "▶▶", "F.FWD", () => this.transport("ff"));
      this.kStop = key("stop", "■",       "STOP",  () => this.transport("stop"));
      this.kRec  = key("rec",  "●",       "REC",   () => this.transport("rec"));
      this.kEj   = key("ej",   "⏏",       "EJECT", () => this.transport("eject"));
      transport.append(this.kRew, this.kPlay, this.kFF, this.kStop, this.kRec, this.kEj);

      const save = document.createElement("button");
      save.className = "acr-save hidden";
      save.type = "button";
      save.textContent = "SAVE .CAS";
      save.addEventListener("click", (e) => { e.stopPropagation(); this.save(); });

      ctl.append(meter, transport, save);
      body.append(win, ctl);
      kase.appendChild(body);
      this.hintEl = el("acr-hint");
      kase.appendChild(this.hintEl);
      root.appendChild(kase);
      this.win = win; this.lamp = lamp; this.counter = counter; this.speedSel = spd;
      this.prog = win.querySelector(".acr-tapeprog");
      this.shellLabel = win.querySelector(".acr-shell-label");
      this.saveBtn = save;
      this.updateHint();
      this.setSpeed(this.speed);
      this.syncKeys();
    },

    pos() { return (machine.tapeStatus ? machine.tapeStatus().pos : 0) | 0; },

    // fixed instructions on the deck: how to load, how to save, use the counter.
    // Line 1 shows the real CLOAD name of whatever tape is in.
    updateHint() {
      if (!this.hintEl) return;
      const name = this.cload || "X";
      this.hintEl.innerHTML =
        `<span><b>LOAD</b>&nbsp; <b>&#9664;&#9664; REW</b>, then type <tt>CLOAD "${name}"</tt> ` +
          `in BASIC and press <b>&#9654; PLAY</b></span>` +
        `<span><b>SAVE</b>&nbsp; press <b>&#9679; REC</b>, then <tt>CSAVE "X"</tt> in BASIC ` +
          `<i>&mdash; a <b>SAVE .CAS</b> button appears to download it</i></span>` +
        `<span><i>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;several programs fit on one tape &mdash; ` +
          `<b>&#9654;&#9654; F.FWD</b> past one (watch the counter) to record the next.</i></span>`;
    },

    // the transport keys, like the recorder the 88-ACR plugged into. CLOAD only
    // pulls bytes while PLAY is down; CSAVE only records while REC is down --
    // so you type the command, then press the key, exactly as you did in 1976.
    transport(action) {
      if (action === "eject") { this.eject(); return; }
      // no status text -- the keys and the head bar show what the tape is doing
      this.setDeck(action);       // "play" | "rec" | "ff" | "rew" | "stop"
      this.poll();
    },

    // Drive the transport. C++ owns the state from here: it moves the head and
    // auto-stops at the tape ends, and poll() renders the keys from what it
    // reports back. mode: "stop" | "play" | "rec" | "ff" | "rew".
    setDeck(mode) {
      const play = mode === "play" || mode === "rec";
      machine.setTapeMotor?.(play);
      machine.setTapeRecordArm?.(mode === "rec");
      machine.setTapeWind?.(mode === "ff" ? 1 : mode === "rew" ? -1 : 0);
      this.syncKeys();
    },

    syncKeys() {
      const loaded = machine.tapeLoaded ? machine.tapeLoaded() : false;
      const t = machine.tapeStatus ? machine.tapeStatus().transport | 0 : 0;
      const set = (k, on) => k && k.classList.toggle("down", !!on);
      set(this.kPlay, t === 1 || t === 2);
      set(this.kRec,  t === 2);
      set(this.kFF,   t === 3);
      set(this.kRew,  t === 4);
      for (const k of [this.kRew, this.kPlay, this.kFF, this.kStop, this.kRec])
        if (k) k.disabled = !loaded;
    },

    setSpeed(key) {
      this.speed = TAPE_SPEEDS[key] != null ? key : "realistic";
      if (this.speedSel) this.speedSel.value = this.speed;
      if (typeof machine.setTapeSpeed === "function")
        machine.setTapeSpeed(TAPE_SPEEDS[this.speed]);
      // don't persist the internal "max" (?test / Auto-load) as a user choice
      if (TAPE_SPEED_KEYS.includes(this.speed))
        try { localStorage.setItem("retro8080.tapespeed", this.speed); } catch {}
    },

    setVisible(v) { document.getElementById("acr").classList.toggle("empty", !v); },

    async loadCatalog() {
      if (this.catalogLoaded) return;
      this.catalogLoaded = true;
      tapeList.innerHTML = "";
      try {
        const r = await fetch("tapes/manifest.json");
        if (!r.ok) throw 0;
        const m = await r.json();
        this.catalog = m.tapes || [];
        for (let i = 0; i < this.catalog.length; i++) {
          const t = this.catalog[i];
          try { const rr = await fetch("tapes/" + t.file); t.bytes = rr.ok ? new Uint8Array(await rr.arrayBuffer()) : null; }
          catch { t.bytes = null; }
          const o = document.createElement("option");
          o.value = String(i);
          o.textContent = t.bytes ? `${t.name}  —  ${t.bytes.length} bytes` : `${t.name}  —  (unavailable)`;
          o.disabled = !t.bytes;
          tapeList.appendChild(o);
        }
      } catch {
        const o = document.createElement("option");
        o.textContent = "(no cassette library — use Choose File)";
        o.disabled = true;
        tapeList.appendChild(o);
      }
    },

    openPicker() { tapeDialog.hidden = false; this.loadCatalog(); },
    closePicker() { tapeDialog.hidden = true; term.focus(); },

    insert(bytes, name, cload) {
      machine.mountTape(bytes || new Uint8Array(0));
      this.setDeck("stop");                 // threaded, transport stopped -- press PLAY
      this.name = name || "TAPE";
      this.cload = cload || null;
      this.counterZero = 0;
      const acr = document.getElementById("acr");
      acr.classList.remove("empty");
      acr.classList.add("loaded");
      this.shellLabel.dataset.label = this.name.toUpperCase().slice(0, 20);
      this.updateHint();
      this.poll();
    },
    // a blank tape you can record onto right away
    blank() { this.insert(new Uint8Array(0), "BLANK"); },

    eject(force) {
      if (!force && machine.tapeDirty && machine.tapeDirty() &&
          !confirm("This tape has an unsaved recording. Eject anyway?")) return;
      machine.ejectTape();
      this.setDeck("stop");
      this.name = null;
      this.cload = null;
      this.counterZero = 0;
      const acr = document.getElementById("acr");
      acr.classList.remove("loaded", "spin", "playing", "rewinding", "recording", "winding");
      this.shellLabel.dataset.label = "";
      this.updateHint();
      this.saveBtn.classList.add("hidden");
      this.syncKeys();
    },

    async save() {
      const bytes = machine.tapeImage();
      if (!bytes || !bytes.length) return;   // SAVE .CAS only shows with a recording
      const base = this.name && this.name !== "BLANK" ? this.name : "recording";
      const suggested = base.replace(/\.(cas|bin)$/i, "").replace(/[^\w.-]+/g, "_") + ".cas";
      const blob = new Blob([bytes], { type: "application/octet-stream" });
      // real "Save As" dialog where the browser supports it (Chrome/Edge)
      if (window.showSaveFilePicker) {
        try {
          const handle = await window.showSaveFilePicker({
            suggestedName: suggested,
            types: [{ description: "Cassette tape image", accept: { "application/octet-stream": [".cas"] } }],
          });
          const w = await handle.createWritable();
          await w.write(blob);
          await w.close();
          machine.clearTapeDirty();     // the SAVE .CAS button clears itself
          return;
        } catch (e) {
          if (e && e.name === "AbortError") return;       // user cancelled the dialog
          // any other failure -> fall through to the download link
        }
      }
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = suggested;
      a.click();
      URL.revokeObjectURL(a.href);
      machine.clearTapeDirty();
    },

    poll() {
      if (typeof machine.tapeStatus !== "function") return;
      const st = machine.tapeStatus();
      const t = st.transport | 0;              // 0 stop 1 play 2 rec 3 FF 4 REW
      // C++ moves the head and auto-stops at a tape end; the keys pop up below
      const rec = t === 2;
      this.lamp.classList.toggle("rec",  rec);
      this.lamp.classList.toggle("play", t === 1);

      // the reels turn while the tape is moving (a key is down)
      const acr = document.getElementById("acr");
      acr.classList.toggle("spin",      t === 1 || t === 2);
      acr.classList.toggle("playing",   t === 1 || t === 2);
      acr.classList.toggle("recording", rec);
      acr.classList.toggle("winding",   t === 3);
      acr.classList.toggle("rewinding", t === 4);

      // 3-digit mechanical counter: rolls over 999->000, click to zero
      const n = Math.round((st.pos - this.counterZero) / 16);
      this.counter.textContent = String(((n % 1000) + 1000) % 1000).padStart(3, "0");
      // fill bar: how far the head is along the physical tape
      if (this.prog)
        this.prog.style.width = Math.min(100, (100 * st.pos) / Math.max(1, st.cap)) + "%";

      // SAVE appears once you've recorded onto the tape (CSAVE) -- unsaved work
      const dirty = !!(machine.tapeDirty && machine.tapeDirty());
      this.saveBtn.classList.toggle("hidden", !dirty);
      this.saveBtn.classList.toggle("dirty", dirty);
      this.syncKeys();
    },
  };

  const tapeDialog   = document.getElementById("tapeDialog");
  const tapeList     = document.getElementById("tapeList");
  const tapeDesc     = document.getElementById("tapeDesc");
  const tapeFile     = document.getElementById("tapeFile");
  const tapeFileName = document.getElementById("tapeFileName");

  cassette.build();
  cassette.setVisible(false);

  tapeList.addEventListener("change", () => {
    const t = cassette.catalog[Number(tapeList.value)];
    tapeDesc.textContent = t ? (t.description || "") : "";
  });
  tapeFile.addEventListener("change", () => {
    tapeFileName.textContent = tapeFile.files[0] ? tapeFile.files[0].name : "no file selected";
  });
  document.getElementById("tapeCancel").addEventListener("click", () => cassette.closePicker());
  document.getElementById("tapeClose").addEventListener("click", () => cassette.closePicker());
  document.getElementById("tapeBlank").addEventListener("click", () => {
    cassette.closePicker();
    cassette.blank();
  });
  tapeDialog.addEventListener("click", (e) => { if (e.target === tapeDialog) cassette.closePicker(); });
  document.getElementById("tapeInsert").addEventListener("click", async () => {
    const file = tapeFile.files[0];
    let bytes, name, cload;
    if (file) { bytes = new Uint8Array(await file.arrayBuffer()); name = file.name; }
    else {
      const t = cassette.catalog[Number(tapeList.value)];
      if (!t || !t.bytes) { bytes = new Uint8Array(0); name = "BLANK"; }
      else { bytes = t.bytes; name = t.name; cload = t.cload; }
    }
    tapeFile.value = "";
    tapeFileName.textContent = "no file selected";
    cassette.closePicker();
    cassette.insert(bytes, name, cload);
  });

  // --- era presets ---------------------------------------
  // Each preset is a period-correct machine build: RAM, primary I/O, the S-100
  // cards plugged in, and (optionally) the flagship software auto-loaded.
  //
  // Every preset's console is the 88-2SIO (the only serial board this emulator
  // implements). A genuinely period "1975" machine would have shipped the
  // earlier 88-SIO at ports 0x00/0x01 instead -- not modelled here, and the
  // BASIC images this project bundles target the 2SIO anyway. So `baremetal`
  // and `stock` are dated 1976 (when the 2SIO existed), not the earlier dates
  // their software's real MITS release would suggest. See ALTAIR_REVIEW.md
  // §5.2 for the fuller options (implement 88-SIO vs. relabel); this is the
  // deliberate, documented "relabel" choice.
  const PRESETS = {
    baremetal: {
      name: "Bare-Metal Toggle", era: "1976",
      ramKb: 4, term: "tty33", focus: "panel",
      cards: ["MITS 88-CPU\n8080 / 2 MHz", "MITS 88-4MCS\nStatic RAM", "MITS 88-2SIO\nserial"],
      devices: ["papertape"],
      preload: { papertape: { match: /kill the bit/i } },
      autoload: { device: "papertape" },
      missing: "Kill the Bit isn't built — run `make roms`",
      blurb: "A minimal toggle-in machine: an 8080, 4K of static RAM, a serial card for the paper-tape reader, and the front panel where the real action is.",
      guide:
`KILL THE BIT is threaded in the paper-tape reader. Auto-load feeds it
in fast; press LOAD to watch the 24-byte tape crawl through the head at
the LOAD SPEED you pick. Then a lit bit sweeps across the top address
lamps (A8 - A15) -- flip the sense switch under it to knock it out.
Clear the row and you win.

  STOP / RUN        halt and restart the CPU
  RESET / CLR       (panel paddle) jump back to the start
  SINGLE STEP       one instruction at a time, while stopped

Thread another tape (click the reader) to load something else -- this
is how a program got into the machine before disks.`,
    },
    stock: {
      name: "Stock Launch", era: "1976",
      ramKb: 4, term: "tty33",
      cards: ["MITS 88-CPU\n8080", "MITS 88-4MCD\nDRAM", "MITS 88-2SIO\nserial", "Teletype\nASR-33"],
      devices: ["papertape"],
      preload: { papertape: { match: /4K BASIC/i } },
      autoload: { device: "papertape" },
      missing: "Altair 4K BASIC could not be loaded",
      blurb: "4K of RAM, a Teletype for a console, and the program that made the Altair worth buying: Altair 4K BASIC.",
      guide:
`Altair 4K BASIC is on a tape in the paper-tape reader. Auto-load feeds
it in fast; press LOAD yourself and it streams at the LOAD SPEED you
pick -- Realistic is 10 B/s, an ASR-33 reader (blank leader first, then
~6 min for 4K BASIC, address lamps climbing). Its questions (MEMORY
SIZE?, TERMINAL WIDTH?, SIN?) are answered for you -- you land at OK
with ~716 bytes free, like a real 4K box.

  PRINT 2+2
  10 PRINT "HELLO"
  20 GOTO 10
  RUN     (Ctrl-C stops it)     LIST     NEW

The Teletype prints at 10 characters a second onto a paper roll, so it
scrolls back -- and it's uppercase only.`,
    },
    cassette: {
      name: "Cassette Hobbyist", era: "1976",
      ramKb: 32, term: "adm3a",
      cards: ["MITS 88-CPU\n8080", "MITS 88-16MCD\n16K DRAM", "MITS 88-16MCD\n16K DRAM", "MITS 88-2SIO\nserial", "MITS 88-ACR\ncassette"],
      devices: ["papertape", "cassette"],
      preload: {
        papertape: { match: /8K BASIC/i },
        cassette:  { tape: "startrek.cas", name: "SUPER STAR TREK", cload: "S" },
      },
      autoload: { device: "papertape", then: "cassetteHint" },
      missing: "Altair 8K BASIC could not be loaded",
      blurb: "32K of RAM, a fast video terminal, an 8K BASIC tape in the reader, and an 88-ACR deck holding Star Trek.",
      guide:
`Two loaders: an 8K BASIC tape in the paper-tape reader, and the SUPER
STAR TREK cassette in the 88-ACR deck. Auto-load feeds 8K BASIC in fast
and cold-starts it. Press LOAD on the reader instead and it streams at
the selected LOAD SPEED -- "Realistic" is 10 B/s (an ASR-33 reader): a
genuine ~14-minute read, blank leader first, then the address lamps
climbing, exactly like a MITS tape. The cassette stays manual:

  1. at OK, type   CLOAD "S"      (BASIC now waits for the tape)
  2. press PLAY on the deck       (the reels turn, the tape streams in)
  3. at OK, type   RUN            (press STOP on the deck)

Hunt the Klingon fleet across an 8x8 galaxy; at COMMAND? type NAV, SRS,
LRS, PHA, TOR, SHE, DAM, COM, or XXX. Ctrl-C stops it.

THE CASSETTE also stores your own work:  CSAVE "NAME"  records the
current program;  CLOAD "NAME"  loads it back -- press REW, then PLAY.
The LOAD SPEED selector paces the transfer ("Realistic" = 300 baud).`,
    },
    cpm: {
      name: "CP/M Workstation", era: "1979",
      ramKb: 64, term: "vt100g",
      cards: ["MITS 88-CPU\n8080", "MITS 88-16MCD\n16K DRAM", "MITS 88-16MCD\n16K DRAM",
              "MITS 88-16MCD\n16K DRAM", "MITS 88-16MCD\n16K DRAM", "MITS 88-2SIO\nserial", "MITS 88-DCDD\ndisk ctlr"],
      devices: ["disk"],
      preload: { disk: { match: /CP\/M/i, drive: 0 } },
      autoload: { device: "disk" },
      missing: "the CP/M diskette could not be loaded",
      blurb: "64K of RAM, a VT100, and two 8-inch floppy drives -- a real disk operating system, the setup that ran a small business.",
      guide:
`CP/M 2.2 has booted to the A> prompt. Type a command:

  DIR             list files          TYPE READ.ME   show a text file
  STAT            free space          ERA JUNK.TXT   delete a file
  REN NEW=OLD     rename              B:             switch drives
  MBASIC / WS / DDT / ...   run a program (leave off the .COM)

Ctrl-C at A> re-reads the disk; Ctrl-C usually aborts a running
program. The disk cabinet's [?] button explains whatever disk is in
each drive, including how to quit each program.`,
    },
  };

  const CUSTOM_BLURB =
    "A custom build. Add or remove load devices (paper tape / cassette / floppy) " +
    "with the checkboxes up top, tune the terminal, and load software through " +
    "whichever device you keep. Pick a named preset to jump to a period-correct setup.";

  const EMU_PRIMER =
`THE FRONT PANEL is live. Address and data lamps, the A0 - A15 toggle
switches, and the paddles: STOP/RUN, SINGLE STEP, EXAMINE (set the
address), DEPOSIT (write the switches to memory), RESET/CLR. The top
eight switches (A8 - A15) are the "sense switches" that programs read.

THE TERMINAL dropdown swaps in a real 1970s terminal -- a fixed 24
lines, no scrollback, uppercase only, at a real baud rate. The
Teletype crawls at 10 characters a second and throttles the 8080,
exactly as it did in 1975.`;

  function buildBackplane(cards) {
    backplaneEl.innerHTML = "";
    if (!cards || !cards.length) { backplaneEl.classList.add("empty"); return; }
    backplaneEl.classList.remove("empty");
    const rail = el("bp-rail");
    for (const c of cards) {
      const parts = String(c).split("\n");
      rail.appendChild(el("bp-card", `<span><b>${parts[0]}</b>${parts[1] ? "<br>" + parts[1] : ""}</span>`));
    }
    backplaneEl.appendChild(rail);
    backplaneEl.appendChild(el("bp-hint", "S-100 bus — the cards this build has plugged in"));
  }

  function markCustom() {
    if (!applyingPreset && presetSelect.value) {
      presetSelect.value = "";
      try { localStorage.removeItem("retro8080.preset"); } catch {}
    }
  }

  // a standing note under the PRESET row — only for failures (media 404, out of
  // RAM). Cleared at the top of applyPreset() so it never outlives its cause.
  function presetHint(msg, opts) {
    presetNote.textContent = msg || "";
    presetNote.style.color = (opts && opts.color) || "";
  }

  // auto-load couldn't reach its software: say so on the terminal and as a
  // standing note, then leave a bare working machine
  function presetSoftwareMissing(sw) {
    presetHint(sw.missing, { persist: true });
    machine.reset();
    term.clear(); outQ.length = 0; crlfPending = false;
    setRunning(true);
    term.write(
      "\r\n\x1b[0m  — " + sw.missing.toUpperCase() + " —\r\n\r\n" +
      "  The hardware is set up; the software just didn't load. Try the\r\n" +
      "  preset again, or load something from the reader / deck / drive.\r\n\r\n");
  }

  // --- loader devices (paper tape / cassette / floppy) ----
  const ALL_DEVICES = ["papertape", "cassette", "disk"];
  const deviceChips = document.getElementById("deviceChips");

  function applyDevices(list) {
    const on = new Set(list || []);
    paperTape.setVisible(on.has("papertape"));
    cassette.setVisible(on.has("cassette"));
    disk.setVisible(on.has("disk"));
    // a fitted 88-DCDD always has its boot PROM at 0FF00h, so EXAMINE / RUN works
    if (on.has("disk")) machine.mapDiskBoot?.();
  }

  function customDevices() {
    try {
      const s = JSON.parse(localStorage.getItem("retro8080.devices") || "null");
      if (Array.isArray(s)) return s.filter((d) => ALL_DEVICES.includes(d));
    } catch {}
    return ALL_DEVICES.slice();          // a custom build starts with all three
  }

  function showDeviceChips(on) {
    deviceChips.hidden = !on;
    if (!on) return;
    const active = new Set(customDevices());
    deviceChips.querySelectorAll("input[data-dev]").forEach((cb) => {
      cb.checked = active.has(cb.dataset.dev);
    });
  }

  deviceChips.querySelectorAll("input[data-dev]").forEach((cb) => {
    cb.addEventListener("change", () => {
      const list = [...deviceChips.querySelectorAll("input[data-dev]:checked")].map((x) => x.dataset.dev);
      try { localStorage.setItem("retro8080.devices", JSON.stringify(list)); } catch {}
      applyDevices(list);
    });
  });

  // thread/insert a preset's media into its devices (nothing is loaded yet)
  async function applyPreloads(p) {
    const pl = p.preload || {};
    let missing = null;
    if (pl.papertape) {
      await loadCatalog();
      const e = catalog.find((r) => r.bytes && pl.papertape.match.test(r.name));
      if (e) paperTape.thread(e); else { paperTape.eject(); missing = p.missing; }
    } else paperTape.eject();
    if (pl.cassette) {
      const tr = await fetch("tapes/" + pl.cassette.tape).catch(() => null);
      const b = tr && tr.ok ? new Uint8Array(await tr.arrayBuffer()) : null;
      if (b) cassette.insert(b, pl.cassette.name, pl.cassette.cload); else cassette.eject(true);
    } else cassette.eject(true);
    if (pl.disk) {
      await disk.loadCatalog();
      const d = disk.catalog.find((x) => x.bytes && pl.disk.match.test(x.name));
      if (d) disk.insert(pl.disk.drive || 0, d.bytes, d.name, d);
      else missing = missing || p.missing;
    }
    return missing;
  }

  async function applyPreset(id) {
    const p = PRESETS[id];
    applyingPreset = true;
    presetHint("");                       // drop any stale failure note
    try {
      if (!p) {
        buildBackplane([]);
        applyDevices(customDevices());
        showDeviceChips(true);
        try { localStorage.removeItem("retro8080.preset"); } catch {}
        return;
      }
      showDeviceChips(false);
      machine.setRam(p.ramKb);
      termSelect.value = p.term;
      applyProfile(p.term);
      buildBackplane(p.cards);
      applyDevices(p.devices);
      try { localStorage.setItem("retro8080.preset", id); } catch {}

      // bare, running machine, then thread the preset's media
      machine.reset(); term.clear(); outQ.length = 0; crlfPending = false;
      if ((p.devices || []).includes("disk")) machine.mapDiskBoot?.();   // reset wiped it
      setRunning(true);
      const missing = await applyPreloads(p);

      if (autoloadChk.checked && p.autoload && !missing) {
        await runPresetSoftware(p);
      } else if (missing) {
        presetHint(missing, { persist: true });
      }
      const anchor = p.focus === "panel" ? "altair" : "screen";
      document.getElementById(anchor).scrollIntoView({ block: "center", behavior: "smooth" });
    } finally {
      applyingPreset = false;
      buildPanelGuide();          // refresh the octal for the new machine
    }
  }

  // ticking Auto-load, or picking a preset with it ticked: load the preset's
  // primary device now (media is already threaded by applyPreloads)
  async function runPresetSoftware(p) {
    const a = p.autoload;
    if (!a) return;
    if (a.device === "disk") {
      if (!machine.diskPresent || !machine.diskPresent(0)) { presetSoftwareMissing({ missing: p.missing }); return; }
      machine.bootDisk();
      turbo = false;
      term.clear(); outQ.length = 0; crlfPending = false;
      setRunning(true); term.focus();
      return;
    }
    if (a.device === "papertape") {
      if (!paperTape.entry) { presetSoftwareMissing({ missing: p.missing }); return; }
      paperTape.load({
        speed: "max",           // auto-load is the fast path; press LOAD yourself to watch it read
        then: a.then === "cassetteHint" ? cassetteTapeHint : undefined,
      });
      return;
    }
  }

  presetSelect.addEventListener("change", () => applyPreset(presetSelect.value));

  // Auto-load is a one-way switch: ticking it starts the current preset's
  // software right now; unticking it just remembers the choice and leaves the
  // machine alone. It does NOT re-apply the hardware, so it can't scroll the
  // page or disturb a running program.
  autoloadChk.addEventListener("change", async () => {
    try { localStorage.setItem("retro8080.autoload", autoloadChk.checked ? "1" : "0"); } catch {}
    const p = PRESETS[presetSelect.value];
    if (!autoloadChk.checked || !p || applyingPreset) return;
    applyingPreset = true;
    try { await runPresetSoftware(p); }
    finally { applyingPreset = false; }
  });

  // the "Guide" button: what this setup is, and how to drive it
  const guideDialog = document.getElementById("presetGuideDialog");
  function showPresetGuide() {
    const p = PRESETS[presetSelect.value];
    document.getElementById("presetGuideName").textContent = p ? p.name : "Custom setup";
    const body = document.getElementById("presetGuideBody");
    const mk = (tag, txt, cls) => {
      const e = document.createElement(tag);
      if (cls) e.className = cls;
      if (txt != null) e.textContent = txt;
      return e;
    };
    body.innerHTML = "";
    body.appendChild(mk("p", p ? `${p.blurb}  (${p.era}, ${p.ramKb} KB)` : CUSTOM_BLURB));
    if (p) {
      body.appendChild(mk("h4", "HOW TO USE IT"));
      body.appendChild(mk("pre", p.guide));
    }
    body.appendChild(mk("div", null, "divider"));
    body.appendChild(mk("h4", "USING THE EMULATOR"));
    body.appendChild(mk("pre", EMU_PRIMER));
    guideDialog.hidden = false;
  }
  const closeGuide = () => { guideDialog.hidden = true; term.focus(); };
  document.getElementById("presetGuide").addEventListener("click", showPresetGuide);
  document.getElementById("presetGuideClose").addEventListener("click", closeGuide);
  document.getElementById("presetGuideOk").addEventListener("click", closeGuide);
  guideDialog.addEventListener("click", (e) => { if (e.target === guideDialog) closeGuide(); });

  // --- paper-tape reader -----------------------------------------
  // A binary loads the way a MITS paper tape really did: a short serial
  // bootstrap keyed into RAM (we do it for you), running while the reader
  // trickles the tape through the 88-2SIO byte by byte -- blank LEADER first
  // (the tape physically spooling before any data), then the image, the
  // address lamps climbing with the loader's write pointer. LOAD SPEED at
  // "Realistic" = 10 B/s, an ASR-33 Teletype reader (8K BASIC then takes ~14
  // minutes, exactly as it did); 5x / 10x / 20x for the impatient.
  // A 0xE000 ROM build can't stream (it sits above RAM) so that one drops in.
  // bytes/sec. Realistic is the real ASR-33 reader; `max` (0 = unlimited) isn't
  // in the picker -- it's what ?test and preset Auto-load use.
  const PAPER_SPEEDS = { realistic: 10, x5: 50, x25: 250, x50: 500, max: 0 };
  const PAPER_SPEED_KEYS = ["realistic", "x5", "x25", "x50"];
  const TAPE_LEADER = 128;                                  // blank frames before the data
  const PROMPTS_4K = [[/MEMORY SIZE/i, "\r"], [/TERMINAL WIDTH/i, "\r"], [/\bSIN\?/i, "Y\r"]];
  const PROMPTS_8K = [[/MEMORY SIZE\?/i, "\r"], [/TERMINAL WIDTH\?/i, "\r"], [/SIN-COS-TAN-ATN\?/i, "Y\r"]];

  // serial bootstrap at `org`: skip leader nulls, then store `count` bytes at
  // `dest`, JMP `start`. ~40 bytes. Assumes the image's first byte is non-zero
  // (true for every MITS load-and-go image -- they start DI / MVI / LXI).
  function makeBootstrap(org, dest, count, start) {
    const lo = (n) => n & 0xff, hi = (n) => (n >> 8) & 0xff;
    const A = (off) => [lo(org + off), hi(org + off)];
    const SKIP = 6, STORE = 18, LOAD = 26, RUN = 37;
    return new Uint8Array([
      0x21, lo(dest), hi(dest),        // 0   LXI  H,dest
      0x11, lo(count), hi(count),      // 3   LXI  D,count
      0xdb, 0x10,                      // 6   SKIP: IN 10h
      0x0f,                            // 8   RRC
      0xd2, ...A(SKIP),                // 9   JNC  SKIP
      0xdb, 0x11,                      // 12  IN   11h
      0xb7,                            // 14  ORA  A          ; leader null?
      0xca, ...A(SKIP),                // 15  JZ   SKIP       ; yes -- keep spooling
      0x77,                            // 18  STORE: MOV M,A
      0x23,                            // 19  INX  H
      0x1b,                            // 20  DCX  D
      0x7a, 0xb3,                      // 21  MOV A,D / ORA E
      0xca, ...A(RUN),                 // 23  JZ   RUN
      0xdb, 0x10,                      // 26  LOAD: IN 10h
      0x0f,                            // 28  RRC
      0xd2, ...A(LOAD),                // 29  JNC  LOAD
      0xdb, 0x11,                      // 32  IN   11h
      0xc3, ...A(STORE),              // 34  JMP  STORE
      0xc3, lo(start), hi(start),      // 37  RUN: JMP start
    ]);
  }

  // the streaming engine; the front panel watches tape.phase / tape.lastByte
  const tape = {
    phase: "idle",           // idle | feeding | draining
    image: null, pos: 0, lead: 0, lastByte: 0, dest: 0, start: 0,
    bytesPerSec: 0, t0: 0, drainT0: 0, label: "", me: 0,
    prompts: null, onDone: null, raw: false,

    get addr() { return (this.dest + this.pos) & 0xffff; },

    // Start the reader. Normally: key a bootstrap into the top of RAM, run it,
    // and stream the tape in. opts.raw = true: just spool the bytes into the
    // 88-2SIO -- no bootstrap, no memory touched -- for a hand-keyed loader
    // (see the panel guide). Returns false if the machine lacks the RAM.
    run(image, dest, start, bytesPerSec, opts = {}) {
      const ramTop = (machine.ramKb ? machine.ramKb() : 64) * 1024;
      const bootOrg = ramTop - 0x40;      // bootstrap sits in the top page of RAM
      this.raw = !!opts.raw;
      if (!this.raw && bootOrg < dest + image.length + 40) {
        presetHint(`not enough RAM to load ${opts.label || "that tape"}`, { persist: true });
        return false;
      }
      this.me = ++loaderToken;
      if (!this.raw) {
        machine.clearMemory?.();
        machine.reboot?.();
        const boot = makeBootstrap(bootOrg, dest & 0xffff, image.length, start & 0xffff);
        machine.loadBytes(boot, bootOrg);
        machine.setPC?.(bootOrg);
      }
      this.image = image;
      this.dest = dest & 0xffff;
      this.start = start & 0xffff;
      this.bytesPerSec = bytesPerSec > 0 ? bytesPerSec : 0;
      this.label = opts.label || "tape image";
      this.prompts = this.raw ? null : (opts.prompts || null);
      this.onDone = this.raw ? null : (opts.onDone || null);
      this.pos = 0;
      this.lead = 0;
      this.credit = 0;
      this.lastTick = 0;
      this.lastByte = 0;
      this.stall = 0;
      this.flooding = false;
      this.t0 = performance.now();
      this.phase = "feeding";
      if (!this.raw) {
        turbo = false;
        coldStartWatch = true;      // capture BASIC's cold-start prompts when it starts
        rxWatch = "";
        term.clear(); outQ.length = 0; crlfPending = false;
        setRunning(true);           // the bootstrap starts polling immediately
      }
      paperTape.setReading(true);
      return true;
    },

    // stop the reader mid-tape -- the ASR-33 lever back to STOP. Memory and the
    // CPU are left exactly as they are; a half-fed load just stalls, as it would
    // on real hardware.
    stop() {
      if (this.phase !== "feeding" && this.phase !== "draining") return;
      this.phase = "idle";
      this.me = ++loaderToken;   // detach any pending post-load prompt answering
      paperTape.setReading(false);
    },

    tick() {
      if (this.phase !== "feeding" && this.phase !== "draining") return;
      if (loaderToken !== this.me || !powered) { this.phase = "idle"; paperTape.setReading(false); return; }
      const n = this.image.length;

      if (this.phase === "feeding") {
        const total = TAPE_LEADER + n;
        const now = performance.now();
        const dt = Math.min(now - (this.lastTick || now), 200);
        this.lastTick = now;
        // accumulate a byte budget at the current rate -- changing LOAD SPEED
        // mid-load takes effect on the next frame, no jump
        if (this.bytesPerSec) {
          this.credit = Math.min((this.credit || 0) + (dt * this.bytesPerSec) / 1000,
                                 Math.max(4, this.bytesPerSec * 0.5));
        } else {
          this.credit = total;                   // "Max"
        }
        const before = this.lead + this.pos;
        while (this.credit >= 1 && this.lead + this.pos < total) {
          // Pace the feed to a loader that is keeping up, so a per-frame CPU
          // burst doesn't miss bytes a real continuously-clocked loader would
          // catch. But once the FIFO stays jammed the consumer isn't keeping up
          // (or, on a raw START, there may be no loader running at all) -- then
          // let the reader overrun the 88-2SIO and shed bytes, exactly as the
          // reader lever would on real hardware.
          if (!this.flooding && machine.rxPending && machine.rxPending() > 400) break;
          if (this.lead < TAPE_LEADER) {          // blank leader spooling through -- nothing loads yet
            this.lead++;
            machine.sendByte(0);
          } else {
            this.lastByte = this.image[this.pos++] | 0;
            machine.sendByte(this.lastByte);
          }
          this.credit -= 1;
        }
        if (this.lead + this.pos > before) this.stall = 0;
        else if (this.raw && ++this.stall > 45) this.flooding = true;   // ~0.75 s jammed -> overrun
        paperTape.setProgress((this.lead + this.pos) / total);
        if (this.pos >= n) {
          if (this.raw && this.flooding) { this.phase = "idle"; paperTape.setReading(false); return; }
          this.phase = "draining"; this.drainT0 = performance.now();
        }
        return;
      }

      // draining: wait for the loader to consume the FIFO and JMP to start
      const pending = machine.rxPending ? machine.rxPending() : 0;
      if (pending > 0 && performance.now() - this.drainT0 < 8000) return;
      this.phase = "idle";
      if (this.raw) { paperTape.setReading(false); return; }   // hand-keyed loader owns the rest
      this._afterLoad();
    },

    async _afterLoad() {
      const me = this.me;
      const dead = () => loaderToken !== me || !powered;
      paperTape.setReading(false);
      const feed = (s) => { for (const b of encoder.encode(s)) machine.sendByte(b); };
      const waitFor = (re, ms = 20000) => new Promise((res, rej) => {
        const t0 = performance.now();
        const iv = setInterval(() => {
          if (dead())                          { clearInterval(iv); rej(new Error("cancelled")); }
          else if (re.test(rxWatch))           { clearInterval(iv); res(); }
          else if (performance.now() - t0 > ms) { clearInterval(iv); rej(new Error("timed out")); }
        }, 25);
      });
      const pause = (ms) => new Promise((r) => setTimeout(r, ms));

      if (this.prompts && this.prompts.length) {
        turbo = true;
        try {
          for (const [re, reply] of this.prompts) {
            await waitFor(re);
            rxWatch = "";
            await pause(120);
            feed(reply);
          }
          await waitFor(/\bOK\b/);
        } catch { /* leave it for the user to finish by hand */ }
      }
      const cb = this.onDone; this.onDone = null;
      if (!dead() && cb) { try { await cb({ feed, waitFor, pause }); } catch {} }
      if (loaderToken === me) {
        coldStartWatch = false;
        setTimeout(() => { if (loaderToken === me) turbo = false; }, 2000);
      }
    },
  };

  // the reader you can see: a punched strip running past a read head
  const paperTape = {
    entry: null,             // catalog entry threaded, or {name,bytes,load,start}
    speed: "realistic",
    _picks: [],

    build() {
      const root = document.getElementById("ptr");
      const kase = el("ptr-case");
      const top = el("ptr-top", `<span class="ptr-plate">PAPER TAPE <span>&nbsp;READER</span></span>`);
      const spdWrap = document.createElement("span");
      spdWrap.className = "dev-speedwrap";
      spdWrap.innerHTML = '<span class="dev-speedlabel">LOAD SPEED</span>';
      const sel = document.createElement("select");
      sel.className = "ptr-speed";
      sel.innerHTML =
        '<option value="realistic">Realistic</option>' +
        '<option value="x5">5&times;</option>' +
        '<option value="x25">25&times;</option>' +
        '<option value="x50">50&times;</option>';
      try { this.speed = localStorage.getItem("retro8080.paperspeed") || "realistic"; } catch {}
      if (!PAPER_SPEED_KEYS.includes(this.speed)) this.speed = "realistic";   // drop a stale value
      sel.value = this.speed;
      sel.addEventListener("change", () => this.setSpeed(sel.value));
      spdWrap.appendChild(sel);
      top.appendChild(spdWrap);
      kase.appendChild(top);

      const body = el("ptr-body");
      const win = el("ptr-window",
        `<span class="ptr-strip"></span><span class="ptr-head"></span>` +
        `<span class="ptr-label"></span><span class="ptr-prog"></span>`);
      win.addEventListener("click", () => this.openPicker());
      const ctl = el("ptr-ctl");
      const lamp = el("ptr-lamp");
      const startB = document.createElement("button");
      startB.className = "ptr-btn ptr-start";
      startB.textContent = "START";
      startB.title = "Run the reader. Spools the tape into the 88-2SIO for a " +
        "loader that is already running (the boot PROM, or one keyed in at the " +
        "front panel). Nothing consumes the bytes if no loader is running -- " +
        "exactly like the reader lever on a real ASR-33.";
      startB.addEventListener("click", (e) => {
        e.stopPropagation();
        if (tape.phase === "feeding" || tape.phase === "draining") tape.stop();
        else this.startReader();
      });
      const loadB = document.createElement("button");
      loadB.className = "ptr-btn ptr-load";
      loadB.textContent = "AUTO-LOAD";
      loadB.title = "Shortcut: reset the machine, key a serial bootstrap into the " +
        "top of RAM, run it, read the tape, and answer BASIC's cold-start " +
        "prompts. Use START for the authentic hand-loaded path.";
      loadB.addEventListener("click", (e) => { e.stopPropagation(); this.load(); });
      const ejB = document.createElement("button");
      ejB.className = "ptr-btn ptr-eject hidden";
      ejB.textContent = "EJECT";
      ejB.addEventListener("click", (e) => { e.stopPropagation(); this.eject(); });
      ctl.append(lamp, startB, loadB, ejB);
      body.append(win, ctl);
      kase.appendChild(body);
      this._hint = "Thread a tape (click the reader). START runs the reader for a " +
        "loader you keyed in; AUTO-LOAD resets the machine and does the lot.";
      kase.appendChild(el("ptr-hint", this._hint));
      root.appendChild(kase);
      this.win = win; this.lamp = lamp; this.startBtn = startB; this.loadBtn = loadB; this.ejBtn = ejB;
      this.strip = win.querySelector(".ptr-strip");
      this.labelEl = win.querySelector(".ptr-label");
      this.prog = win.querySelector(".ptr-prog");
      this.speedSel = sel;
      this.setSpeed(this.speed);
      this.syncButtons();
    },

    setSpeed(key) {
      this.speed = PAPER_SPEEDS[key] != null ? key : "realistic";
      if (this.speedSel) this.speedSel.value = this.speed;
      // don't persist the internal "max" (?test / Auto-load) as a user choice
      if (PAPER_SPEED_KEYS.includes(this.speed))
        try { localStorage.setItem("retro8080.paperspeed", this.speed); } catch {}
      // if a tape is reading right now, change its rate on the fly
      if (tape.phase === "feeding" || tape.phase === "draining") {
        tape.bytesPerSec = PAPER_SPEEDS[this.speed] > 0 ? PAPER_SPEEDS[this.speed] : 0;
      }
    },

    setVisible(v) { document.getElementById("ptr").classList.toggle("empty", !v); },

    thread(src) {
      this.entry = src;
      const ptr = document.getElementById("ptr");
      ptr.classList.add("loaded");
      this.labelEl.textContent = (src && src.name ? src.name : "TAPE").toUpperCase().slice(0, 22);
      this.ejBtn.classList.remove("hidden");
      this.syncButtons();
      buildPanelGuide();   // the hand-load loader's byte count follows the tape
    },

    eject() {
      this.entry = null;
      const ptr = document.getElementById("ptr");
      ptr.classList.remove("loaded", "reading");
      this.labelEl.textContent = "";
      this.ejBtn.classList.add("hidden");
      this.syncButtons();
      buildPanelGuide();
    },

    setReading(on) {
      document.getElementById("ptr").classList.toggle("reading", on);
      const h = document.querySelector("#ptr .ptr-hint");
      if (h && !on) h.textContent = this._hint;
      if (!on && this.prog) this.prog.style.width = "0";
      this.syncButtons();
    },
    setProgress(frac) {
      if (this.prog) this.prog.style.width = (frac * 100).toFixed(1) + "%";
      const h = document.querySelector("#ptr .ptr-hint");
      if (h && document.getElementById("ptr").classList.contains("reading")) {
        const pct = Math.round(frac * 100);
        h.textContent = pct < 2
          ? "reading tape — leader… (change LOAD SPEED any time)"
          : `reading tape — ${pct}%  (change LOAD SPEED any time)`;
      }
    },

    syncButtons() {
      const reading = document.getElementById("ptr").classList.contains("reading");
      if (this.startBtn) {
        this.startBtn.textContent = reading ? "STOP" : "START";
        this.startBtn.disabled = !this.entry && !reading;
      }
      if (this.loadBtn) this.loadBtn.disabled = !this.entry || reading;
      if (this.ejBtn) this.ejBtn.disabled = reading;
    },

    openPicker() { ptrDialog.hidden = false; this.fillPicker(); },
    closePicker() { ptrDialog.hidden = true; term.focus(); },

    async fillPicker() {
      await loadCatalog();
      ptrList.innerHTML = "";
      this._picks = [];
      for (const r of catalog) {
        if (r.basic) continue;                        // BASIC listings are typed in
        const ok = !!r.bytes;
        const o = document.createElement("option");
        o.value = String(this._picks.length);
        o.textContent = `${r.name}  —  ${ok ? r.bytes.length + " bytes" : "(unavailable)"}`;
        o.disabled = !ok;
        ptrList.appendChild(o);
        this._picks.push(r);
      }
      const first = this._picks.findIndex((r) => r.bytes);
      if (first >= 0) {
        ptrList.value = String(first);
        ptrDesc.textContent = this._picks[first].description || "";
      }
    },

    // how a threaded tape actually loads.
    //   opts.speed  -- override the selector (preset auto-load passes "max")
    //   opts.then   -- callback once loaded (and BASIC is at OK), gets {feed,waitFor}
    load(opts = {}) {
      const e = this.entry;
      if (!e || !e.bytes) { this.flash("thread a tape first"); return; }
      // the 0xE000 ROM build can't stream -- drop it in and cold-start
      if (e.basicRom) { coldStart8k(e.bytes, { then: opts.then }); return; }
      const load  = parseHex(e.load, 0);
      const start = parseHex(e.start, load);
      const bps = PAPER_SPEEDS[opts.speed] != null ? PAPER_SPEEDS[opts.speed] : PAPER_SPEEDS[this.speed];
      const prompts = e.basic8k ? PROMPTS_8K : e.basic4k ? PROMPTS_4K : null;
      tape.run(e.bytes, load, start, bps, { label: e.name, prompts, onDone: opts.then });
    },

    // spool the threaded tape into the 88-2SIO with no bootstrap, for a loader
    // you keyed in by hand (panel guide). The CPU must already be running it.
    feedRaw() {
      const e = this.entry;
      if (!e || !e.bytes) { this.flash("thread a tape first"); return false; }
      if (tape.phase === "feeding" || tape.phase === "draining") return false;
      tape.run(e.bytes, parseHex(e.load, 0), parseHex(e.start, 0),
               PAPER_SPEEDS[this.speed], { label: e.name, raw: true });
      return true;
    },

    // the reader's own START button: run the reader motor and nothing else --
    // no memory touched, no bootstrap, no RUN. A loader has to be running
    // already (boot PROM, or one keyed in at the panel) or the bytes fall on
    // the floor, exactly as on real hardware. AUTO-LOAD (this.load) is the
    // shortcut that keys a bootstrap in and runs it for you.
    startReader() {
      if (!this.feedRaw()) return false;   // nothing threaded, or already reading
      // if no loader is running the bytes just overrun the 2SIO -- warn, since
      // the reader animation looks the same either way
      if (!running) this.flash("no loader running — press RUN or the bytes are lost");
      return true;
    },

    flash(msg) {
      const h = document.querySelector("#ptr .ptr-hint");
      if (!h) return;
      const prev = h.innerHTML;
      h.textContent = "— " + msg + " —";
      /* v8 ignore next -- flash text restores itself after 1.4 s */
      setTimeout(() => { h.innerHTML = prev; }, 1400);
    },
  };

  // Boot the 8K BASIC ROM and answer its cold-start questions. opts.then, if
  // given, runs once BASIC is at OK (used to CLOAD a cassette afterwards).
  async function coldStart8k(bytes, opts = {}) {
    const me = ++loaderToken;
    applyProgram(bytes, 0xe000, 0xe000, "Altair 8K BASIC", 0);
    turbo = true;
    coldStartWatch = true;
    rxWatch = "";
    const dead = () => loaderToken !== me || !powered;
    const feed = (s) => { for (const b of encoder.encode(s)) machine.sendByte(b); };
    const waitFor = (re, ms = 30000) => new Promise((res, rej) => {
      const t0 = performance.now();
      const iv = setInterval(() => {
        if (dead())                          { clearInterval(iv); rej(new Error("cancelled")); }
        else if (re.test(rxWatch))           { clearInterval(iv); res(); }
        else if (performance.now() - t0 > ms) { clearInterval(iv); rej(new Error("timed out at " + re.source)); }
      }, 25);
    });
    try {
      await waitFor(/MEMORY SIZE\?/i);       feed("\r");
      await waitFor(/TERMINAL WIDTH\?/i);    feed("\r");
      await waitFor(/SIN-COS-TAN-ATN\?/i);   feed("Y\r");
      await waitFor(/\bOK\b/);
      if (opts.then) await opts.then({ feed, waitFor, dead });
    } catch (e) {
      /* v8 ignore next -- cold-start prompt wait timed out / was cancelled */
      if (!dead()) term.write("\r\n\x1b[0m[loader: " + e.message + "]\r\n");
    }
    if (loaderToken === me) { coldStartWatch = false; turbo = false; }
  }

  // used as coldStart8k's `then`: BASIC is up and the Star Trek cassette is in
  // the deck -- tell the user how to pull it in (the cassette stays manual)
  async function cassetteTapeHint() {
    turbo = false;
    // queue it so it lands after BASIC's banner finishes printing, not mid-line
    const s = '\r\n\x1b[2m[ SUPER STAR TREK is in the deck -- type  CLOAD "S" , press\r\n' +
              '  PLAY on the deck, then  RUN  once it says OK ]\x1b[0m\r\n';
    for (let i = 0; i < s.length; i++) outQ.push(s.charCodeAt(i));
  }


  // --- program catalog ------------------------------------
  const parseHex = (str, fallback = 0) => {
    const n = parseInt(String(str).trim().replace(/^0x/i, ""), 16);
    return Number.isFinite(n) ? n & 0xffff : fallback;
  };

  let catalog = [];       // manifest entries, each gains a `.bytes` (or null)
  let catalogLoaded = false;

  async function loadCatalog() {
    if (catalogLoaded) return;
    catalogLoaded = true;

    let manifest;
    try {
      const resp = await fetch("roms/manifest.json");
      if (!resp.ok) throw new Error("HTTP " + resp.status);
      manifest = await resp.json();
    } catch (err) {
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
    for (const rom of catalog) {
      try {
        rom.bytes = await fetchImage(rom);
      } catch {
        rom.bytes = null;
      }
    }
  }

  function applyProgram(bytes, load, start, label, sense = 0) {
    machine.clearMemory();
    machine.loadBytes(bytes, load);
    machine.reboot();
    applySense(sense);
    if (typeof machine.setPC === "function") machine.setPC(start);
    setRunning(true);
    turbo = false;
    term.clear();           // fresh screen for the new program; its output (if
    outQ.length = 0;        // any) is all that will appear
    crlfPending = false;
    term.focus();
  }

  // --- paper-tape reader dialog wiring --------------------
  const ptrDialog   = document.getElementById("ptrDialog");
  const ptrList     = document.getElementById("ptrList");
  const ptrDesc     = document.getElementById("ptrDesc");
  const ptrFile     = document.getElementById("ptrFile");
  const ptrFileName = document.getElementById("ptrFileName");
  const ptrAddr     = document.getElementById("ptrAddr");
  const ptrStart    = document.getElementById("ptrStart");

  paperTape.build();
  paperTape.setVisible(false);
  buildPanelGuide(false);          // now that paperTape / PRESETS exist

  ptrList.addEventListener("change", () => {
    const r = paperTape._picks[Number(ptrList.value)];
    ptrDesc.textContent = r ? (r.description || "") : "";
    if (r) { ptrAddr.value = r.load || "0x0000"; ptrStart.value = r.start || r.load || "0x0000"; }
  });
  ptrFile.addEventListener("change", () => {
    ptrFileName.textContent = ptrFile.files[0] ? ptrFile.files[0].name : "no file selected";
  });
  document.getElementById("ptrClose").addEventListener("click", () => paperTape.closePicker());
  document.getElementById("ptrCancel").addEventListener("click", () => paperTape.closePicker());
  ptrDialog.addEventListener("click", (e) => { if (e.target === ptrDialog) paperTape.closePicker(); });
  document.getElementById("ptrThread").addEventListener("click", async () => {
    const file = ptrFile.files[0];
    if (file) {
      const bytes = new Uint8Array(await file.arrayBuffer());
      paperTape.thread({ name: file.name, bytes, load: ptrAddr.value, start: ptrStart.value });
    } else {
      const r = paperTape._picks[Number(ptrList.value)];
      if (!r || !r.bytes) { ptrDesc.textContent = "That tape is unavailable."; return; }
      paperTape.thread(r);
    }
    ptrFile.value = "";
    ptrFileName.textContent = "no file selected";
    paperTape.closePicker();
  });

  if (location.search.includes("debug")) {
    window.__loader = {
      get rx() { return rxWatch.slice(-400); },
      get pending() { return machine.rxPending && machine.rxPending(); },
    };
    window.__dbg = {
      get screen() {
        const b = term.buffer.active;
        let s = "";
        for (let i = 0; i < b.length; i++) s += b.getLine(i).translateToString(true) + "\n";
        return s.replace(/\n+$/, "");
      },
      get tape() { return machine.tapeStatus ? machine.tapeStatus() : null; },
      get spin() { return document.getElementById("acr").classList.contains("spin"); },
      get reading() { return document.getElementById("ptr").classList.contains("reading"); },
      get preset() { return document.getElementById("preset").value; },
      get devices() { return [...document.querySelectorAll("#deviceChips input:checked")].map((x) => x.dataset.dev); },
      get ram() { return machine.ramKb && machine.ramKb(); },
      get pc() { return machine.state && machine.state().pc; },
      get running() { return running; },
      mem: (a) => machine.readByte && machine.readByte(a & 0xffff),
    };
  }

  // Automated-test seam. `?test=1` forces the sanctioned load-speed override
  // (paper tape / cassette / floppy loads run at Max so a suite isn't held up
  // by a ~14-minute read or a needless ~166 ms/rev pause on every disk poll)
  // and exposes the internals the Playwright suite drives. The CPU still
  // runs at real 2 MHz -- nothing here is a speed knob. Absent `?test`, none
  // of this exists. See CLAUDE.md "Current sanctioned overrides".
  if (new URLSearchParams(location.search).get("test") === "1") {
    paperTape.setSpeed("max");
    cassette.setSpeed("max");
    disk.setSpeed("max");
    window.__test = {
      machine, term, paperTape, cassette, disk, tape,
      applyPreset, applyProfile, applyDevices, setRunning,
      switchState, PRESETS, TERM_PROFILES,
      get running()     { return running; },
      get switchWord()  { return switchWord(); },
      get loaderToken() { return loaderToken; },
      get applyingPreset() { return applyingPreset; },
      get outQLen()     { return outQ.length; },
      get rxWatch()     { return rxWatch; },
      regs: () => machine.state(),
      leds: () => {
        const bits = (arr) => arr.reduce((w, l, i) => w | ((l && l.classList.contains("on") ? 1 : 0) << i), 0);
        const st = {};
        for (const k in statusLeds) st[k] = statusLeds[k].classList.contains("on");
        // per-bit brightness (0 when off) -- lets a test tell a lightly-touched
        // address bit apart from a heavily-touched one, not just lit/unlit
        const addrBrightness = addrLeds.map((l) => (l && l.style.opacity ? Number(l.style.opacity) : (l && l.classList.contains("on") ? 1 : 0)));
        return { addr: bits(addrLeds), data: bits(dataLeds), status: st, addrBrightness };
      },
      screen: () => {
        const b = term.buffer.active;
        let s = "";
        for (let i = 0; i < b.length; i++) s += b.getLine(i).translateToString(true) + "\n";
        return s.replace(/\n+$/, "");
      },
    };
  }

  if (location.search.includes("help")) setTimeout(printManual, 300);

  // restore / apply an era preset (URL wins over the stored choice); a custom
  // build ("") still runs applyPreset so its load devices get set up
  buildBackplane([]);
  let startPreset = new URLSearchParams(location.search).get("preset");
  try { if (startPreset == null) startPreset = localStorage.getItem("retro8080.preset"); } catch {}
  if (startPreset == null || !PRESETS[startPreset]) startPreset = "";
  presetSelect.value = startPreset;
  setTimeout(() => applyPreset(startPreset), 200);   // let the wasm + fonts settle

}

boot();
