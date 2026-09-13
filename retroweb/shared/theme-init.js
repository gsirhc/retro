// Anti-FOUC theme bootstrap -- shared verbatim by every machine page (and
// mirrored by retroweb/index.html's own inline copy, which can't load an
// external script this early without adding its own network round trip).
// Runs synchronously, before any themed element is painted, so there's no
// flash of the wrong theme on reload. Kept a single tiny file rather than
// folded into theme-picker.js so it can be inlined at the very top of
// <head>/<body> with nothing else to block on.
try {
  var t = localStorage.getItem("retro8080.theme") || "win", r = document.documentElement;
  if (t == "moderndark") { r.dataset.theme = "modern"; r.dataset.mode = "dark"; }
  else { r.dataset.theme = t; }
} catch (e) {}
