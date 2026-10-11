"use strict";
// On-screen joystick and buttons for phones and tablets. They press the same codes the keyboard does.

function arcadeTouchOnly() {
  try {
    return matchMedia("(pointer: coarse)").matches && !matchMedia("(any-pointer: fine)").matches;
  } catch {
    return false;
  }
}

// opts: { root, stick: "4way" | "8way" | "2way" | null, buttons: [{ code, label, cls }],
//         groups: optional [[codes left], [codes center], [codes right]], setKey(code, down) }
function initArcadeTouch(opts) {
  const { root, stick, setKey } = opts;
  const buttons = opts.buttons || [];
  const buzz = () => { try { if (navigator.vibrate) navigator.vibrate(12); } catch {} };

  const panel = document.createElement("div");
  panel.className = "touch-panel";
  root.appendChild(panel);

  // ---- joystick ----
  let stickEl = null;
  let ball = null;
  let dir = { up: false, down: false, left: false, right: false };
  const dirCodes = { up: "ArrowUp", down: "ArrowDown", left: "ArrowLeft", right: "ArrowRight" };
  function setDir(next) {
    let changed = false;
    for (const k of Object.keys(dirCodes)) {
      if (next[k] !== dir[k]) {
        setKey(dirCodes[k], next[k]);
        changed = true;
      }
    }
    if (changed && (next.up || next.down || next.left || next.right)) buzz();
    dir = next;
    stickEl.dataset.dir = ["up", "down", "left", "right"].filter((k) => dir[k]).join(" ");
    const x = (dir.right ? 1 : 0) - (dir.left ? 1 : 0);
    const y = (dir.down ? 1 : 0) - (dir.up ? 1 : 0);
    const n = x && y ? Math.SQRT1_2 : 1;
    ball.style.transform = `translate(${x * n * 28}%, ${y * n * 28}%)`;
  }
  const kDead = 0.25;
  function resolve(dx, dy) {
    const none = { up: false, down: false, left: false, right: false };
    const mag = Math.hypot(dx, dy);
    if (stick === "2way") {
      if (Math.abs(dx) < kDead) return none;
      return { ...none, left: dx < 0, right: dx > 0 };
    }
    if (mag < kDead) return none;
    if (stick === "8way") {
      const sector = Math.round(Math.atan2(dy, dx) / (Math.PI / 4));
      const s = ((sector % 8) + 8) % 8;
      return {
        right: s === 0 || s === 1 || s === 7,
        down: s === 1 || s === 2 || s === 3,
        left: s === 3 || s === 4 || s === 5,
        up: s === 5 || s === 6 || s === 7,
      };
    }
    // 4-way restrictor: no diagonals. The held direction sticks until the other axis clearly wins.
    const horizNow = dir.left || dir.right;
    const vertNow = dir.up || dir.down;
    let horiz = Math.abs(dx) > Math.abs(dy);
    if (horizNow && Math.abs(dx) * 1.3 > Math.abs(dy)) horiz = true;
    else if (vertNow && Math.abs(dy) * 1.3 > Math.abs(dx)) horiz = false;
    return horiz ? { ...none, left: dx < 0, right: dx > 0 } : { ...none, up: dy < 0, down: dy > 0 };
  }
  if (stick) {
    stickEl = document.createElement("div");
    stickEl.className = "touch-stick";
    stickEl.dataset.ways = stick;
    stickEl.setAttribute("role", "group");
    stickEl.setAttribute("aria-label", stick === "2way" ? "Joystick, left and right" : "Joystick");
    const gate = document.createElement("div");
    gate.className = "touch-gate";
    ball = document.createElement("div");
    ball.className = "touch-ball";
    gate.appendChild(ball);
    stickEl.appendChild(gate);
    let activeId = null;
    const track = (e) => {
      const r = gate.getBoundingClientRect();
      const dx = (e.clientX - (r.left + r.width / 2)) / (r.width / 2);
      const dy = (e.clientY - (r.top + r.height / 2)) / (r.height / 2);
      setDir(resolve(dx, dy));
    };
    stickEl.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      activeId = e.pointerId;
      try { stickEl.setPointerCapture(e.pointerId); } catch {}
      track(e);
    });
    stickEl.addEventListener("pointermove", (e) => {
      if (e.pointerId === activeId) track(e);
    });
    const end = (e) => {
      if (e.pointerId !== activeId) return;
      activeId = null;
      setDir({ up: false, down: false, left: false, right: false });
    };
    stickEl.addEventListener("pointerup", end);
    stickEl.addEventListener("pointercancel", end);
  }

  // ---- buttons ----
  const btnEls = new Map();
  for (const b of buttons) {
    const el = document.createElement("button");
    el.type = "button";
    el.className = "touch-btn" + (b.cls ? " " + b.cls : "");
    el.dataset.code = b.code;
    el.setAttribute("aria-label", b.label);
    const cap = document.createElement("span");
    cap.className = "touch-btn-cap";
    const lbl = document.createElement("span");
    lbl.className = "touch-btn-label";
    lbl.textContent = b.label;
    el.append(cap, lbl);
    const down = (e) => {
      e.preventDefault();
      try { el.setPointerCapture(e.pointerId); } catch {}
      if (el.classList.contains("pressed")) return;
      el.classList.add("pressed");
      setKey(b.code, true);
      buzz();
    };
    const up = () => {
      if (!el.classList.contains("pressed")) return;
      el.classList.remove("pressed");
      setKey(b.code, false);
    };
    el.addEventListener("pointerdown", down);
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    el.addEventListener("contextmenu", (e) => e.preventDefault());
    btnEls.set(b.code, el);
  }

  const groups = opts.groups || [stick ? ["stick"] : [], buttons.map((b) => b.code)];
  for (const g of groups) {
    if (g.length === 0) continue;
    const col = document.createElement("div");
    col.className = "touch-group";
    for (const id of g) col.appendChild(id === "stick" ? stickEl : btnEls.get(id));
    panel.appendChild(col);
  }

  // ---- show on touch-only devices; a real key press hides them for the visit ----
  function show(on) {
    root.hidden = !on;
    document.documentElement.classList.toggle("arcade-touch", on);
    if (!on) {
      if (stickEl) setDir({ up: false, down: false, left: false, right: false });
      for (const el of btnEls.values()) el.dispatchEvent(new Event("pointercancel"));
    }
  }
  show(arcadeTouchOnly());
  window.addEventListener("keydown", () => { if (!root.hidden) show(false); }, true);

  return {
    show,
    get visible() { return !root.hidden; },
    get dir() { return { ...dir }; },
  };
}
