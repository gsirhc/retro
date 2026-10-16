function initThumbArt(sel, cards, key, system) {
  var LIBRETRO = "https://thumbnails.libretro.com/" + system + "/";
  var DIRS = { snap: "Named_Snaps", title: "Named_Titles", boxart: "Named_Boxarts" };
  var LABELS = { snap: "screenshot from libretro-thumbnails", title: "title screen from libretro-thumbnails", boxart: "box art from libretro-thumbnails", marquee: "marquee from Arcade Database" };
  if (!sel) return;

  function artUrl(c, mode) {
    if (mode === "marquee") return "https://adb.arcadeitalia.net/media/mame.current/marquees/" + c.getAttribute("data-marquee") + ".png";
    return LIBRETRO + DIRS[mode] + "/" + encodeURIComponent(c.getAttribute("data-art")) + ".png";
  }

  function local(c, img) {
    c.classList.remove("art", "marquee");
    img.src = img.getAttribute("data-local");
    img.alt = img.getAttribute("data-local-alt");
    img.style.objectPosition = "";
    img.classList.add("art-ready");
  }

  cards.forEach(function (c) {
    var img = c.querySelector("img.shot");
    img.setAttribute("data-local", img.getAttribute("src"));
    img.setAttribute("data-local-alt", img.getAttribute("alt"));
    img.addEventListener("load", function () {
      if (c.classList.contains("art")) img.classList.add("art-ready");
    });
    img.addEventListener("error", function () {
      if (c.classList.contains("art")) local(c, img);
    });
  });

  function apply(mode) {
    if (!LABELS[mode] || !sel.querySelector('option[value="' + mode + '"]')) mode = "";
    sel.value = mode;
    cards.forEach(function (c) {
      var img = c.querySelector("img.shot");
      if (!mode) return local(c, img);
      var y = c.getAttribute("data-" + mode + "-y");
      c.classList.add("art");
      c.classList.toggle("marquee", mode === "marquee");
      img.src = artUrl(c, mode);
      img.alt = c.getAttribute("data-art") + " " + LABELS[mode];
      img.style.objectPosition = y ? "center " + y : "";
    });
  }

  var mode = "";
  try { mode = localStorage.getItem(key) || ""; } catch (e) {}
  if (mode) apply(mode);

  sel.addEventListener("change", function () {
    try { localStorage.setItem(key, sel.value); } catch (e) {}
    apply(sel.value);
  });
}
