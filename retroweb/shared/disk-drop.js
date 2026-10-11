"use strict";
// Drop a disk image anywhere on the page. message() is read on dragenter, since the
// browser doesn't expose the file name until the drop.
// opts: { accept: RegExp, message() -> string, onDrop(file), onReject(file) }
function initDiskDrop(opts) {
  const veil = document.createElement("div");
  veil.className = "disk-drop-veil";
  veil.hidden = true;
  const msg = document.createElement("div");
  msg.className = "disk-drop-msg";
  veil.appendChild(msg);
  document.body.appendChild(veil);

  let depth = 0;
  const hasFiles = (e) => !!e.dataTransfer && Array.from(e.dataTransfer.types || []).includes("Files");
  const hide = () => { depth = 0; veil.hidden = true; };

  document.addEventListener("dragenter", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    if (depth++ === 0) {
      msg.textContent = opts.message();
      veil.hidden = false;
    }
  });
  document.addEventListener("dragover", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "copy";
  });
  document.addEventListener("dragleave", (e) => {
    if (!hasFiles(e)) return;
    if (--depth <= 0) hide();
  });
  document.addEventListener("drop", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    hide();
    const file = e.dataTransfer.files[0];
    if (!file) return;
    if (opts.accept.test(file.name)) opts.onDrop(file);
    else if (opts.onReject) opts.onReject(file);
  });

  let flashTimer = 0;
  return {
    flash(text) {
      msg.textContent = text;
      veil.hidden = false;
      clearTimeout(flashTimer);
      flashTimer = setTimeout(() => { if (depth === 0) veil.hidden = true; }, 2500);
    },
  };
}
