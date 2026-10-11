"use strict";
// Disk library dialog for the PC machines, fed by disks/library/library.json.
// opts: { button, drives: [{ id, name, accepts(format) }], insert(driveId, label, bytes) -> Promise }
// format is a floppy size in K, or "cd" for an entry with "media": "cd".
function initDiskLibrary(opts) {
  const BASE = "disks/library/";
  const dlg = document.createElement("dialog");
  dlg.id = "libraryDialog";
  dlg.innerHTML =
    '<h2>Disk library</h2>' +
    '<div class="lib-body">' +
    '<div class="lib-list" role="listbox" aria-label="Disks"></div>' +
    '<div class="lib-detail">' +
    '<h3 class="lib-title"></h3>' +
    '<p class="lib-meta"></p>' +
    '<p class="lib-desc"></p>' +
    '<div class="lib-disks"></div>' +
    '<pre class="lib-help"></pre>' +
    '<p class="lib-license"></p>' +
    '</div></div>' +
    '<div class="row"><span class="lib-status"></span><button type="button" class="lib-close">Close</button></div>';
  document.body.appendChild(dlg);

  const $ = (sel) => dlg.querySelector(sel);
  const list = $(".lib-list");
  const status = $(".lib-status");
  let catalog = null;
  let current = null;

  const formatOf = (entry) => entry.media === "cd" ? "cd" : (entry.build && entry.build.format) || entry.format || 1440;
  const metaLine = (e) => [e.year, e.publisher, e.needs && "Needs " + e.needs].filter(Boolean).join("  ·  ");

  function select(i) {
    current = catalog[i];
    list.querySelectorAll(".lib-item").forEach((el, j) => el.setAttribute("aria-selected", String(i === j)));
    $(".lib-title").textContent = current.title;
    $(".lib-meta").textContent = metaLine(current);
    $(".lib-desc").textContent = current.description || "";
    $(".lib-help").textContent = current.help || "";
    $(".lib-license").textContent = current.license || "";
    const disks = $(".lib-disks");
    disks.innerHTML = "";
    const fmt = formatOf(current);
    for (const img of current.images) {
      const row = document.createElement("div");
      row.className = "lib-disk";
      const name = document.createElement("span");
      name.textContent = current.images.length > 1 ? img.label
        : fmt === "cd" ? "CD-ROM" : `${fmt >= 1000 ? fmt / 1000 + "MB" : fmt + "K"} diskette`;
      row.appendChild(name);
      for (const d of opts.drives) {
        if (!d.accepts(fmt)) continue;
        const b = document.createElement("button");
        b.type = "button";
        b.textContent = `Insert in ${d.name}`;
        b.dataset.file = img.file;
        b.dataset.drive = String(d.id);
        b.addEventListener("click", () => insert(current, img, d));
        row.appendChild(b);
      }
      disks.appendChild(row);
    }
    status.textContent = "";
  }

  async function insert(entry, img, drive) {
    status.textContent = "Fetching…";
    try {
      const r = await fetch(BASE + img.file);
      if (!r.ok) throw new Error("HTTP " + r.status);
      const bytes = new Uint8Array(await r.arrayBuffer());
      const label = entry.images.length > 1 ? `${entry.title}, ${img.label.toLowerCase()}` : entry.title;
      await opts.insert(drive.id, label, bytes);
      status.textContent = `${entry.images.length > 1 ? img.label : "It"} is in ${drive.name}`;
    } catch (err) {
      console.error("disk library:", err);
      status.textContent = "Couldn't fetch that disk. See the console.";
    }
  }

  async function load() {
    if (catalog) return;
    try {
      const r = await fetch(BASE + "library.json");
      if (!r.ok) throw new Error("HTTP " + r.status);
      catalog = (await r.json()).disks || [];
    } catch (err) {
      console.warn("disk library:", err);
      catalog = [];
      list.textContent = "The library isn't installed on this server.";
      return;
    }
    catalog.forEach((e, i) => {
      const item = document.createElement("button");
      item.type = "button";
      item.className = "lib-item";
      item.setAttribute("role", "option");
      item.innerHTML = '<span class="lib-item-title"></span><span class="lib-item-year"></span>';
      item.querySelector(".lib-item-title").textContent = e.title;
      item.querySelector(".lib-item-year").textContent = e.year || "";
      item.addEventListener("click", () => select(i));
      list.appendChild(item);
    });
    if (catalog.length) select(0);
  }

  opts.button.addEventListener("click", async () => {
    dlg.showModal();
    await load();
  });
  $(".lib-close").addEventListener("click", () => dlg.close());
  dlg.addEventListener("click", (e) => { if (e.target === dlg) dlg.close(); });
}
