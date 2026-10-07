// Anti-FOUC theme bootstrap -- shared verbatim by every machine page (and
// mirrored by the inline copies in retroweb/index.html and about.html,
// which can't load an external script this early without adding their own
// network round trip).
// Runs synchronously, before any themed element is painted, so there's no
// flash of the wrong theme on reload. Kept a single tiny file rather than
// folded into theme-picker.js so it can be inlined at the very top of
// <head>/<body> with nothing else to block on.
// "system" is Modern, "systemaurora" is Aurora; both follow the OS.
// Old stored value "ai" maps to systemaurora.
try {
  var t = localStorage.getItem("retro8080.theme") || "win", r = document.documentElement;
  if (t == "ai") t = "systemaurora";
  var sys = matchMedia("(prefers-color-scheme: dark)").matches;
  if (t == "moderndark" || t == "system") {
    r.dataset.theme = "modern";
    if (t == "moderndark" || sys) r.dataset.mode = "dark";
  } else if (t == "systemaurora") {
    r.dataset.theme = "aurora";
    if (sys) r.dataset.mode = "dark";
  } else {
    r.dataset.theme = t;
  }
} catch (e) {}
