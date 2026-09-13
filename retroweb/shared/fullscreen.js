"use strict";
// ---- fullscreen -----------------------------------------------------------
// Expands the bezel (CRT frame + vignette + power LED), not the bare screen
// element -- see the CSS comment by #bezel:fullscreen for why. A pure
// web-UI convenience with no real hardware to be faithful to.
//
// initFullscreen(opts):
//   bezelEl, screenEl   -- required. The element the Fullscreen API expands,
//                          and the element that gets keyboard focus back.
//   fullscreenBtn       -- required. The toggle button (#fullscreenBtn).
//   isRunning           -- optional zero-arg predicate; when it returns
//                          true, screenEl is refocused after any fullscreen
//                          transition. Defaults to always true.
//   fsEscHint, fsEscHintOkBtn -- optional. A <dialog> (#fsEscHint) warning
//                          that a real Esc keypress can't reach the guest
//                          while fullscreen (the browser reserves it to
//                          exit fullscreen and never dispatches it to the
//                          page at all), shown once before the *first* time
//                          fullscreen is entered. Skipped entirely if the
//                          machine doesn't pass one.
//   escBtn, sendEscape  -- optional. A button (#escBtn, shown only while
//                          fullscreen -- see fullscreen.css) whose click
//                          calls sendEscape() to inject an Escape keystroke
//                          straight into the guest, bypassing the native key
//                          event this whole feature is a workaround for.
//                          Omit both when a machine already has its own
//                          generic on-screen-key-button plumbing that
//                          matches #escBtn some other way (e.g. ibmpc-at's
//                          shared [data-key] handling covers it already).
//   onFullscreenChange  -- optional, called (with no args) after every
//                          fullscreen transition, once the button label/
//                          focus above are already updated. ibmpc-at's
//                          #screen is a <canvas> with an aspect-ratio CSS
//                          property, so plain `width:auto;height:100%` (see
//                          fullscreen.css) is enough to make it fill the
//                          fullscreened bezel. altair8800's and
//                          assembler6502's #screen is a <div> wrapping
//                          xterm.js, sized via inline pixel width/height
//                          JS sets to fit the *normal* page layout -- those
//                          inline styles beat the stylesheet rule outright,
//                          so those two machines use this hook to size
//                          against the fullscreened bezel instead (see
//                          their own app.js).
function initFullscreen(opts) {
  const { bezelEl, screenEl, fullscreenBtn, fsEscHint, fsEscHintOkBtn, escBtn, sendEscape, onFullscreenChange } = opts;
  const isRunning = opts.isRunning || (() => true);

  function isFullscreen() {
    return (document.fullscreenElement || document.webkitFullscreenElement) === bezelEl;
  }
  function enterFullscreen() {
    (bezelEl.requestFullscreen || bezelEl.webkitRequestFullscreen).call(bezelEl);
  }

  // One-time "your Esc key won't reach the guest" hint -- not shown again
  // once seen, tracked the same way the theme picker remembers its own
  // choice (localStorage, wrapped in try/catch: private browsing can throw
  // on either call). The version string is the cache-bust: bump it whenever
  // the hint's content changes meaningfully, and everyone who saw an older
  // version sees it again, since their stored value no longer matches.
  const FS_ESC_HINT_VERSION = "2";
  function fsEscHintSeen() {
    try { return localStorage.getItem("retro8080.fsEscHintSeen") === FS_ESC_HINT_VERSION; } catch { return false; }
  }
  if (fsEscHint) {
    // Closing the dialog always means "go fullscreen now, and don't ask
    // again" -- whether that's the "Got it" button or the browser's own
    // native Escape-cancels-a-dialog behavior (a plain page dialog, not the
    // fullscreen problem this hint is about; Escape closing it here is
    // completely normal).
    if (fsEscHintOkBtn) fsEscHintOkBtn.addEventListener("click", () => fsEscHint.close());
    fsEscHint.addEventListener("close", () => {
      try { localStorage.setItem("retro8080.fsEscHintSeen", FS_ESC_HINT_VERSION); } catch {}
      enterFullscreen();
      if (isRunning()) screenEl.focus();
    });
  }

  function updateFullscreenBtn() {
    const label = isFullscreen() ? "Exit fullscreen" : "Fullscreen";
    fullscreenBtn.title = label;
    fullscreenBtn.setAttribute("aria-label", label);
    // Re-grab keyboard focus on the way both in and out -- fullscreen
    // transitions move focus to the bezel itself, and a real machine has no
    // such thing as "the front panel has focus". Skipped while the hint
    // dialog is open so it doesn't fight the dialog's own focused button.
    if (isRunning() && !(fsEscHint && fsEscHint.open)) screenEl.focus();
    if (onFullscreenChange) onFullscreenChange();
  }
  fullscreenBtn.addEventListener("click", () => {
    if (isFullscreen()) {
      (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    } else if (!fsEscHint || fsEscHintSeen()) {
      enterFullscreen();
    } else {
      // Show the hint first and wait for it to be dismissed (see the
      // dialog's own "close" listener above) -- a first-time visitor should
      // read this before the screen jumps, not have it appear after.
      fsEscHint.showModal();
    }
  });
  document.addEventListener("fullscreenchange", updateFullscreenBtn);
  document.addEventListener("webkitfullscreenchange", updateFullscreenBtn);
  // Known limitation, not fixable from here: browsers reserve the real Esc
  // key to exit fullscreen and never dispatch it to the page at all while
  // doing so. There's no way for page script to claim it back from the
  // Fullscreen API -- escBtn (see fullscreen.css) is the workaround.
  if (escBtn && sendEscape) {
    escBtn.addEventListener("click", () => sendEscape());
  }
}
