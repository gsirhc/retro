"use strict";
// ---- fullscreen ----
// Expands the whole bezel (see fullscreen.css). Web-UI convenience only.
// initFullscreen(opts): bezelEl, screenEl, fullscreenBtn required; optional
// isRunning (predicate, default true), fsEscHint + fsEscHintOkBtn (first-use
// dialog), escBtn + sendEscape (inject Escape into the guest), onFullscreenChange
// (machines whose #screen is sized by inline JS resize against the bezel here).
function initFullscreen(opts) {
  const { bezelEl, screenEl, fullscreenBtn, fsEscHint, fsEscHintOkBtn, escBtn, sendEscape, onFullscreenChange } = opts;
  const isRunning = opts.isRunning || (() => true);

  function isFullscreen() {
    return (document.fullscreenElement || document.webkitFullscreenElement) === bezelEl;
  }
  function enterFullscreen() {
    (bezelEl.requestFullscreen || bezelEl.webkitRequestFullscreen).call(bezelEl);
  }

  // One-time Esc hint. Bump the version to show it again after a content change.
  const FS_ESC_HINT_VERSION = "2";
  function fsEscHintSeen() {
    try { return localStorage.getItem("retro8080.fsEscHintSeen") === FS_ESC_HINT_VERSION; } catch { return false; }
  }
  if (fsEscHint) {
    // any close (button or native Escape) enters fullscreen and marks the hint seen
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
    // refocus the guest on the way in and out, but not over the hint dialog
    if (isRunning() && !(fsEscHint && fsEscHint.open)) screenEl.focus();
    if (onFullscreenChange) onFullscreenChange();
  }
  fullscreenBtn.addEventListener("click", () => {
    if (isFullscreen()) {
      (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    } else if (!fsEscHint || fsEscHintSeen()) {
      enterFullscreen();
    } else {
      // hint first; its close handler enters fullscreen
      fsEscHint.showModal();
    }
  });
  document.addEventListener("fullscreenchange", updateFullscreenBtn);
  document.addEventListener("webkitfullscreenchange", updateFullscreenBtn);
  // browsers never dispatch the real Esc to the page in fullscreen; escBtn is the workaround
  if (escBtn && sendEscape) {
    escBtn.addEventListener("click", () => sendEscape());
  }

  // clicking a control in fullscreen steals focus from the guest; give it back
  const bezelControls = bezelEl.querySelector(".bezel-controls");
  if (bezelControls) {
    bezelControls.addEventListener("click", (e) => {
      if (!isFullscreen() || !isRunning()) return;
      if (!e.target.closest(".bezel-btn")) return;
      queueMicrotask(() => { if (isRunning()) screenEl.focus(); });
    });
  }
}
