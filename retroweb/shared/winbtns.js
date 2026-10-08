"use strict";
// ---- Windows title-bar buttons ----
// Soft-minimize collapses .inner + footer under the pagebar. Maximize clicks
// #fullscreenBtn when present (emulators). Home close stays disabled in markup
// (Win95 CS_NOCLOSE).

(function () {
  function setMinimized(page, on) {
    page.classList.toggle("is-minimized", on);
    const min = page.querySelector(".pb-min");
    if (min) {
      min.title = on ? "Restore" : "Minimize";
      min.setAttribute("aria-label", on ? "Restore" : "Minimize");
      min.setAttribute("aria-pressed", on ? "true" : "false");
    }
  }

  function init() {
    const page = document.querySelector(".page");
    if (!page) return;
    const min = page.querySelector(".pb-min");
    const max = page.querySelector(".pb-max");

    if (min) {
      min.addEventListener("click", () => {
        setMinimized(page, !page.classList.contains("is-minimized"));
      });
    }

    if (max && !max.disabled) {
      max.addEventListener("click", () => {
        if (page.classList.contains("is-minimized")) setMinimized(page, false);
        const fs = document.getElementById("fullscreenBtn");
        if (fs) fs.click();
      });
    }
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
