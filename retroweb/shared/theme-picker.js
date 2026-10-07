"use strict";
// ---- page theme (Win95 / mid-90s web / Light / Dark / System Modern) ----
// Shared by every machine page (and mirrored by the inline copies in
// retroweb/index.html and about.html) via the single
// retro8080.theme localStorage key -- a theme picked on any page carries
// across the whole site. "moderndark" is Modern's layout with
// data-mode="dark" bolted on, and "system" is that same layout following
// the OS appearance, so the <select> value and the stored key differ from
// the data-theme attribute actually set on <html>.
//
// initThemePicker(onChange) wires up #pageTheme, resolves the starting theme
// from (in priority order) ?theme=, the stored preference, whatever
// theme-init.js already stamped onto <html> before first paint, or "win",
// and returns that resolved value. onChange (optional) runs after every
// later interactive change, including a System theme following an OS
// appearance switch. Each machine's own post-theme-change layout work
// (re-measuring a terminal's cell size, repositioning a side-by-side bar)
// goes there instead of being duplicated in this shared file.
function initThemePicker(onChange) {
  const pageTheme = document.getElementById("pageTheme");
  const root = document.documentElement;
  const THEME_VALUES = ["win", "web94", "modern", "moderndark", "system", "ai"];
  const systemDark = window.matchMedia("(prefers-color-scheme: dark)");
  const applyTheme = (v) => {
    if (!THEME_VALUES.includes(v)) v = "win";
    const dark = v === "moderndark" || (v === "system" && systemDark.matches);
    root.dataset.theme = (v === "moderndark" || v === "system") ? "modern" : v;
    if (dark) root.dataset.mode = "dark";
    else delete root.dataset.mode;
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
  systemDark.addEventListener("change", () => {
    if (pageTheme.value !== "system") return;
    applyTheme("system");
    if (onChange) onChange();
  });
  return savedPageTheme;
}
