/* Injects the shared site footer. Machine pages' "back to all machines"
   link (a.pb-close[href=../]) is the cue that about.html lives one level
   up; the landing page and about.html themselves use a same-directory href. */
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
      '" title="About" aria-label="About this site">' +
      '<span class="about-sign-win">Help</span>' +
      '<span class="about-sign-about">About</span></a></p>' +
      "<p>&copy; 2026 RetroCG - An indie project featuring old-ass tech " +
      "emulation you can use for free.</p>" +
      '<p>Source code available on <a class="source-link" href="' +
      SOURCE_HREF + '" target="_blank" rel="noopener noreferrer">GitHub</a></p>';
    var page = document.querySelector(".page");
    (page || document.body).appendChild(footer);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", mount);
  else mount();
})();
