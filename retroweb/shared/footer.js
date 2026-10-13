/* Injects the shared site footer. a.pb-close[href] starting with ../ marks a
   machine page (or arcade hub), so about.html is one level up; landing uses a
   disabled close span instead. */
(function () {
  function aboutHref() {
    var close = document.querySelector("a.pb-close");
    if (!close) return "about.html";
    var href = close.getAttribute("href") || "";
    return href.indexOf("..") === 0 ? "../about.html" : "about.html";
  }

  var SOURCE_HREF = "https://github.com/gsirhc/retro/tree/main/retroweb";

  function mount() {
    if (document.getElementById("siteFooter")) return;
    var footer = document.createElement("footer");
    footer.id = "siteFooter";
    footer.className = "site-footer";
    footer.innerHTML =
      '<p class="site-footer-about">' +
      '<a class="about-sign" href="' + aboutHref() +
      '" title="Help" aria-label="Help">Help</a></p>' +
      "<p>&copy; 2026 RetroCG - An indie project featuring old-ass computer " +
      "emulation you can use for free.</p>" +
      '<p>Source code available on <a class="source-link" href="' +
      SOURCE_HREF + '" target="_blank" rel="noopener noreferrer">GitHub</a></p>';
    var page = document.querySelector(".page");
    (page || document.body).appendChild(footer);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", mount);
  else mount();
})();

/* One extra line above the source link. Runs after mount, which footer.js queued first. */
function addFooterNote(html) {
  function add() {
    var footer = document.getElementById("siteFooter");
    if (!footer) return;
    var p = document.createElement("p");
    p.className = "site-footer-note";
    p.innerHTML = html;
    footer.insertBefore(p, footer.lastElementChild);
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", add);
  else add();
}
