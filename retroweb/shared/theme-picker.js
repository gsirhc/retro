"use strict";
// ---- page theme (Win95 / mid-90s web / Modern / Atmosphere × light/dark/system) -
// Shared by every machine page and by retroweb/index.html / about.html via
// retro8080.theme + retro8080.mode. A choice on any page carries across the
// whole site. data-theme is the family; data-mode="dark" when the resolved
// appearance is dark (omitted when light). Old single-key composites
// (moderndark, system, systemaurora, ai) migrate into the two keys on first
// interactive load.
//
// initThemePicker(onChange) wires #pageThemeBtn, builds #themeDialog once,
// resolves the start from (in priority order) ?theme=/&mode=, stored prefs,
// whatever theme-init.js stamped onto <html>, or win+light, and returns the
// resolved family. onChange (optional) runs after every later interactive
// change, including System mode following an OS appearance switch.

function initThemePicker(onChange) {
  const THEMES = ["win", "web94", "modern", "aurora"];
  const MODES = ["light", "dark", "system"];
  const LABELS = {
    win: "Windows 95",
    web94: "Mid-1990s Web",
    modern: "Modern",
    aurora: "Atmosphere",
  };
  const root = document.documentElement;
  const systemDark = window.matchMedia("(prefers-color-scheme: dark)");
  const btn = document.getElementById("pageThemeBtn");
  if (!btn) return "win";

  const migrateLegacy = (theme, mode) => {
    let t = theme, m = mode;
    if (t === "ai" || t === "systemaurora") { t = "aurora"; if (!m) m = "system"; }
    else if (t === "moderndark") { t = "modern"; if (!m) m = "dark"; }
    else if (t === "system") { t = "modern"; if (!m) m = "system"; }
    if (!THEMES.includes(t)) t = "win";
    if (!MODES.includes(m)) m = "light";
    return { theme: t, mode: m };
  };

  const readStored = () => {
    let theme, mode;
    try {
      theme = localStorage.getItem("retro8080.theme");
      mode = localStorage.getItem("retro8080.mode");
    } catch {}
    return migrateLegacy(theme || "win", mode);
  };

  const writeStored = (theme, mode) => {
    try {
      localStorage.setItem("retro8080.theme", theme);
      localStorage.setItem("retro8080.mode", mode);
    } catch {}
  };

  let current = readStored();
  // Rewrite legacy composites so later FOUC loads see clean keys.
  writeStored(current.theme, current.mode);

  const params = new URLSearchParams(location.search);
  const qTheme = params.get("theme");
  const qMode = params.get("mode");
  if (qTheme || qMode) {
    current = migrateLegacy(qTheme || current.theme, qMode || current.mode);
  } else if (root.dataset.theme && THEMES.includes(root.dataset.theme)) {
    // Prefer storage (already migrated) over a stale dataset from FOUC.
    current = { theme: current.theme, mode: current.mode };
  }

  const isDark = (mode) =>
    mode === "dark" || (mode === "system" && systemDark.matches);

  const apply = (theme, mode, persist) => {
    const next = migrateLegacy(theme, mode);
    current = next;
    root.dataset.theme = next.theme;
    if (isDark(next.mode)) root.dataset.mode = "dark";
    else delete root.dataset.mode;
    btn.textContent = LABELS[next.theme];
    if (persist) writeStored(next.theme, next.mode);
    syncDialog();
    return next.theme;
  };

  let dialog = document.getElementById("themeDialog");
  if (!dialog) {
    dialog = document.createElement("dialog");
    dialog.id = "themeDialog";
    dialog.className = "site-dialog";
    dialog.innerHTML =
      "<h2>Theme</h2>" +
      '<fieldset class="theme-dialog-themes">' +
      "<legend>Look</legend>" +
      THEMES.map((t) =>
        '<label><input type="radio" name="pageThemeFamily" value="' + t + '"> ' +
        LABELS[t] + "</label>").join("") +
      "</fieldset>" +
      '<fieldset class="theme-dialog-modes">' +
      "<legend>Appearance</legend>" +
      '<label><input type="radio" name="pageThemeMode" value="light"> Light</label>' +
      '<label><input type="radio" name="pageThemeMode" value="dark"> Dark</label>' +
      '<label><input type="radio" name="pageThemeMode" value="system"> System</label>' +
      "</fieldset>" +
      '<div class="row"><button type="button" id="themeDialogDone">Done</button></div>';
    document.body.appendChild(dialog);
  }

  const syncDialog = () => {
    const themeInput = dialog.querySelector(
      'input[name="pageThemeFamily"][value="' + current.theme + '"]');
    const modeInput = dialog.querySelector(
      'input[name="pageThemeMode"][value="' + current.mode + '"]');
    if (themeInput) themeInput.checked = true;
    if (modeInput) modeInput.checked = true;
  };

  const liveFromDialog = () => {
    const themeEl = dialog.querySelector('input[name="pageThemeFamily"]:checked');
    const modeEl = dialog.querySelector('input[name="pageThemeMode"]:checked');
    apply(themeEl ? themeEl.value : current.theme,
          modeEl ? modeEl.value : current.mode, true);
    if (onChange) onChange();
  };

  dialog.addEventListener("change", liveFromDialog);
  dialog.querySelector("#themeDialogDone").addEventListener("click", () => {
    dialog.close();
  });

  btn.addEventListener("click", () => {
    syncDialog();
    dialog.showModal();
  });

  apply(current.theme, current.mode, true);

  systemDark.addEventListener("change", () => {
    if (current.mode !== "system") return;
    apply(current.theme, current.mode, false);
    if (onChange) onChange();
  });

  return current.theme;
}
