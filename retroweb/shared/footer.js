/* Injects the shared site footer. Machine pages' "back to all machines"
   link (a.pb-close[href=../]) is the cue that about.html lives one level
   up; the landing page and about.html themselves use a same-directory href. */
(function () {
  function aboutHref() {
    var close = document.querySelector("a.pb-close");
    return close ? "../about.html" : "about.html";
  }

  // Backlit US EXIT (white acrylic housing, condensed red lettering, chevrons
  // both ways). Letters are paths — a real sign's I has no serifs, and the
  // E's middle arm is short, which Arial Black would get wrong.
  var SIGN =
    '<svg viewBox="0 0 154 62" aria-hidden="true">' +
      '<defs>' +
        '<linearGradient id="sfFace" x1="0" y1="0" x2="0" y2="1">' +
          '<stop offset="0%" stop-color="#fffcf4"/>' +
          '<stop offset="40%" stop-color="#ffffff"/>' +
          '<stop offset="100%" stop-color="#e6decc"/>' +
        '</linearGradient>' +
        '<radialGradient id="sfHot" cx="50%" cy="40%" r="62%">' +
          '<stop offset="0%" stop-color="#ffffff"/>' +
          '<stop offset="100%" stop-color="#f3ead8" stop-opacity="0"/>' +
        '</radialGradient>' +
      '</defs>' +
      '<rect x="0.5" y="0.5" width="153" height="61" rx="1.2" fill="#c9c3b5" stroke="#b4ae9f" stroke-width="0.75"/>' +
      '<rect x="2.4" y="2.4" width="149.2" height="57.2" rx="0.5" fill="url(#sfFace)"/>' +
      '<rect x="2.4" y="2.4" width="149.2" height="57.2" rx="0.5" fill="url(#sfHot)"/>' +
      '<g fill="none" stroke="#ed3b32" stroke-width="1.7" stroke-linejoin="miter" stroke-miterlimit="8">' +
        '<polyline points="16,20.5 8,31 16,41.5"/>' +
        '<polyline points="138,20.5 146,31 138,41.5"/>' +
      '</g>' +
      '<g fill="#ed3b32">' +
        '<path d="M22,8 h28 v8.6 h-19.2 v10.1 h15 v8.6 h-15 v10.1 h19.2 v8.6 h-28 z"/>' +
        '<path d="M54,8 h6.6 L84,47.2 V54 h-6.6 L54,14.8 z"/>' +
        '<path d="M84,8 h-6.6 L54,47.2 V54 h6.6 L84,14.8 z"/>' +
        '<rect x="89.5" y="8" width="8.6" height="46"/>' +
        '<path d="M103,8 h29.5 v8.6 h-10.45 v37.4 h-8.6 v-37.4 h-10.45 z"/>' +
      '</g>' +
    '</svg>';

  function mount() {
    if (document.getElementById("siteFooter")) return;
    var footer = document.createElement("footer");
    footer.id = "siteFooter";
    footer.className = "site-footer";
    footer.innerHTML =
      '<p>&copy; Copyright 2026. Built in conjunction with machine intelligence (Cursor / Claude).</p>' +
      '<p>If using artificial intelligence to build retro emulators offends your ' +
      'sensibilities, please head to the exit.</p>' +
      '<p class="site-footer-exit">' +
      '<a class="exit-sign" href="' + aboutHref() + '" title="About" aria-label="exit — about this site">' +
      SIGN + "</a></p>";
    var page = document.querySelector(".page");
    (page || document.body).appendChild(footer);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", mount);
  else mount();
})();
