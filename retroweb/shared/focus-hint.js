"use strict";
// "Click to focus" banner. isRunning is a predicate so it reads live state.
// Returns update(); call it from power on/off handlers too.
// Uses contains() and focusin/focusout because xterm.js focuses a nested
// textarea, never the #screen div itself.
function initFocusHint(screenEl, isRunning) {
  const hintEl = document.getElementById("focusHint");
  function update() {
    hintEl.classList.toggle("visible", isRunning() && !screenEl.contains(document.activeElement));
  }
  document.addEventListener("focusin", update);
  document.addEventListener("focusout", update);
  return update;
}
