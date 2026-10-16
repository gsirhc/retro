"use strict";
// Output filters for the serial terminals. xterm is a full VT100+ parser; each
// filter is a stateful byte transform, state spanning metered chunks, that
// approximates a period terminal. A profile without one passes through.

// ASR-33 / Glass TTY: no escape handling. Only CR LF BS BEL HT pass; ESC and ESC[...final
// are swallowed. BS needs no translation (xterm moves left, like overstrike).
function termDumbFilter() {
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

// DEC VT52: direct address (ESC Y row col, each +32) becomes an ANSI CUP, cursor moves
// gain the CSI "[", keypad-mode and identify sequences are dropped.
function termVt52Filter() {
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

// Lear Siegler ADM-3A: bare control codes (^K up, ^L right, ^Z clear+home; ^H and ^J need
// no translation) and ESC = row col (each +32), as WordStar's ADM-3A driver emits.
function termAdm3aFilter() {
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
