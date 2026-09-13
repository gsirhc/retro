"use strict";
// "Click to focus" banner -- purely a web-UI convenience (no hardware
// equivalent), see the CSS comment in focus-hint.css for why it exists.
//
// initFocusHint(screenEl, isRunning) wires the banner (expects a
// #focusHint element) to screenEl's focus and returns an update() function
// -- call it from the machine's own power-on/power-off handlers too, since
// "running" isn't only a focus-driven state.
// isRunning is a zero-arg predicate (not a plain boolean) so it can read
// whatever live state variable each machine's app.js declares for "the
// machine is powered/running" at the moment update() actually runs, rather
// than a value captured once at wiring time.
//
// Uses screenEl.contains(activeElement) rather than strict equality, and
// focusin/focusout (which bubble) rather than focus/blur (which don't):
// ibmpc-at's #screen is a plain focusable <canvas>, but altair8800's and
// assembler6502's is a <div> wrapping xterm.js's own internally-created
// <textarea> -- keystrokes actually focus that nested textarea, never the
// div itself, so a strict-equality/non-bubbling check would see every
// keypress there as "unfocused" and never hide the banner.
function initFocusHint(screenEl, isRunning) {
  const hintEl = document.getElementById("focusHint");
  function update() {
    hintEl.classList.toggle("visible", isRunning() && !screenEl.contains(document.activeElement));
  }
  document.addEventListener("focusin", update);
  document.addEventListener("focusout", update);
  return update;
}
