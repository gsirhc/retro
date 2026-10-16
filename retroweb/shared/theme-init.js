// Theme bootstrap, run before first paint. Inlined copies: index.html, about.html, arcade/index.html.
try {
  var r = document.documentElement;
  var t = localStorage.getItem("retro8080.theme") || "win";
  var m = localStorage.getItem("retro8080.mode");
  // pre-split composite values; theme-picker.js rewrites them
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
