// Anti-FOUC theme bootstrap -- shared verbatim by every machine page (and
// mirrored by the inline copies in retroweb/index.html and about.html,
// which can't load an external script this early without adding their own
// network round trip).
// Runs synchronously, before any themed element is painted, so there's no
// flash of the wrong theme on reload. Kept a single tiny file rather than
// folded into theme-picker.js so it can be inlined at the very top of
// <head>/<body> with nothing else to block on.
// Storage is retro8080.theme (win|winxp|web94|modern|aurora) + retro8080.mode
// (light|dark|system). Old composite theme values (moderndark, system,
// systemaurora, ai) still resolve here; theme-picker.js rewrites them.
try {
  var r = document.documentElement;
  var t = localStorage.getItem("retro8080.theme") || "win";
  var m = localStorage.getItem("retro8080.mode");
  if (t == "ai" || t == "systemaurora") { t = "aurora"; if (!m) m = "system"; }
  else if (t == "moderndark") { t = "modern"; if (!m) m = "dark"; }
  else if (t == "system") { t = "modern"; if (!m) m = "system"; }
  if (t != "win" && t != "winxp" && t != "web94" && t != "modern" && t != "aurora") t = "win";
  if (m != "light" && m != "dark" && m != "system") m = "light";
  var dark = m == "dark" || (m == "system" && matchMedia("(prefers-color-scheme: dark)").matches);
  r.dataset.theme = t;
  if (dark) r.dataset.mode = "dark";
  else delete r.dataset.mode;
} catch (e) {}
