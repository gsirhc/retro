"use strict";
// ---- page theme (Win95 / mid-90s Mosaic web / Modern / Dark Modern) -------
// Shared by every machine page (and mirrored by retroweb/index.html's own
// simpler copy, which has no #pageTheme <select> of its own) via the single
// retro8080.theme localStorage key -- a theme picked on any page carries
// across the whole site. "moderndark" is Modern's layout with
// data-mode="dark" bolted on, so the <select> value and the stored key
// differ from the data-theme attribute actually set on <html>.
//
// initThemePicker(onChange) wires up #pageTheme, resolves the starting theme
// from (in priority order) ?theme=, the stored preference, whatever
// theme-init.js already stamped onto <html> before first paint, or "win",
// and returns that resolved value. onChange (optional) runs after every
// later interactive change -- each machine's own post-theme-change layout
// work (re-measuring a terminal's cell size, repositioning a side-by-side
// bar) goes there instead of being duplicated in this shared file.
function initThemePicker(onChange) {
  const pageTheme = document.getElementById("pageTheme");
  const root = document.documentElement;
  const THEME_VALUES = ["win", "web94", "modern", "moderndark", "ai"];
  const applyTheme = (v) => {
    if (!THEME_VALUES.includes(v)) v = "win";
    if (v === "moderndark") { root.dataset.theme = "modern"; root.dataset.mode = "dark"; }
    else { root.dataset.theme = v; delete root.dataset.mode; }
    return v;
  };
  let stored; try { stored = localStorage.getItem("retro8080.theme"); } catch {}
  const savedPageTheme = applyTheme(
    new URLSearchParams(location.search).get("theme") || stored || root.dataset.theme || "win");
  pageTheme.value = savedPageTheme;
  pageTheme.addEventListener("change", () => {
    applyTheme(pageTheme.value);
    try { localStorage.setItem("retro8080.theme", pageTheme.value); } catch {}
    if (onChange) onChange();
  });
  return savedPageTheme;
}
