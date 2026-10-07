"use strict";
// ---- page theme (Win95 / mid-90s web / Modern / System Aurora) ----------
// Shared by every machine page (and mirrored by the inline copies in
// retroweb/index.html and about.html) via the single
// retro8080.theme localStorage key -- a theme picked on any page carries
// across the whole site. "moderndark" is Modern's layout with
// data-mode="dark" bolted on; "system" is that same layout following the
// OS appearance; "systemaurora" is the purple/cyan Aurora palette
// following the OS. Stored keys can differ from the data-theme attribute
// actually set on <html>. Old stored value "ai" maps to systemaurora.
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
  const THEME_VALUES = ["win", "web94", "modern", "moderndark", "system", "systemaurora"];
  const systemDark = window.matchMedia("(prefers-color-scheme: dark)");
  const applyTheme = (v) => {
    if (v === "ai") v = "systemaurora";
    if (!THEME_VALUES.includes(v)) v = "win";
    let theme = v;
    let dark = false;
    if (v === "moderndark") { theme = "modern"; dark = true; }
    else if (v === "system") { theme = "modern"; dark = systemDark.matches; }
    else if (v === "systemaurora") { theme = "aurora"; dark = systemDark.matches; }
    root.dataset.theme = theme;
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
    if (pageTheme.value !== "system" && pageTheme.value !== "systemaurora") return;
    applyTheme(pageTheme.value);
    if (onChange) onChange();
  });
  return savedPageTheme;
}
