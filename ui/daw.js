// The DAW screen: tracks on a timeline, per-track effect inserts, a mixer and a
// transport. All state lives in C++ (src/DawBridge.cpp); this file draws the
// "dawSession" it receives and sends edits back through one native function,
// daw (command, args). Every edit is one undo step: drags only send on release.
import * as Juce from "./juce/index.js";
import { CATEGORIES, MODULE_KEYS, MODULES, NOTE_NAMES } from "./modules.js";
import { createActivity, createFxView } from "./fxviews.js";
import { createPresetPicker, presetParams } from "./fxpresets.js";
import { LibraryBrowser } from "./browser.js";
import { PianoRoll } from "./pianoroll.js";
import { TakeLanes } from "./takes.js";
import { TempoMap, createTempoLane, gridImage } from "./tempo.js";
import { createRhythmPanel, drawHitpoints, drawWarpMarkers, savedQuantizeArgs, startWarpDrag } from "./rhythm.js";
import { ARC_END, ARC_START, arcPath, clamp, formatDuration, formatValue, html, IS_MAC, keyText, polar, svg } from "./widgets.js";

const nativeDaw = Juce.getNativeFunction("daw");

const HEAD_WIDTH = 244;           // track header column, px, by default (--head in daw.css); resizable
const MIN_HEAD = 170, MAX_HEAD = 420;
const FOLDED_ROW = 28;             // a folded track's height, px
const MIN_ROW = 44, MAX_ROW = 200; // track height range, px (vertical zoom)
const MIN_ZOOM = 4, MAX_ZOOM = 2000; // pixels per second
const METER_FLOOR_DB = -60;
const FADER_MIN_DB = -60, FADER_MAX_DB = 6;
const GRIDS = [["bar", "Bar"], [1, "Beat"], [0.5, "1/2 beat"], [0.25, "1/4 beat"]];
const SCALE_STEPS = { major: [0, 2, 4, 5, 7, 9, 11], minor: [0, 2, 3, 5, 7, 8, 10] };
// Auto Mix's track roles (the bridge sends the same list with its tags).
const AUTOMIX_ROLES = [
  { key: "voice", name: "Voice" }, { key: "background", name: "Background vocals" }, { key: "instrumental", name: "Instrumental" },
  { key: "guitar", name: "Guitar" }, { key: "piano", name: "Piano / keys" }, { key: "pad", name: "Pad / strings" }, { key: "synth", name: "Synth lead" }, { key: "bass", name: "Bass" }, { key: "drums", name: "Drums" },
  { key: "instrument", name: "Other instrument" }, { key: "other", name: "Leave as is" },
];
const SIGNATURES = [[2, "2/4"], [3, "3/4"], [4, "4/4"], [5, "5/4"], [6, "6/8"], [7, "7/8"], [9, "9/8"], [12, "12/8"]];

// Per-viewer UI preferences (grid, return on stop). The page may have no storage.
const prefs = {
  get(key, fallback) { try { const v = localStorage.getItem(`micfx.daw.${key}`); return v == null ? fallback : JSON.parse(v); } catch { return fallback; } },
  set(key, value) { try { localStorage.setItem(`micfx.daw.${key}`, JSON.stringify(value)); } catch { /* no storage */ } },
};

const TRACK_COLOURS = ["#8c7bff", "#3fd9c6", "#ffb547", "#ff6fb1", "#5aafff", "#9be15d", "#ff8a5c", "#c78bff"];


const PITCH_PARTS = ["tune", "pitch", "harmony"].map((id) => MODULES.find((m) => m.id === id));

let parameterInfo = {};
let toast = () => {};

// Sends a command; shows its error, if any. Resolves with the command's result.
async function daw(command, args = {}) {
  const result = await nativeDaw(command, args);
  if (result && result.ok === false && result.error && !result.cancelled) toast(result.error, "is-error");
  return result;
}

const REDUCED_MOTION = window.matchMedia?.("(prefers-reduced-motion: reduce)") ?? { matches: false };
const monitorMode = (track) => track.monitorMode ?? (track.monitor ? "in" : "off");
const isTyping = (event) => Boolean(event.target.closest?.("input:not([type=range]):not([type=checkbox]), select, textarea, [contenteditable]"));
const trackColour = (track, index) => track.colour || TRACK_COLOURS[(track.id ?? index) % TRACK_COLOURS.length];

// Return tracks: lettered A, B, C... in mixer order, coloured apart from the tracks.
const RETURN_COLOURS = ["#5fd0ff", "#b38cff", "#ffd166", "#7ee0a1"];
const returnLetter = (index) => String.fromCharCode(65 + (index % 26));
const returnColour = (bus, index) => bus.colour || RETURN_COLOURS[index % RETURN_COLOURS.length];
const sendText = (v) => (v <= -59.5 ? "off" : `${v > 0 ? "+" : ""}${v.toFixed(v > -10 ? 1 : 0)}`);

// A track's input: [left] mono, [left, right] stereo, [] none (indices from 0). As text for
// the pickers: "0", "0,1", "".
const inputKey = (input) => (input ?? [0]).join(",");
const inputNumber = (i) => String(i + 1);
const inputText = (input, names = []) => {
  if (!input?.length) return "No input";
  if (input.length === 1) return `In ${inputNumber(input[0])}${names[input[0]] ? ` · ${names[input[0]]}` : ""}`;
  const [l, r] = input;
  return `In ${inputNumber(l)}${r === l + 1 ? "/" : "+"}${inputNumber(r)} · Stereo`;
};
// The same, short enough for a mixer strip: "In 1", "In 1/2 ST", "In 3+2 ST".
const inputShort = (input) => (!input?.length ? "No input" : inputText(input).replace(" · Stereo", " ST"));
// What the strip's picker offers: no input, each input (mono), neighbouring pairs (stereo),
// and the track's own pair when it's two other inputs.
const inputChoices = (names, current) => {
  const count = Math.max(1, names.length);
  const choices = [[], ...Array.from({ length: count }, (_, i) => [i])];
  for (let i = 0; i + 1 < count; i += 2) choices.push([i, i + 1]);
  if (current?.length && !choices.some((c) => inputKey(c) === inputKey(current))) choices.push(current);
  return choices;
};
// What "+ Return…" offers: [module key (addReturn's effect), name]. The effect starts fully wet.
// The offline processes that change a clip's audio (the Audio menu and the clip menu).
const AUDIO_PROCESSES = [
  ["Normalise", "normaliseClip", "Set the clip gain so its loudest peak is −1 dB"],
  ["Strip silence: quiet room (−55 dB)", "stripSilenceQuiet", "Split the clip into phrases and remove the silent gaps between them"],
  ["Strip silence: normal (−45 dB)", "stripSilenceNormal", "Split the clip into phrases and remove the silent gaps between them"],
  ["Strip silence: noisy (−35 dB)", "stripSilenceNoisy", "Split the clip into phrases and remove the silent gaps between them"],
  ["Split into stems…", "splitStems", "Separate the song into vocals, drums, bass and other, each on its own track"],
];
const RETURN_EFFECTS = [["reverb", "Reverb"], ["chorus", "Chorus"], ["echo", "Echo"], ["doubler", "Doubler"],
                        ["flanger", "Flanger"], ["phaser", "Phaser"], ["none", "Empty return"]];
const dbText = (value) => {
  const db = Number(value) || 0;
  return db <= FADER_MIN_DB ? "−∞" : `${db > 0.05 ? "+" : ""}${db.toFixed(1)}`;
};

function button(className, text, title, onClick) {
  const element = html("button", className, text);
  if (title) element.title = keyText(title);
  if (onClick) element.addEventListener("click", onClick);
  return element;
}


// Inline 24x24 stroke icons for the toolbar (no icon library).
const ICONS = {
  new: '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5M12 12v6M9 15h6"/>',
  open: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  recent: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  save: '<path d="M5 3h11l4 4v13a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M8 3v5h7V3M8 21v-7h8v7"/>',
  saveas: '<path d="M5 3h11l4 4v6M5 3a1 1 0 0 0-1 1v16a1 1 0 0 0 1 1h7"/><path d="M8 3v5h7V3M17 15v6M14 18h6"/>',
  import: '<path d="M12 3v12M7 10l5 5 5-5M4 20h16"/>',
  automix: '<path d="M5 4v16M12 4v16M19 4v16"/><circle cx="5" cy="14" r="2" fill="currentColor"/><circle cx="12" cy="8" r="2" fill="currentColor"/><circle cx="19" cy="15" r="2" fill="currentColor"/>',
  export: '<path d="M12 15V3M7 8l5-5 5 5M4 20h16"/>',
  tempo: '<path d="M3 18h5l5-10h8"/><circle cx="8" cy="18" r="1.6" fill="currentColor"/><circle cx="13" cy="8" r="1.6" fill="currentColor"/><path d="M3 21h18" stroke-dasharray="2 3"/>',
  tap: '<path d="M9 11V5a2 2 0 0 1 4 0v6M9 11l-1-1.5a1.8 1.8 0 0 0-3 2L8 18a5 5 0 0 0 4 2h2a5 5 0 0 0 5-5v-3a2 2 0 0 0-4 0M13 11a2 2 0 0 1 4 0"/>',
  key: '<circle cx="8" cy="15" r="4"/><path d="M11 12l9-9M16 7l3 3"/>',
  click: '<path d="M8 21h8l-2-16h-4zM12 17l4-9"/>',
  countin: '<path d="M4 8h3v8M12 8h4v3h-4v5h4M3 3h2M19 3h2"/><path d="M19 8v8"/>',
  loop: '<path d="M17 2l3 3-3 3"/><path d="M4 11V9a4 4 0 0 1 4-4h12M7 22l-3-3 3-3"/><path d="M20 13v2a4 4 0 0 1-4 4H4"/>',
  snap: '<path d="M6 3v8a6 6 0 0 0 12 0V3h-4v8a2 2 0 0 1-4 0V3zM6 7h4M14 7h4"/>',
  returnstop: '<path d="M9 14L4 9l5-5"/><path d="M4 9h11a5 5 0 0 1 0 10h-4"/>',
  follow: '<path d="M4 12h12M12 7l5 5-5 5M20 4v16"/>',
  select: '<path d="M5 3l14 8-6 2-3 6z"/>',
  cut: '<circle cx="6" cy="6" r="3"/><circle cx="6" cy="18" r="3"/><path d="M8.6 7.6L20 18M8.6 16.4L20 6"/>',
  gain: '<path d="M4 9v6h4l5 4V5L8 9zM16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"/>',
  warp: '<path d="M3 12c3-8 6-8 9 0s6 8 9 0"/><path d="M3 20h18" stroke-dasharray="2 3"/>',
  fit: '<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>',
  undo: '<path d="M9 14L4 9l5-5"/><path d="M4 9h10a6 6 0 0 1 0 12h-3"/>',
  redo: '<path d="M15 14l5-5-5-5"/><path d="M20 9H10a6 6 0 0 0 0 12h3"/>',
  tracks: '<path d="M3 6h18M3 12h18M3 18h18"/><rect x="6" y="4.5" width="6" height="3" rx="1" fill="currentColor"/><rect x="12" y="10.5" width="7" height="3" rx="1" fill="currentColor"/><rect x="5" y="16.5" width="5" height="3" rx="1" fill="currentColor"/>',
  mixer: '<path d="M6 3v18M12 3v18M18 3v18"/><rect x="4" y="13" width="4" height="3" rx="1" fill="currentColor"/><rect x="10" y="7" width="4" height="3" rx="1" fill="currentColor"/><rect x="16" y="11" width="4" height="3" rx="1" fill="currentColor"/>',
  piano: '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M9 5v14M15 5v14M7 5v7M12 5v7M17 5v7"/>',
  library: '<path d="M4 4h4v16H4zM10 4h4v16h-4zM15.5 5.2l3.8-1 3.4 15.6-3.8 1z"/>',
  panel: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 14h18"/>',
  more: '<circle cx="5" cy="12" r="1.4" fill="currentColor"/><circle cx="12" cy="12" r="1.4" fill="currentColor"/><circle cx="19" cy="12" r="1.4" fill="currentColor"/>',
  help: '<circle cx="12" cy="12" r="9"/><path d="M9.5 9.5a2.5 2.5 0 1 1 3.5 2.3c-.7.4-1 .9-1 1.7M12 17h.01"/>',
};

function icon(name) {
  return `<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name]}</svg>`;
}

// An icon-only button. The tooltip carries the name and the shortcut.
function iconButton(className, name, title, onClick) {
  const element = button(`${className} ib`, "", title, onClick);
  element.innerHTML = icon(name);
  element.setAttribute("aria-label", title.replace(/\s*\(.*\)\s*$/, "").replace(/[.:].*$/, ""));
  return element;
}

// Thin divider between toolbar groups.
const sep = () => html("span", "tb-sep");

// A pop-up menu at a point (right-click menus, the Project menu, "more"). fill gets
// helpers: item (label, action, { disabled, danger, checked, shortcut, hint, indent }),
// rule (), heading (text) and the menu element itself. One menu is open at a time;
// Escape or a click elsewhere closes it, and clicking its anchor again too.
let openMenu = null;
function closeMenu() {
  if (!openMenu) return;
  const { menu, away, anchor } = openMenu;
  menu.remove();
  document.removeEventListener("pointerdown", away, true);
  document.removeEventListener("keydown", away, true);
  anchor?.setAttribute("aria-expanded", "false");
  openMenu = null;
}
let menuClosedOn = null; // the anchor whose menu a press on it just closed
function popMenu(x, y, fill, anchor = null) {
  if (anchor && menuClosedOn === anchor) { menuClosedOn = null; return null; }
  closeMenu();
  const menu = html("div", "track-menu");
  menu.setAttribute("role", "menu");
  const item = (label, action, { disabled = false, danger = false, checked = null, shortcut = "", hint = "", indent = false } = {}) => {
    const b = html("button", `track-menu-item${danger ? " is-danger" : ""}${indent ? " is-indented" : ""}`);
    b.type = "button";
    b.setAttribute("role", checked === null ? "menuitem" : "menuitemcheckbox");
    if (checked !== null) b.setAttribute("aria-checked", String(Boolean(checked)));
    b.append(html("span", "track-menu-check", checked ? "✓" : ""), html("span", "track-menu-label", label));
    if (shortcut) b.append(html("kbd", "key-chip", keyText(shortcut)));
    if (hint) b.title = hint;
    b.disabled = disabled;
    b.addEventListener("click", () => { closeMenu(); action(); });
    menu.append(b);
    return b;
  };
  const rule = () => menu.append(html("div", "track-menu-rule"));
  const heading = (text) => menu.append(html("div", "track-menu-heading", text));
  fill({ item, rule, heading, menu });
  // A rule at either end, or two together (from optional groups), is dropped.
  [...menu.children].forEach((child, i, all) => {
    if (child.classList.contains("track-menu-rule") && (i === 0 || i === all.length - 1 || all[i + 1]?.classList.contains("track-menu-rule"))) child.remove();
  });
  if (!menu.children.length) return null;

  document.body.append(menu);
  const box = menu.getBoundingClientRect();
  menu.style.left = `${Math.max(4, Math.min(x, innerWidth - box.width - 4))}px`;
  menu.style.top = `${Math.max(4, Math.min(y, innerHeight - box.height - 4))}px`;
  const away = (e) => {
    if (e.type === "keydown") { if (e.key === "Escape") { e.stopPropagation(); closeMenu(); } return; }
    if (menu.contains(e.target)) return;
    if (anchor?.contains(e.target)) menuClosedOn = anchor;
    closeMenu();
    setTimeout(() => { menuClosedOn = null; }, 400);
  };
  openMenu = { menu, away, anchor };
  anchor?.setAttribute("aria-expanded", "true");
  setTimeout(() => {
    if (openMenu?.menu !== menu) return;
    document.addEventListener("pointerdown", away, true);
    document.addEventListener("keydown", away, true);
  });
  menu.querySelector("button:not(:disabled)")?.focus({ preventScroll: true });
  return menu;
}

// Vertical drag helper: calls move(dx, dy, event) and end(moved, event).
function drag(event, move, end = () => {}) {
  event.preventDefault();
  const target = event.currentTarget;
  target.setPointerCapture?.(event.pointerId);
  const x0 = event.clientX, y0 = event.clientY;
  let moved = false;

  const onMove = (e) => {
    if (!moved && Math.hypot(e.clientX - x0, e.clientY - y0) < 3) return;
    moved = true;
    move(e.clientX - x0, e.clientY - y0, e);
  };
  const onUp = (e) => {
    target.removeEventListener("pointermove", onMove);
    target.removeEventListener("pointerup", onUp);
    target.removeEventListener("pointercancel", onUp);
    end(moved, e);
  };

  target.addEventListener("pointermove", onMove);
  target.addEventListener("pointerup", onUp);
  target.addEventListener("pointercancel", onUp);
}

// True when `id` is the only soloed track.
function session_onlySolo(session, id) {
  return (session?.tracks ?? []).every((t) => (t.id === id) === Boolean(t.solo));
}

//==============================================================================
// A rotary control for a value that isn't a host parameter (insert values, pan,
// clip gain). Same look as the Live screen's knobs. onChange (value, first) is
// called while dragging; first is true for the first change of a gesture.
// "45 s", "2 min 10 s": a length of time as it's said, for progress messages.
function spokenDuration(seconds) {
  const total = Math.max(1, Math.round(seconds));
  return total < 60 ? `${total} s` : `${Math.floor(total / 60)} min ${total % 60} s`;
}

//==============================================================================
// Control precision helpers (knobs, faders, sliders, send boxes).

// Scroll-wheel safety: a control takes the wheel only once the mouse has rested on it
// for 300 ms, or while it has focus, so scrolling a panel past it changes nothing.
function wheelReady(element) {
  let since = 0;
  element.addEventListener("pointerenter", () => { since = performance.now(); });
  element.addEventListener("pointerleave", () => { since = 0; });
  return () => document.activeElement === element || (since > 0 && performance.now() - since >= 300);
}

// Joins quick changes to one control (wheel ticks, arrow keys) into one undo step: it
// says "first" only when the last change was over half a second ago.
function stepper() {
  let last = 0;
  return () => {
    const now = performance.now(), first = now - last > 500;
    last = now;
    return first;
  };
}

// The number in typed text ("2.5 dB", "−3", "120ms"), or NaN.
function parseNumber(text) {
  const match = String(text).replace(/[−–]/g, "-").replace(",", ".").match(/-?\d*\.?\d+/);
  return match ? Number(match[0]) : NaN;
}

// Pan typed as "30L", "30 R", "C", "-30" or "0.3".
function parsePan(text) {
  const t = String(text).trim().toUpperCase();
  if (t === "C" || t === "CENTER" || t === "CENTRE") return 0;
  const n = parseNumber(t);
  if (Number.isNaN(n)) return NaN;
  const amount = Math.abs(n) > 1 ? n / 100 : n;
  return clamp(t.endsWith("L") ? -Math.abs(amount) : t.endsWith("R") ? Math.abs(amount) : amount, -1, 1);
}

// A send level typed: "off", "-inf" or a number of dB.
const parseSend = (text) => (/off|inf|∞/i.test(String(text)) ? -100 : parseNumber(text));

// Type a value in place: a small box over `anchor` with the current text selected.
// Enter or leaving it commits, Escape cancels. onCommit gets the text.
function typeValue(anchor, text, onCommit) {
  if (anchor.querySelector(":scope > .value-entry")) return;
  const input = html("input", "value-entry");
  Object.assign(input, { value: text, spellcheck: false });
  input.setAttribute("aria-label", "Type a value");
  let done = false;
  const finish = (commit) => {
    if (done) return;
    done = true;
    const value = input.value;
    input.remove();
    if (commit) onCommit(value);
    (anchor.closest("[tabindex]") ?? anchor).focus?.({ preventScroll: true });
  };
  input.addEventListener("keydown", (event) => {
    event.stopPropagation();
    if (event.key === "Enter") finish(true);
    else if (event.key === "Escape") finish(false);
  });
  input.addEventListener("blur", () => finish(true));
  for (const type of ["pointerdown", "dblclick", "click", "wheel"]) input.addEventListener(type, (event) => event.stopPropagation());
  anchor.append(input);
  input.focus();
  input.select();
}

class ValueKnob {
  constructor({ label, min, max, skew = 1, step = 0, unit = "", value, defaultValue, bipolar = false, format, parse = parseNumber, onChange }) {
    Object.assign(this, { min, max, skew, step, unit, bipolar, onChange, format, parse });
    this.defaultValue = defaultValue ?? value;
    this.value = value;
    this.dragging = false;

    this.arc = svg("path", { class: "value-arc" });
    this.pointer = svg("line", { class: "pointer" });
    const dial = svg("svg", { viewBox: "0 0 100 100" });
    dial.append(svg("path", { class: "track", d: arcPath(ARC_START, ARC_END, 44) }), this.arc,
                svg("circle", { class: "body", cx: 50, cy: 50, r: 32 }), this.pointer);

    this.readout = html("div", "readout");
    this.element = html("div", "knob");
    this.element.append(dial, this.readout, html("div", "name", label));
    this.element.tabIndex = 0;
    this.element.setAttribute("role", "slider");
    this.element.setAttribute("aria-label", label);
    this.element.title = "Drag or scroll · Shift for fine control · Double-click to reset · Double-click the value (or Enter) to type it";
    const wheelOk = wheelReady(this.element), joined = stepper();

    this.readout.addEventListener("dblclick", (event) => {
      event.stopPropagation();
      this.typeValue();
    });
    this.element.addEventListener("pointerdown", (event) => {
      if (event.target.closest(".value-entry")) return;
      this.element.focus();
      let start = this.normalised, first = true;
      this.dragging = true;
      drag(event, (dx, dy, e) => {
        this.set(this.fromNormalised(clamp(start - dy / (e.shiftKey ? 900 : 220))), first);
        first = false;
      }, () => { this.dragging = false; });
    });
    this.element.addEventListener("wheel", (event) => {
      if (!wheelOk()) return; // let the panel scroll
      event.preventDefault();
      this.set(this.fromNormalised(clamp(this.normalised - event.deltaY * (event.shiftKey ? 0.0004 : 0.002))), joined());
    }, { passive: false });
    this.element.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        event.stopPropagation();
        return this.typeValue();
      }
      const size = event.shiftKey ? 0.002 : 0.02;
      const delta = { ArrowUp: size, ArrowRight: size, ArrowDown: -size, ArrowLeft: -size }[event.key];
      if (delta === undefined) return;
      event.preventDefault();
      event.stopPropagation();
      this.set(this.fromNormalised(clamp(this.normalised + delta)), joined());
    });
    this.element.addEventListener("dblclick", () => this.set(this.defaultValue, true));
    this.render();
  }

  typeValue() {
    typeValue(this.readout, this.readout.textContent, (text) => {
      const value = this.parse(text);
      if (Number.isFinite(value)) this.set(clamp(value, this.min, this.max), true);
    });
  }

  get normalised() {
    return this.max === this.min ? 0 : Math.pow(clamp((this.value - this.min) / (this.max - this.min)), this.skew);
  }

  fromNormalised(position) {
    let value = this.min + (this.max - this.min) * Math.pow(position, 1 / this.skew);
    if (this.step > 0) value = Math.round((value - this.min) / this.step) * this.step + this.min;
    return clamp(value, this.min, this.max);
  }

  set(value, first) {
    if (value === this.value) return;
    this.value = value;
    this.render();
    this.onChange(value, first);
  }

  // From the session; ignored mid-drag so the knob doesn't jump under the mouse.
  update(value) {
    if (this.dragging || value === undefined || value === this.value) return;
    this.value = value;
    this.render();
  }

  render() {
    const angle = ARC_START + this.normalised * (ARC_END - ARC_START);
    const from = this.bipolar ? 0 : ARC_START;
    this.arc.setAttribute("d", Math.abs(angle - from) < 0.5 ? "" : arcPath(from, angle, 44));
    const [x1, y1] = polar(angle, 12);
    const [x2, y2] = polar(angle, 25);
    Object.entries({ x1, y1, x2, y2 }).forEach(([name, v]) => this.pointer.setAttribute(name, v));
    const text = this.format ? this.format(this.value) : formatValue(this.value, this.unit);
    this.readout.textContent = text;
    this.element.setAttribute("aria-valuetext", text);
  }
}

//==============================================================================
// A vertical volume fader in dB.
class Fader {
  constructor({ value, onChange }) {
    this.onChange = onChange;
    this.value = value;
    this.dragging = false;
    this.element = html("div", "fader");
    this.element.tabIndex = 0;
    this.element.setAttribute("role", "slider");
    this.element.setAttribute("aria-label", "Volume");
    this.element.title = "Drag to set the volume · Double-click for 0 dB · Enter to type a value";
    const wheelOk = wheelReady(this.element), step = stepper();
    this.thumb = html("div", "fader-thumb");
    this.element.append(html("div", "fader-track"), this.thumb);

    this.element.addEventListener("pointerdown", (event) => {
      const box = this.element.getBoundingClientRect();
      let start = Fader.toPosition(this.value), first = true;
      this.dragging = true;
      drag(event, (dx, dy, e) => {
        this.set(Fader.toDecibels(clamp(start - dy / (box.height * (e.shiftKey ? 4 : 1)))), first);
        first = false;
      }, () => { this.dragging = false; });
    });
    this.element.addEventListener("dblclick", () => this.set(0, true));
    this.element.addEventListener("wheel", (event) => {
      if (!wheelOk()) return;
      event.preventDefault();
      this.set(Fader.toDecibels(clamp(Fader.toPosition(this.value) - event.deltaY * 0.001)), step());
    }, { passive: false });
    this.element.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        event.stopPropagation();
        typeValue(this.element, dbText(this.value), (text) => {
          const db = /inf/i.test(text) ? FADER_MIN_DB : parseNumber(text);
          if (Number.isFinite(db)) this.set(clamp(db, FADER_MIN_DB, FADER_MAX_DB), true);
        });
        return;
      }
      const size = event.shiftKey ? 0.004 : 0.03;
      const delta = { ArrowUp: size, ArrowRight: size, ArrowDown: -size, ArrowLeft: -size }[event.key];
      if (delta === undefined) return;
      event.preventDefault();
      event.stopPropagation();
      this.set(Fader.toDecibels(clamp(Fader.toPosition(this.value) + delta)), step());
    });
    this.render();
  }

  // More travel near unity, where you mix: the dB range is the square root of the position.
  static toPosition(db) { return Math.pow(clamp((db - FADER_MIN_DB) / (FADER_MAX_DB - FADER_MIN_DB)), 2); }
  static toDecibels(position) {
    const db = FADER_MIN_DB + (FADER_MAX_DB - FADER_MIN_DB) * Math.sqrt(position);
    return Math.abs(db) < 0.4 ? 0 : Math.round(db * 10) / 10; // a little detent at 0 dB
  }

  set(db, first) {
    this.value = db;
    this.render();
    this.onChange(db, first);
  }

  update(db) {
    if (this.dragging || db === this.value) return;
    this.value = db;
    this.render();
  }

  render() {
    this.thumb.style.bottom = `calc(${Fader.toPosition(this.value) * 100}% - 7px)`;
    this.element.setAttribute("aria-valuetext", `${dbText(this.value)} dB`);
  }
}

//==============================================================================
// A peak meter with a falling display, horizontal or vertical.
// With a key, a peak at or over 0 dB lights a clip light that stays on (across
// re-renders) until the meter is clicked.
class Level {
  static clipped = new Set();

  constructor(vertical = false, key = null) {
    this.key = key;
    this.element = html("div", vertical ? "level-meter is-vertical" : "level-meter");
    this.fill = html("div", "level-fill");
    this.element.append(this.fill);
    this.shown = METER_FLOOR_DB;
    if (key != null) {
      this.light = html("span", "clip-light");
      this.element.append(this.light);
      this.element.title = "Level. Red light: it went over 0 dB. Click to reset.";
      this.element.addEventListener("pointerdown", (event) => {
        event.stopPropagation();
        Level.clipped.delete(this.key);
        this.element.classList.remove("is-clipped");
      });
      this.element.classList.toggle("is-clipped", Level.clipped.has(key));
    }
  }

  push(db, elapsed) {
    this.shown = Math.max(db ?? METER_FLOOR_DB, this.shown - elapsed * 0.03);
    const amount = clamp((this.shown - METER_FLOOR_DB) / -METER_FLOOR_DB);
    this.fill.style.setProperty("--level", amount);
    this.element.classList.toggle("is-hot", this.shown > -1);
    if (this.key != null && (db ?? -100) >= -0.01 && !Level.clipped.has(this.key)) {
      Level.clipped.add(this.key);
      this.element.classList.add("is-clipped");
    }
  }
}

// Two vertical meters, left and right, for the mixer strips.
class StereoLevel {
  constructor(key) {
    this.element = html("div", "stereo-meter");
    this.left = new Level(true, key == null ? null : `${key}:L`);
    this.right = new Level(true, key == null ? null : `${key}:R`);
    this.element.append(this.left.element, this.right.element);
  }

  push(levels, elapsed) {
    this.left.push(levels?.[0], elapsed);
    this.right.push(levels?.[1], elapsed);
  }
}

//==============================================================================
// A small horizontal slider for the track headers (volume, pan). Click or drag to
// set, double-click to reset. onChange (value, first) like the knobs.
class HSlider {
  constructor({ value, defaultValue, toPosition, fromPosition, format, parse = parseNumber, title, bipolar = false, onChange }) {
    Object.assign(this, { value, defaultValue, toPosition, fromPosition, format, parse, bipolar, onChange });
    this.dragging = false;
    this.element = html("div", bipolar ? "hslider is-bipolar" : "hslider");
    this.element.title = `${title} · Drag to change · Shift for fine · Double-click to type · Ctrl+click to reset`;
    const wheelOk = wheelReady(this.element), step = stepper();
    this.element.setAttribute("role", "slider");
    this.element.setAttribute("aria-label", title);
    this.element.tabIndex = 0;
    this.fill = html("div", "hslider-fill");
    this.text = html("span", "hslider-text");
    this.element.append(this.fill, this.text);

    this.element.addEventListener("pointerdown", (event) => {
      event.stopPropagation();
      if (event.button !== 0 || event.target.closest(".value-entry")) return;
      if (event.ctrlKey || event.metaKey) {
        event.preventDefault();
        return this.set(this.defaultValue, true);
      }
      const box = this.element.getBoundingClientRect();
      const start = this.toPosition(this.value);
      let first = true;
      this.dragging = true;
      drag(event, (dx, dy, e) => {
        this.set(this.fromPosition(clamp(start + dx / (box.width * (e.shiftKey ? 5 : 1)))), first);
        first = false;
      }, (moved, e) => {
        this.dragging = false;
        if (!moved) {
          // A click sets the value there; remembered briefly in case it starts a double-click.
          if (this.beforeClick === undefined) this.beforeClick = this.value;
          clearTimeout(this.clickTimer);
          this.clickTimer = setTimeout(() => { this.beforeClick = undefined; }, 400);
          this.set(this.fromPosition(clamp((e.clientX - box.left) / box.width)), true);
        }
      });
    });
    // A click sets the value where it lands, so the double-click's first click moved it:
    // put it back before typing.
    this.element.addEventListener("dblclick", () => {
      if (this.beforeClick !== undefined) this.set(this.beforeClick, false);
      this.typeValue();
    });
    this.element.addEventListener("wheel", (event) => {
      if (!wheelOk()) return;
      event.preventDefault();
      const delta = (event.deltaX || -event.deltaY) * (event.shiftKey ? 0.0001 : 0.0005);
      this.set(this.fromPosition(clamp(this.toPosition(this.value) + delta)), step());
    }, { passive: false });
    this.element.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        event.stopPropagation();
        return this.typeValue();
      }
      if (event.key === "Delete" || event.key === "Backspace") {
        event.preventDefault();
        event.stopPropagation();
        return this.set(this.defaultValue, true);
      }
      const size = event.shiftKey ? 0.005 : 0.03;
      const delta = { ArrowRight: size, ArrowUp: size, ArrowLeft: -size, ArrowDown: -size }[event.key];
      if (delta === undefined) return;
      event.preventDefault();
      event.stopPropagation();
      this.set(this.fromPosition(clamp(this.toPosition(this.value) + delta)), step());
    });
    this.render();
  }

  set(value, first) {
    if (value === this.value) return;
    this.value = value;
    this.render();
    this.onChange(value, first);
  }

  typeValue() {
    typeValue(this.element, this.text.textContent, (text) => {
      const value = this.parse(text);
      const ends = [this.fromPosition(0), this.fromPosition(1)];
      if (Number.isFinite(value)) this.set(clamp(value, Math.min(...ends), Math.max(...ends)), true);
    });
  }

  update(value) {
    if (this.dragging || value === this.value) return;
    this.value = value;
    this.render();
  }

  render() {
    const position = this.toPosition(this.value);
    if (this.bipolar) {
      const from = Math.min(position, 0.5), to = Math.max(position, 0.5);
      this.fill.style.left = `${from * 100}%`;
      this.fill.style.width = `${(to - from) * 100}%`;
    } else {
      this.fill.style.left = "0";
      this.fill.style.width = `${position * 100}%`;
    }
    this.text.textContent = this.format(this.value);
    this.element.setAttribute("aria-valuetext", this.text.textContent);
  }
}

//==============================================================================
// A send level in a small box: the return's letter and the level. Drag up or down (Shift
// for fine), wheel after resting on it, arrow keys, double-click to type ("off", "-12"),
// Ctrl+click or Delete for off. onChange (dB, first): -100 is off.
class SendBox {
  static MIN = -60;
  static MAX = 6;
  static toPosition(db) { return db <= SendBox.MIN + 0.5 ? 0 : Math.pow(clamp((db - SendBox.MIN) / (SendBox.MAX - SendBox.MIN)), 2.2); }
  static toDecibels(position) {
    if (position <= 0.001) return -100;
    const db = SendBox.MIN + (SendBox.MAX - SendBox.MIN) * Math.pow(clamp(position), 1 / 2.2);
    return Math.abs(db) < 0.3 ? 0 : Math.round(db * 10) / 10;
  }

  constructor({ letter, name, colour, value, onChange }) {
    Object.assign(this, { onChange, value });
    this.dragging = false;
    this.element = html("div", "send-box");
    this.element.style.setProperty("--send-color", colour);
    this.element.tabIndex = 0;
    this.element.setAttribute("role", "slider");
    this.element.setAttribute("aria-label", `Send to ${name}`);
    this.element.title = `Send to ${letter} · ${name}\nDrag up or down · Double-click to type · Ctrl+click: off`;
    this.text = html("span", "send-box-value");
    this.element.append(html("span", "send-box-letter", letter), this.text);
    const wheelOk = wheelReady(this.element), step = stepper();

    this.element.addEventListener("pointerdown", (event) => {
      event.stopPropagation();
      if (event.button !== 0 || event.target.closest(".value-entry")) return;
      this.element.focus();
      if (event.ctrlKey || event.metaKey) { event.preventDefault(); return this.set(-100, true); }
      const start = SendBox.toPosition(this.value);
      let first = true;
      this.dragging = true;
      drag(event, (dx, dy, e) => {
        this.set(SendBox.toDecibels(clamp(start + (dx - dy) / (e.shiftKey ? 600 : 150))), first);
        first = false;
      }, () => { this.dragging = false; });
    });
    this.element.addEventListener("dblclick", (event) => {
      event.stopPropagation();
      typeValue(this.element, this.value <= -59.5 ? "off" : String(this.value), (text) => {
        const db = parseSend(text);
        if (Number.isFinite(db)) this.set(db <= SendBox.MIN ? -100 : clamp(db, SendBox.MIN, SendBox.MAX), true);
      });
    });
    this.element.addEventListener("wheel", (event) => {
      if (!wheelOk()) return;
      event.preventDefault();
      this.set(SendBox.toDecibels(clamp(SendBox.toPosition(this.value) - event.deltaY * (event.shiftKey ? 0.0004 : 0.002))), step());
    }, { passive: false });
    this.element.addEventListener("keydown", (event) => {
      if (event.key === "Delete" || event.key === "Backspace") { event.preventDefault(); event.stopPropagation(); return this.set(-100, true); }
      const size = event.shiftKey ? 0.004 : 0.03;
      const delta = { ArrowUp: size, ArrowRight: size, ArrowDown: -size, ArrowLeft: -size }[event.key];
      if (delta === undefined) return;
      event.preventDefault();
      event.stopPropagation();
      this.set(SendBox.toDecibels(clamp(SendBox.toPosition(this.value) + delta)), step());
    });
    this.render();
  }

  set(db, first) {
    if (db === this.value) return;
    this.value = db;
    this.render();
    this.onChange(db, first);
  }

  update(db) {
    if (this.dragging || db === this.value) return;
    this.value = db;
    this.render();
  }

  render() {
    const on = this.value > -59.5;
    this.text.textContent = on ? sendText(this.value) : "off";
    this.element.classList.toggle("is-on", on);
    this.element.style.setProperty("--level", SendBox.toPosition(this.value));
    this.element.setAttribute("aria-valuetext", on ? `${dbText(this.value)} dB` : "off");
  }
}

const panText = (v) => (Math.abs(v) < 0.01 ? "C" : `${Math.round(Math.abs(v) * 100)}${v < 0 ? "L" : "R"}`);

//==============================================================================
// The effect inserts of one track (or the master): a card per insert, plus "add".
class InsertRack {
  constructor(element, owner) {
    this.element = element;
    this.owner = owner; // { id, inserts }, id 0 = master
    this.cards = new Map();
    this.signature = "";
  }

  update(owner) {
    this.owner = owner;
    const signature = owner.inserts.map((insert) => `${insert.id}:${insert.type}`).join(",");

    if (signature !== this.signature) {
      const grew = this.signature !== "" && owner.inserts.length > this.cards.size;
      this.signature = signature;
      this.build();
      if (grew) {
        // Bring the new effect into view without scrolling the rest of the page.
        const card = this.element.lastElementChild, dock = this.element.closest(".inspector");
        if (card && dock) {
          const right = card.offsetLeft + card.offsetWidth - dock.offsetLeft;
          if (right > dock.scrollLeft + dock.clientWidth) dock.scrollLeft = right - dock.clientWidth + 10;
        }
      }
    }

    for (const insert of owner.inserts) this.cards.get(insert.id)?.update(insert);
  }

  // From "dawStatus": live readings by insert id ({ in, out, spectrum? }, null when off).
  live(readings) {
    // With reduced motion asked for, the effect pictures move a few times a second, not 30.
    if (REDUCED_MOTION.matches) {
      const now = performance.now();
      if (now - (this.lastLive ?? 0) < 250) return;
      this.lastLive = now;
    }
    for (const [id, card] of this.cards) card.live(readings?.[id] ?? null);
  }

  build() {
    this.element.replaceChildren();
    this.cards.clear();

    if (this.owner.inserts.length === 0)
      this.element.append(html("p", "inserts-empty", "No effects yet. The track plays dry; pick one from Add effect."));

    this.owner.inserts.forEach((insert, index) => {
      const card = new InsertCard(insert, index, this.owner.inserts.length);
      this.cards.set(insert.id, card);
      this.element.append(card.element);
      this.enableDrag(card, insert.id);
    });
  }

  // Drag a card by its header to reorder the chain (pointer events, so it works in the WebView).
  enableDrag(card, id) {
    const head = card.element.querySelector(".insert-head");
    head.classList.add("is-draggable");
    head.title = head.title || "Drag to reorder";

    head.addEventListener("pointerdown", (event) => {
      if (event.button !== 0 || event.target.closest("button, input, .switch, [role=switch]")) return;
      const startX = event.clientX;
      const cards = [...this.element.querySelectorAll(":scope > .insert")];
      const from = cards.indexOf(card.element);
      const rects = cards.map((c) => c.getBoundingClientRect());
      let dragging = false, to = from;
      head.setPointerCapture(event.pointerId);

      const place = () => {
        // Cards between the old and new slot slide aside to open the gap.
        const width = rects[from].width + 8;
        cards.forEach((c, i) => {
          if (i === from) return;
          const shift = from < to && i > from && i <= to ? -width : from > to && i < from && i >= to ? width : 0;
          c.style.transform = shift ? `translateX(${shift}px)` : "";
        });
      };
      const move = (e) => {
        const dx = e.clientX - startX;
        if (!dragging) {
          if (Math.abs(dx) < 5) return;
          dragging = true;
          this.element.classList.add("is-reordering");
          card.element.classList.add("is-dragging");
        }
        card.element.style.transform = `translateX(${dx}px)`;
        const centre = rects[from].left + rects[from].width / 2 + dx;
        let target = from;
        rects.forEach((r, i) => { if (i !== from && (i < from ? centre < r.left + r.width / 2 : centre > r.left + r.width / 2)) target = i < from ? Math.min(target, i) : Math.max(target, i); });
        if (target !== to) { to = target; place(); }
      };
      const finish = (e) => {
        head.removeEventListener("pointermove", move);
        head.removeEventListener("pointerup", finish);
        head.removeEventListener("pointercancel", finish);
        try { head.releasePointerCapture(event.pointerId); } catch {}
        if (!dragging) return;
        this.element.classList.remove("is-reordering");
        card.element.classList.remove("is-dragging");
        cards.forEach((c) => { c.style.transform = ""; });
        if (e.type === "pointerup" && to !== from) daw("moveInsert", { id, index: to });
      };
      head.addEventListener("pointermove", move);
      head.addEventListener("pointerup", finish);
      head.addEventListener("pointercancel", finish);
    });
  }
}

class InsertCard {
  constructor(insert, index, count) {
    this.insert = insert;
    this.controls = []; // [{ param, widget }]
    this.switches = []; // [{ param, element }]

    const parts = insert.type === "pitch" ? PITCH_PARTS : MODULES.filter((m) => MODULE_KEYS[m.id] === insert.type).slice(0, 1);
    const main = parts[0];
    const category = CATEGORIES[main?.category] ?? CATEGORIES.cleanup;

    this.element = html("section", ["pitch", "paraEq", "multiband", "masterEq"].includes(insert.type) ? "insert is-wide" : "insert");
    this.element.style.setProperty("--cat", category.color);

    const head = html("header", "insert-head");
    const title = html("span", "insert-name", insert.type === "pitch" ? "Pitch" : main?.name ?? insert.type);
    this.activity = createActivity();
    if (insert.type !== "pitch") head.append(this.addSwitch(main.power, `Switch ${main.name} on or off`));
    head.append(title, this.activity.element,
      button("mini", "↑", "Earlier in the chain", () => daw("moveInsert", { id: insert.id, index: index - 1 })),
      button("mini", "↓", "Later in the chain", () => daw("moveInsert", { id: insert.id, index: index + 1 })),
      button("mini is-danger", "×", "Remove this effect", () => daw("removeInsert", { id: insert.id })));
    head.children[head.children.length - 3].disabled = index === 0;
    head.children[head.children.length - 2].disabled = index === count - 1;

    this.body = html("div", "insert-body");
    this.element.append(head, this.body);

    // A picture of what the effect does; some have handles to drag.
    this.view = insert.type === "pitch" ? null : createFxView(insert.type, {
      get: (param) => this.insert.values[param] ?? parameterInfo[param]?.default ?? 0,
      set: (param, value, first) => {
        this.insert.values[param] = value;
        this.update(this.insert);
        this.send(param, value, first);
      },
      range: (param) => parameterInfo[param],
    });
    if (this.view) this.body.append(this.view.element);

    this.parts = []; // pitch: [{ power, row }], so parts that are off fold away
    for (const part of parts) {
      const row = html("div", "insert-controls");

      if (insert.type === "pitch") {
        const sub = html("div", "insert-sub");
        sub.append(this.addSwitch(part.power, `Switch ${part.name} on or off`), html("span", "", part.name));
        this.body.append(sub);
        this.parts.push({ power: part.power, row });
      }

      for (const control of part.controls) {
        const widget = this.createControl(control);
        if (widget) row.append(widget);
      }
      this.body.append(row);
    }

    this.body.append(this.createFooter(parts));
    this.update(insert);
  }

  // Presets for this effect, and its own input and output gain.
  createFooter(parts) {
    const footer = html("div", "insert-footer");
    const ids = parts.map((part) => part.id);
    const isPitch = this.insert.type === "pitch";
    // The pitch insert's presets also remember which of its parts are on.
    const params = () => [...ids.flatMap((id) => presetParams(id)), ...(isPitch ? parts.map((part) => part.power) : [])];

    const picker = createPresetPicker({
      getNativeFunction: Juce.getNativeFunction,
      ids,
      read: () => Object.fromEntries(params().map((param) => [param, this.insert.values[param] ?? parameterInfo[param]?.default ?? 0])),
      apply: (values, fromId) => {
        // A factory preset for one part of the pitch insert switches that part on.
        if (isPitch && fromId) values[parts.find((part) => part.id === fromId).power] = 1;
        Object.entries(values).forEach(([param, value], i) => {
          this.insert.values[param] = value;
          this.send(param, value, i === 0);
        });
        this.update(this.insert);
      },
      defaultOf: (param) => parameterInfo[param]?.default,
    });

    const gains = html("div", "insert-gains");
    for (const [suffix, label, title] of [["InGain", "In", "Input gain: the level going into this effect"],
                                          ["OutGain", "Out", "Output gain: the level coming out of this effect"]]) {
      const widget = this.createControl({ type: "knob", param: `${this.insert.type}${suffix}`, label, bipolar: true });
      widget.title = `${title}. Drag or scroll · Double-click to reset`;
      gains.append(widget);
    }
    footer.append(picker.element, gains);
    return footer;
  }

  // Animates the picture and the level bars from the signal (see fxviews.js).
  live(reading) {
    this.view?.live(reading);
    const level = this.activity.push(reading);
    this.element.classList.toggle("is-live", Boolean(reading));
    this.element.style.setProperty("--act", level.toFixed(2));
  }

  addSwitch(param, title) {
    const element = button("led-switch", "", title, () => {
      const on = element.getAttribute("aria-pressed") !== "true";
      element.setAttribute("aria-pressed", String(on));
      this.send(param, on ? 1 : 0, true);
    });
    this.switches.push({ param, element });
    return element;
  }

  send(param, value, first) {
    daw("setInsert", { id: this.insert.id, param, value, continue: !first });
  }

  createControl(control) {
    // The autotune key picker and tuner, and the reverb picture, are Live-screen extras.
    if (control.type === "keys") control = { type: "choice", param: control.param, label: "Key" };
    if (control.type !== "knob" && control.type !== "choice" && control.type !== "toggle") return null;

    const info = parameterInfo[control.param] ?? { min: 0, max: 1, skew: 1, step: 0, label: "" };
    const value = this.insert.values[control.param] ?? info.default ?? info.min;

    if (control.type === "choice") {
      const wrapper = html("label", "insert-choice");
      const select = html("select", "dropdown");
      (info.choices ?? []).forEach((choice) => select.append(new Option(choice)));
      select.addEventListener("change", () => this.send(control.param, select.selectedIndex, true));
      wrapper.append(html("span", "name", control.label), select);
      this.controls.push({ param: control.param, widget: { update: (v) => { select.selectedIndex = Math.round(v); } } });
      return wrapper;
    }

    if (control.type === "toggle") {
      const pill = html("button", "pill");
      pill.append(html("span", "switch"), html("span", "", control.label));
      pill.addEventListener("click", () => {
        const on = pill.getAttribute("aria-pressed") !== "true";
        pill.setAttribute("aria-pressed", String(on));
        this.send(control.param, on ? 1 : 0, true);
      });
      this.controls.push({ param: control.param, widget: { update: (v) => pill.setAttribute("aria-pressed", String(v >= 0.5)) } });
      return pill;
    }

    const knob = new ValueKnob({
      label: control.label, min: info.min, max: info.max, skew: info.skew || 1, step: info.step || 0,
      unit: info.label, value, defaultValue: info.default, bipolar: control.bipolar,
      onChange: (v, first) => {
        this.insert.values[control.param] = v;
        this.view?.update();
        this.send(control.param, v, first);
      },
    });
    this.controls.push({ param: control.param, widget: knob });
    return knob.element;
  }

  update(insert) {
    this.insert = insert;
    for (const { param, element } of this.switches)
      element.setAttribute("aria-pressed", String((insert.values[param] ?? 0) >= 0.5));
    for (const { param, widget } of this.controls) widget.update(insert.values[param]);
    this.view?.update();
    for (const { power, row } of this.parts) row.hidden = (insert.values[power] ?? 0) < 0.5;
    this.element.classList.toggle("is-off", !this.switches.some(({ param }) => (insert.values[param] ?? 0) >= 0.5));
  }
}

//==============================================================================
class DawScreen {
  constructor(root) {
    this.root = root;
    this.session = null;
    this.status = { position: 0, length: 0, playing: false, recording: false, tracks: {} };
    this.zoom = 20;            // pixels per second (horizontal zoom)
    this.rowHeight = 84;       // track height, px (vertical zoom): tracks without their own
    this.trackHeights = new Map(); // track id -> its own height, px (dragged at its bottom edge)
    this.folded = new Set();   // track ids folded to one line
    this.headWidth = HEAD_WIDTH; // the track header column, px (dragged at its right edge)
    this.showRouting = false;  // header sections, per project (view state)
    this.showSends = false;
    this.pinnedFolded = false; // the return and master rows folded to one line each
    this.follow = true;        // scroll the view with the playhead while playing
    this.tool = "select";      // "select" | "cut" | "gain"
    this.clipboard = null;     // a copied clip
    this.recLevels = [];       // [seconds, gain] of the input while recording, for the live take
    this.snap = true;
    this.grid = prefs.get("grid", 1);           // snap step in beats, or "bar"
    this.returnOnStop = prefs.get("returnOnStop", false);
    this.playStart = null;     // where play was pressed, for Return on stop
    this.taps = [];            // tap tempo times, ms
    this.view = "arrange";     // or "mixer"
    this.selectedTrack = null; // track id, 0 = master
    this.selectedClip = null;
    this.peaks = new Map();    // file -> { seconds, peaks } | "loading" (whole-file overview)
    this.detail = new Map();   // file -> [{ id, start, end, seconds, min, max }] (zoomed-in stretches)
    this.detailPending = new Set();
    this.detailCount = 0;
    this.meters = new Map();   // track id -> [Level]
    this.dragging = false;
    this.pendingSession = null;
    this.reveal = null;        // { track, clip?, start? } to scroll into view once drawn
    this.trackWaiters = new Map(); // track id -> resolve, see waitForTrack

    this.buildTransport();
    this.buildArrange();
    this.buildMixer();
    this.buildInspector();
    this.buildBanners();
    this.buildAutoMix();
    this.buildStems();
    this.buildLibrary();
    this.buildKeyPanel();

    this.takes = new TakeLanes(this, { daw, toast: (...args) => toast(...args) });
    window.__JUCE__.backend.addEventListener("dawSession", (session) => this.onSession(session));
    window.__JUCE__.backend.addEventListener("dawStatus", (status) => this.onStatus(status));
    window.__JUCE__.backend.addEventListener("dawExport", (progress) => this.onExport(progress));
    window.__JUCE__.backend.addEventListener("dawStems", (event) => this.onStems(event));
    document.addEventListener("keydown", (event) => this.onKey(event));
    document.addEventListener("keydown", (event) => this.onPianoKey(event, true), true); // before the shortcuts
    document.addEventListener("keyup", (event) => this.onPianoKey(event, false), true);
    window.addEventListener("blur", () => this.releasePianoKeys());
    new ResizeObserver(() => this.layoutArrange()).observe(this.arrange);

    let last = performance.now();
    requestAnimationFrame(function frame(now) {
      this.tickMeters(now - last);
      last = now;
      requestAnimationFrame(frame.bind(this));
    }.bind(this));
  }

  get active() { return document.body.classList.contains("is-daw"); }
  get tempo() { return this.session?.tempo ?? 120; }
  get beat() { return 60 / this.tempo; }
  get beatsPerBar() { return this.session?.beatsPerBar ?? 4; }
  get bar() { return this.beat * this.beatsPerBar; }
  get gridBeats() { return this.grid === "bar" ? this.beatsPerBar : this.grid; }

  // The tempo over the song (src/daw/TempoMap): beats <-> seconds through every change.
  get tempoMap() {
    const key = JSON.stringify(this.session?.tempoMap ?? this.tempo);
    if (key !== this.tempoMapKey) {
      this.tempoMapKey = key;
      this.tempoMapCache = new TempoMap(this.session?.tempoMap, this.tempo);
    }
    return this.tempoMapCache;
  }

  // Seconds `beats` beats on from a time, on the tempo map (a beat or bar from there).
  beatsFrom(seconds, beats) {
    const map = this.tempoMap;
    return map.toSeconds(map.toBeats(seconds) + beats) - seconds;
  }
  get markers() { return this.session?.markers ?? []; }

  // Seconds snapped to the grid (unless snap is off or Alt is held).
  snapTime(seconds, event) {
    if (!this.snap || event?.altKey) return Math.max(0, seconds);
    return Math.max(0, this.tempoMap.snap(seconds, this.gridBeats));
  }

  barsBeats(seconds) {
    const beats = Math.max(0, this.tempoMap.toBeats(Math.max(0, seconds)) + 1e-9);
    return `${Math.floor(beats / this.beatsPerBar) + 1}.${Math.floor(beats % this.beatsPerBar) + 1}`;
  }

  // "12" or "12.3" (bar.beat), "1:23.4" or "83.4" (time) -> seconds, or null.
  parsePosition(text) {
    const t = text.trim().toLowerCase();
    let m;
    if ((m = t.match(/^(\d+):(\d+(?:\.\d*)?)$/))) return Number(m[1]) * 60 + Number(m[2]);
    if ((m = t.match(/^(\d+(?:\.\d*)?)\s*s$/))) return Number(m[1]);
    if ((m = t.match(/^(\d+)(?:\.(\d+))?$/))) {
      const bar = Number(m[1]), beat = Number(m[2] ?? 1);
      if (bar < 1 || beat < 1 || beat > this.beatsPerBar) return null;
      return this.tempoMap.toSeconds((bar - 1) * this.beatsPerBar + (beat - 1));
    }
    return null;
  }

  // Click the clock to type where to go.
  editClock() {
    if (this.clockInput) return;
    const input = (this.clockInput = html("input", "clock-input"));
    Object.assign(input, { value: this.barsBeats(this.status.position), spellcheck: false, title: "Bar (12 or 12.3), time (1:23.4) or seconds (83s)" });
    const done = (go) => {
      if (!this.clockInput) return;
      const seconds = go ? this.parsePosition(input.value) : null;
      this.clockInput = null;
      input.remove();
      this.clock.classList.remove("is-editing");
      if (go && seconds == null) toast("Type a bar like 12 or 12.3, or a time like 1:23.4 or 83s");
      else if (seconds != null) this.seek(seconds);
    };
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") { event.preventDefault(); done(true); }
      else if (event.key === "Escape") { event.preventDefault(); done(false); }
      event.stopPropagation();
    });
    input.addEventListener("blur", () => done(false));
    this.clock.classList.add("is-editing");
    this.clock.append(input);
    input.focus();
    input.select();
  }

  tapTempo() {
    const now = performance.now();
    if (this.taps.length && now - this.taps[this.taps.length - 1] > 2000) this.taps = [];
    this.taps.push(now);
    this.taps = this.taps.slice(-8);
    if (this.taps.length < 3) return toast(this.taps.length === 1 ? "Keep tapping on the beat…" : "One more…");
    const gaps = this.taps.slice(1).map((t, i) => t - this.taps[i]);
    const bpm = Math.round(60000 / (gaps.reduce((a, b) => a + b, 0) / gaps.length));
    daw("tempo", { bpm: clamp(bpm, 20, 400), continue: this.taps.length > 3, ...(this.tempoMap.steady ? {} : { at: this.status.position }) });
  }

  //============================================================================ markers
  addMarker() {
    const time = this.snapTime(this.status.position);
    const existing = this.markers.find((m) => Math.abs(m.time - time) < 0.01);
    if (existing) return toast(`"${existing.name}" is already here.`);
    daw("addMarker", { time });
  }

  jumpMarker(step) {
    const now = this.status.position;
    const sorted = [...this.markers].sort((a, b) => a.time - b.time);
    const target = step > 0 ? sorted.find((m) => m.time > now + 0.01)
                            : [...sorted].reverse().find((m) => m.time < now - (this.status.playing ? 0.5 : 0.01));
    if (target) this.seek(target.time);
    else if (step < 0) this.seek(0);
  }

  renderMarkers() {
    this.ruler.querySelectorAll(".marker").forEach((m) => m.remove());
    this.inner.querySelectorAll(".marker-line").forEach((m) => m.remove());
    for (const marker of this.markers) {
      const flag = html("div", "marker");
      flag.style.left = `${marker.time * this.zoom}px`;
      flag.title = `${marker.name}\nClick to go here · Drag to move · Double-click to rename`;
      const name = html("span", "marker-name", marker.name);
      const remove = button("marker-remove", "×", "Delete the marker", (event) => { event.stopPropagation(); daw("removeMarker", { id: marker.id }); });
      remove.addEventListener("pointerdown", (event) => event.stopPropagation());
      flag.append(name, remove);
      flag.addEventListener("pointerdown", (event) => {
        if (event.button !== 0) return;
        event.stopPropagation();
        let time = marker.time;
        drag(event, (dx, dy, e) => {
          time = this.snapTime(marker.time + dx / this.zoom, e);
          flag.style.left = `${time * this.zoom}px`;
          line.style.left = `${this.headWidth + time * this.zoom}px`;
        }, (moved) => {
          if (moved) daw("setMarker", { id: marker.id, time });
          else this.seek(marker.time);
        });
      });
      flag.addEventListener("dblclick", (event) => {
        event.stopPropagation();
        const input = html("input", "marker-input");
        Object.assign(input, { value: marker.name, spellcheck: false });
        let finished = false;
        const finish = (save) => {
          if (finished) return;
          finished = true;
          if (save && input.value.trim() && input.value.trim() !== marker.name) daw("setMarker", { id: marker.id, name: input.value.trim() });
          input.remove();
          name.hidden = false;
        };
        input.addEventListener("keydown", (e) => {
          e.stopPropagation();
          if (e.key === "Enter") finish(true);
          if (e.key === "Escape") finish(false);
        });
        input.addEventListener("pointerdown", (e) => e.stopPropagation());
        input.addEventListener("blur", () => finish(true));
        name.hidden = true;
        flag.append(input);
        input.focus();
        input.select();
      });
      const line = html("div", "marker-line");
      line.style.left = `${this.headWidth + marker.time * this.zoom}px`;
      this.ruler.append(flag);
      this.inner.append(line);
    }
  }

  //============================================================================ transport
  // The toolbar: one row in three zones. Left, the song (tempo, signature, key, click);
  // centre, the transport (larger than the rest); right, how you work (tools, snap,
  // Auto Mix, running jobs) and the view switches. The file actions sit in the header
  // beside the project name, and zoom and Follow in the bar under the tracks.
  buildTransport() {
    const bar = this.root.querySelector(".daw-transport");
    const zone = (className, ...children) => {
      const element = html("div", `tb-zone ${className}`);
      element.append(...children);
      return element;
    };

    //---------------------------------------------------------------- header: project
    this.projectName = document.querySelector(".app-project");
    const project = html("div", "daw-project-actions");
    const projectMenu = button("tool project-menu-button", "Project", "New, open, recent projects, save as and import", (event) => this.openProjectMenu(event.currentTarget));
    projectMenu.setAttribute("aria-haspopup", "menu");
    project.append(projectMenu,
      (this.saveButton = iconButton("tool", "save", "Save the project (Ctrl+S)", () => daw("save"))),
      (this.exportButton = iconButton("tool is-accent", "export", "Export the mix or stems to WAV (Ctrl+Shift+R)", () => this.toggleExportPanel())),
      html("span", "tb-sep"),
      (this.undoButton = iconButton("tool", "undo", "Undo (Ctrl+Z)", () => daw("undo"))),
      (this.redoButton = iconButton("tool", "redo", "Redo (Ctrl+Shift+Z)", () => daw("redo"))));
    document.querySelector(".topbar .brand")?.append(project);

    //---------------------------------------------------------------- left: song
    this.tempoInput = html("input", "tempo-input");
    Object.assign(this.tempoInput, { type: "number", min: 20, max: 400, step: 1, title: "Tempo (BPM)" });
    // With tempo changes, the box shows and sets the tempo at the playhead.
    this.tempoInput.addEventListener("change", () => daw("tempo", { bpm: Number(this.tempoInput.value) || 120,
                                                                    ...(this.tempoMap.steady ? {} : { at: this.status.position }) }));
    const tempo = html("label", "tempo");
    tempo.append(this.tempoInput, html("span", "", "BPM"));
    const tap = iconButton("tool tap-tempo", "tap", "Tap tempo: click on the beat a few times (T)", () => this.tapTempo());

    this.signature = html("select", "dropdown signature");
    this.signature.title = "Time signature (beats in a bar). In x/8 the tempo counts eighth notes.";
    SIGNATURES.forEach(([beats, text]) => this.signature.append(new Option(text, beats)));
    this.signature.addEventListener("change", () => daw("beatsPerBar", { beats: Number(this.signature.value) }));

    this.keyButton = iconButton("tool key-button", "key", "The song's key: detect it, pick it, and set the autotune to it", () => this.toggleKeyPanel());
    this.showTempo = prefs.get("showTempo", false);
    this.tempoButton = iconButton("toggle has-label", "tempo", "Tempo track: tempo changes and ramps over the song", () => {
      this.showTempo = !this.showTempo;
      prefs.set("showTempo", this.showTempo);
      this.renderTransport();
      this.renderArrange(true);
    });
    this.tempoButton.insertAdjacentHTML("beforeend", '<span class="ib-text">Track</span>');
    const songTempo = html("div", "tb-joined");
    songTempo.append(tempo, tap);

    // The click: one switch, and an arrow that opens its volume and the count-in.
    this.clickVolume = html("input", "click-volume");
    Object.assign(this.clickVolume, { type: "range", min: 0, max: 200, step: 1, title: "Click volume" });
    let clickFirst = true;
    this.clickVolume.addEventListener("input", () => {
      daw("metronomeVolume", { volume: Number(this.clickVolume.value) / 100, continue: !clickFirst });
      clickFirst = false;
    });
    this.clickVolume.addEventListener("change", () => { clickFirst = true; });
    this.countIn = html("select", "dropdown count-in");
    this.countIn.title = "Count-in before recording";
    ["No count-in", "1 bar", "2 bars", "4 bars"].forEach((text, i) => this.countIn.append(new Option(text, [0, 1, 2, 4][i])));
    this.countIn.addEventListener("change", () => daw("countIn", { bars: Number(this.countIn.value) }));

    this.metronomeButton = iconButton("toggle", "click", "Click: metronome on / off (C)", () => daw("metronome", { on: !this.session?.metronome }));
    this.clickMore = button("toggle tb-arrow", "", "Click volume and count-in", () => this.toggleClickPopover());
    this.clickMore.innerHTML = '<span class="tb-badge"></span><svg viewBox="0 0 10 10" aria-hidden="true"><path d="M2 3.5l3 3 3-3"/></svg>';
    this.clickMore.setAttribute("aria-haspopup", "dialog");
    this.clickPopover = html("div", "tb-popover");
    this.clickPopover.hidden = true;
    const volumeRow = html("label", "tb-popover-row");
    volumeRow.append(html("span", "", "Click volume"), this.clickVolume);
    const countRow = html("label", "tb-popover-row");
    countRow.append(html("span", "", "Count-in"), this.countIn);
    this.clickPopover.append(volumeRow, countRow);
    const click = html("div", "tb-joined click-group");
    click.append(this.metronomeButton, this.clickMore, this.clickPopover);

    const left = zone("zone-song", songTempo, this.signature, this.keyButton, this.tempoButton, click);

    //---------------------------------------------------------------- centre: transport
    const start = button("transport-button", "", "Back to the start (Enter)", () => daw("transport", { action: "stop" }));
    this.playButton = button("transport-button play-button", "", "Play / pause (Space). Right-click: return on stop", () => this.togglePlay());
    this.playButton.addEventListener("contextmenu", (event) => {
      event.preventDefault();
      popMenu(event.clientX, event.clientY, ({ item }) => {
        item("Return on stop", () => this.setReturnOnStop(!this.returnOnStop), { checked: this.returnOnStop,
          hint: "Pausing goes back to where you pressed play" });
      });
    });
    this.recordButton = button("record-button", "", "Record on the armed tracks (R)", () => this.toggleRecord());
    start.innerHTML = `<svg viewBox="0 0 16 16" aria-hidden="true"><rect x="3" y="3" width="2" height="10" rx="0.5"/><path d="M13 3.5v9a.5.5 0 0 1-.8.4L6.3 8.4a.5.5 0 0 1 0-.8l5.9-4.5a.5.5 0 0 1 .8.4z"/></svg>`;
    this.playButton.innerHTML = `<svg class="icon-play" viewBox="0 0 16 16" aria-hidden="true"><path d="M4 2.8v10.4a.6.6 0 0 0 .9.5l8.2-5.2a.6.6 0 0 0 0-1L4.9 2.3a.6.6 0 0 0-.9.5z"/></svg><svg class="icon-pause" viewBox="0 0 16 16" aria-hidden="true"><rect x="3.5" y="2.5" width="3" height="11" rx="0.8"/><rect x="9.5" y="2.5" width="3" height="11" rx="0.8"/></svg><span class="return-dot" title="Return on stop is on"></span>`;
    this.recordButton.innerHTML = `<span class="record-dot"></span><span class="record-label">Rec</span>`;
    this.recordButton.setAttribute("aria-label", "Record");
    this.playButton.setAttribute("aria-label", "Play");

    const clock = (this.clock = html("div", "daw-clock"));
    clock.title = "Click to type a bar or time to go to";
    clock.addEventListener("click", (event) => { if (event.target === clock || !event.target.closest("input")) this.editClock(); });
    clock.append((this.clockBars = html("span", "clock-bars", "1.1")), (this.clockTime = html("span", "clock-time", "0:00.0")));
    this.loopButton = iconButton("toggle", "loop", "Loop the marked range (L). Drag on the ruler to mark it.", () => this.toggleLoop());

    // The mic meter next to Record: "am I too loud to record?"
    this.inputMeter = new Level(false, "input");
    this.inputReadout = html("span", "mic-readout", "−∞");
    const mic = html("div", "mic-meter");
    mic.title = "Mic input level. Aim for peaks around −12 dB; red means it went over 0 dB (click to reset).";
    mic.append(html("span", "mic-label", "Mic"), this.inputMeter.element, this.inputReadout);

    const buttons = html("div", "daw-transport-buttons");
    buttons.append(start, this.playButton, this.recordButton);
    const centre = zone("zone-transport", buttons, clock, this.loopButton, mic);

    //---------------------------------------------------------------- right: tools, features, view
    this.tools = html("div", "segmented tools");
    for (const [tool, text, title] of [["select", "Select", "Select tool: move, trim and fade clips (V)"],
                                       ["cut", "Cut", "Cut tool: click a clip to split it there (B)"],
                                       ["gain", "Gain", "Gain tool: drag a clip up or down to change its gain (G)"],
                                       ["warp", "Warp", "Warp tool: drag inside a clip to stretch its timing; double-click a marker to remove it (W)"]]) {
      const b = iconButton("", tool, title, () => this.setTool(tool));
      b.dataset.tool = tool;
      this.tools.append(b);
    }

    // Snap and its grid as one split control: they are editing behaviour, like the tools.
    this.snapButton = iconButton("toggle", "snap", "Snap edits to the grid (hold Alt to drag freely)", () => {
      this.snap = !this.snap;
      this.renderTransport();
      this.rememberView();
    });
    this.gridSelect = html("select", "dropdown grid-size");
    this.gridSelect.title = "Snap grid";
    GRIDS.forEach(([value, text]) => this.gridSelect.append(new Option(text, value)));
    this.gridSelect.addEventListener("change", () => {
      this.grid = this.gridSelect.value === "bar" ? "bar" : Number(this.gridSelect.value);
      prefs.set("grid", this.grid);
      this.rememberView();
    });
    const snap = html("div", "tb-joined snap-group");
    snap.append(this.snapButton, this.gridSelect);

    this.autoMixButton = iconButton("tool has-label automix-button", "automix", "Auto Mix: listen to the voice and backing and set EQ, compression and levels", () => this.toggleAutoMix());
    this.autoMixButton.insertAdjacentHTML("beforeend", '<span class="ib-text">Auto Mix</span>');
    this.buildActivity();

    this.viewTabs = html("div", "segmented view-tabs");
    for (const view of ["arrange", "mixer"]) {
      const tab = iconButton("", view === "arrange" ? "tracks" : "mixer", view === "arrange" ? "Tracks and clips (Tab)" : "Mixer (Tab / X)", () => this.setView(view));
      tab.dataset.view = view;
      this.viewTabs.append(tab);
    }
    this.libraryButton = iconButton("toggle", "library", "Show / hide the library (Shift+B)", () => this.library.toggle());
    this.dockButton = iconButton("toggle", "panel", "Show / hide the bottom panel (E)", () => this.run("toggleDock"));
    this.pianoButton = iconButton("tool", "piano", "Play MIDI from the computer keyboard (M)", () => this.togglePiano());
    const help = iconButton("tool", "help", "Keyboard shortcuts (?)", () => this.toggleShortcuts());
    this.moreButton = iconButton("tool tb-more", "more", "More", (event) => this.openMoreMenu(event.currentTarget));
    this.moreButton.hidden = true;

    const right = zone("zone-work", this.tools, snap, this.autoMixButton, this.activity.button,
                      html("span", "tb-sep"), this.libraryButton, this.viewTabs, this.dockButton, this.pianoButton, help, this.moreButton);
    bar.append(left, html("span", "tb-spacer"), centre, html("span", "tb-spacer"), right);

    // Under the tracks, at the right of the bar above the bottom panel: the timeline's view.
    this.followButton = iconButton("toggle", "follow", "Follow: scroll with the playhead while playing (F). Scrolling yourself switches it off.", () => this.setFollow(!this.follow));
    const timeline = html("div", "timeline-tools");
    const slider = (label, title, onInput) => {
      const wrapper = html("label", "zoom-slider");
      wrapper.title = title;
      const input = html("input");
      Object.assign(input, { type: "range", min: 0, max: 1000, step: 1 });
      input.addEventListener("input", () => onInput(Number(input.value) / 1000));
      wrapper.append(html("span", "", label), input);
      return [wrapper, input];
    };
    // Logarithmic, so each step of the slider zooms by the same factor.
    const [zoomWrap, zoomInput] = slider("↔", "Horizontal zoom (+ / − or Ctrl+scroll)", (p) => this.setZoom(MIN_ZOOM * Math.pow(MAX_ZOOM / MIN_ZOOM, p), true));
    const [heightWrap, heightInput] = slider("↕", "Track height (Shift+↑ / Shift+↓ or Alt+scroll)", (p) => this.setRowHeight(MIN_ROW + (MAX_ROW - MIN_ROW) * p));
    this.zoomSlider = zoomInput;
    this.heightSlider = heightInput;
    timeline.append(this.followButton, iconButton("tool fit-button", "fit", "Zoom to fit the whole song (Shift+Z)", () => this.run("zoomFit")), zoomWrap, heightWrap);
    this.timelineTools = timeline;
    const splitter = this.root.querySelector(".dock-splitter");
    splitter.insertBefore(timeline, splitter.querySelector(".dock-toggle"));

    // The controls that fold into the "more" menu as the window narrows, least used first.
    this.foldable = [
      { level: 2, element: help, label: "Keyboard shortcuts", run: () => this.toggleShortcuts() },
      { level: 2, element: this.pianoButton, label: "Computer keyboard piano", run: () => this.togglePiano(), checked: () => Boolean(this.piano?.on) },
      { level: 3, element: this.tempoButton, label: "Tempo track", run: () => this.tempoButton.click(), checked: () => this.showTempo },
      { level: 3, element: tap, label: "Tap tempo", run: () => this.tapTempo() },
      { level: 3, element: this.keyButton, label: "Song key…", run: () => this.toggleKeyPanel() },
      { level: 4, element: this.libraryButton, label: "Library", run: () => this.library.toggle(), checked: () => !this.library.collapsed },
      { level: 4, element: this.dockButton, label: "Bottom panel", run: () => this.run("toggleDock"), checked: () => !this.root.classList.contains("dock-hidden") },
      { level: 5, element: this.autoMixButton, label: "Auto Mix…", run: () => this.toggleAutoMix() },
    ];
    this.foldLevel = 0;
    // Only a change of width matters (folding changes the height when it wraps).
    let foldedAt = -1;
    new ResizeObserver(() => {
      if (bar.clientWidth === foldedAt) return;
      foldedAt = bar.clientWidth;
      requestAnimationFrame(() => this.foldToolbar());
    }).observe(bar);

    document.addEventListener("pointerdown", (event) => {
      if (!this.clickPopover.hidden && !click.contains(event.target)) this.toggleClickPopover(false);
    }, true);
  }

  toggleClickPopover(show = this.clickPopover.hidden) {
    this.clickPopover.hidden = !show;
    this.clickMore.setAttribute("aria-expanded", String(show));
  }

  setReturnOnStop(on) {
    this.returnOnStop = on;
    prefs.set("returnOnStop", on);
    this.renderTransport();
  }

  // New, Open, the recent projects, Save as and Import (Save and Export stay as buttons).
  openProjectMenu(anchor) {
    const box = anchor.getBoundingClientRect();
    const s = this.session;
    popMenu(box.left, box.bottom + 4, ({ item, rule, heading }) => {
      item("New project", () => daw("new"), { shortcut: "Ctrl+N" });
      item("Open…", () => daw("open"), { shortcut: "Ctrl+O" });
      const recent = (s?.recent ?? []).filter((r) => r.path !== s.project.path);
      if (recent.length) {
        heading("Recent");
        for (const r of recent.slice(0, 8)) item(r.name, () => daw("openRecent", { path: r.path }), { hint: r.path, indent: true });
      }
      rule();
      item("Save", () => daw("save"), { shortcut: "Ctrl+S" });
      item("Save as…", () => daw("saveAs"), { shortcut: "Ctrl+Shift+S" });
      rule();
      item("Import audio…", () => this.importAudio(), { shortcut: "Ctrl+I" });
      item("Export…", () => this.toggleExportPanel(true), { shortcut: "Ctrl+Shift+R" });
    }, anchor);
  }

  // Narrow windows: fold the least used controls into the "more" menu, one level at a
  // time, until the toolbar fits on one row (level 1 drops the text labels first).
  foldToolbar() {
    const bar = this.root.querySelector(".daw-transport");
    if (!bar.clientWidth) return;
    const apply = (level) => {
      this.foldLevel = level;
      bar.dataset.fold = String(level);
      for (const f of this.foldable) f.element.classList.toggle("is-folded", level >= f.level);
      this.moreButton.hidden = !this.foldable.some((f) => level >= f.level);
    };
    let level = 0;
    bar.classList.remove("is-wrapped");
    apply(0);
    while (bar.scrollWidth > bar.clientWidth + 1 && level < 6) apply(++level);
    // Still too narrow: let the zones wrap onto a second row.
    bar.classList.toggle("is-wrapped", bar.scrollWidth > bar.clientWidth + 1);
  }

  openMoreMenu(anchor) {
    const box = anchor.getBoundingClientRect();
    popMenu(box.right - 200, box.bottom + 4, ({ item }) => {
      for (const f of this.foldable)
        if (this.foldLevel >= f.level) item(f.label, f.run, { checked: f.checked?.() ?? false });
    }, anchor);
  }

  renderTransport() {
    const s = this.session;
    if (!s) return;

    this.saveButton.classList.toggle("is-dirty", s.project.dirty || !s.project.open);
    this.showTempoValue();
    this.countIn.value = String(s.countIn);
    const badge = this.clickMore.querySelector(".tb-badge");
    badge.textContent = s.countIn > 0 ? String(s.countIn) : "";
    this.clickMore.title = `Click volume and count-in${s.countIn > 0 ? ` (count-in: ${s.countIn} bar${s.countIn > 1 ? "s" : ""})` : ""}`;
    this.signature.value = String(s.beatsPerBar ?? 4);
    const songKey = s.key?.key ?? -1;
    this.keyButton.innerHTML = icon("key") + (songKey >= 0 ? `<span class="ib-label">${NOTE_NAMES[songKey]}${s.key.minor ? "m" : ""}</span>` : "");
    this.keyButton.classList.toggle("is-set", songKey >= 0);
    this.renderKeyPanel();
    if (document.activeElement !== this.clickVolume) this.clickVolume.value = String(Math.round((s.metronomeVolume ?? 1) * 100));
    this.gridSelect.value = String(this.grid);
    this.gridSelect.disabled = !this.snap;
    this.playButton.classList.toggle("returns-on-stop", this.returnOnStop);
    this.metronomeButton.setAttribute("aria-pressed", String(s.metronome));
    this.loopButton.setAttribute("aria-pressed", String(s.loop.on));
    this.snapButton.setAttribute("aria-pressed", String(this.snap));
    this.followButton.setAttribute("aria-pressed", String(this.follow));
    this.tempoButton.setAttribute("aria-pressed", String(!!this.showTempo));
    this.libraryButton.setAttribute("aria-pressed", String(!this.library?.collapsed));
    this.dockButton.setAttribute("aria-pressed", String(!this.root.classList.contains("dock-hidden")));
    [...this.tools.children].forEach((b) => b.classList.toggle("is-selected", b.dataset.tool === this.tool));
    if (document.activeElement !== this.zoomSlider)
      this.zoomSlider.value = String(Math.round(1000 * Math.log(this.zoom / MIN_ZOOM) / Math.log(MAX_ZOOM / MIN_ZOOM)));
    if (document.activeElement !== this.heightSlider)
      this.heightSlider.value = String(Math.round(1000 * (this.rowHeight - MIN_ROW) / (MAX_ROW - MIN_ROW)));
    // Named undo: the button says what Ctrl+Z will do.
    const what = (name) => (name ? ` ${name.charAt(0).toLowerCase()}${name.slice(1)}` : "");
    this.undoButton.disabled = !s.canUndo;
    this.redoButton.disabled = !s.canRedo;
    this.undoButton.title = s.canUndo ? `Undo${what(s.undoName)} (Ctrl+Z)` : "Nothing to undo";
    this.redoButton.title = s.canRedo ? `Redo${what(s.redoName)} (Ctrl+Shift+Z)` : "Nothing to redo";
    this.undoButton.setAttribute("aria-label", this.undoButton.title.replace(/ \(.*\)$/, ""));
    this.redoButton.setAttribute("aria-label", this.redoButton.title.replace(/ \(.*\)$/, ""));
    [...this.viewTabs.children].forEach((tab) => tab.classList.toggle("is-selected", tab.dataset.view === this.view));
    this.timelineTools.hidden = this.view !== "arrange";
  }

  //============================================================================ activity
  // One place for the long jobs (export, stems, Auto Mix, key detection, plug-in scan):
  // a spinner with a count at the right of the toolbar, opening a list with progress,
  // a way back to each job's panel, and cancel.
  buildActivity() {
    const jobs = new Map(); // id -> { label, progress, cancel, open }
    const element = button("tool activity-button", "", "Running jobs", () => this.toggleActivity());
    element.innerHTML = '<span class="activity-spinner" aria-hidden="true"></span><span class="activity-count">0</span>';
    element.hidden = true;
    element.setAttribute("aria-haspopup", "dialog");
    const list = html("div", "tb-popover activity-list");
    list.hidden = true;
    const wrap = html("div", "activity");
    wrap.append(element, list);
    this.activity = { jobs, button: wrap, element, list };
    document.addEventListener("pointerdown", (event) => { if (!list.hidden && !wrap.contains(event.target)) list.hidden = true; }, true);
  }

  setJob(id, job) {
    this.activity.jobs.set(id, { ...this.activity.jobs.get(id), ...job });
    this.renderActivity();
  }

  endJob(id) {
    if (this.activity.jobs.delete(id)) this.renderActivity();
  }

  toggleActivity(show = this.activity.list.hidden) {
    this.activity.list.hidden = !show || this.activity.jobs.size === 0;
    this.renderActivity();
  }

  renderActivity() {
    const { jobs, element, list } = this.activity;
    element.hidden = jobs.size === 0;
    element.querySelector(".activity-count").textContent = String(jobs.size);
    element.title = jobs.size ? `Running: ${[...jobs.values()].map((j) => j.label).join(", ")}` : "Running jobs";
    if (jobs.size === 0) list.hidden = true;
    if (list.hidden) return;
    list.replaceChildren(...[...jobs.entries()].map(([id, job]) => {
      const row = html("div", "activity-job");
      const name = button("activity-name", job.label, "Show it", () => { list.hidden = true; job.open?.(); });
      const bar = html("div", "export-bar");
      bar.append(html("div"));
      bar.style.setProperty("--progress", job.progress ?? 0);
      bar.classList.toggle("is-indeterminate", job.progress == null);
      row.append(name, bar);
      if (job.cancel) row.append(button("mini activity-cancel", "×", `Cancel ${job.label.toLowerCase()}`, () => job.cancel()));
      row.dataset.job = id;
      return row;
    }));
  }

  togglePlay() {
    if (this.status.playing) {
      daw("transport", { action: "pause" });
      if (this.returnOnStop && this.playStart != null && !this.status.recording) this.seek(this.playStart);
    } else {
      this.playStart = this.status.position;
      daw("transport", { action: "play" });
    }
  }

  async toggleRecord() {
    if (this.status.recording) {
      const take = await daw("record", { on: false });
      if (take?.dropped) toast("Some audio was dropped while recording. Try a larger buffer size.", "is-error");
    } else {
      await daw("record", { on: true });
    }
  }

  toggleLoop() {
    const loop = this.session.loop;
    if (loop.end > loop.start) daw("loop", { on: !loop.on, start: loop.start, end: loop.end });
    else {
      // No range yet: loop the next 4 bars from the playhead.
      const start = this.snapTime(this.status.position);
      daw("loop", { on: true, start, end: start + this.beatsFrom(start, 4 * this.beatsPerBar) });
    }
  }

  //============================================================================ arrange
  buildArrange() {
    this.arrange = this.root.querySelector(".arrange");
    this.inner = html("div", "arrange-inner");

    const rulerRow = html("div", "ruler-row");
    const corner = html("div", "corner");
    this.clearButton = button("tool clear-mute-solo", "Clear M/S", "Turn every mute and solo off", () => this.run("clearMuteSolo"));
    // Which header sections every track shows (kept per project), and the column's width.
    const section = (text, title, key) => {
      const b = button("toggle head-switch", text, title, () => {
        this[key] = !this[key];
        b.setAttribute("aria-pressed", String(this[key]));
        this.rememberView();
        this.renderArrange();
      });
      b.dataset.key = key;
      return b;
    };
    this.headSwitches = [section("I/O", "Routing: show each track's input, monitoring and output", "showRouting"),
                         section("Sends", "Sends: show each track's send levels to the returns", "showSends")];
    const resizer = html("div", "head-resizer");
    resizer.title = "Drag to widen or narrow the track headers · Double-click: default width";
    resizer.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      event.stopPropagation();
      const start = this.headWidth;
      drag(event, (dx) => this.setHeadWidth(start + dx), () => this.rememberView());
    });
    resizer.addEventListener("dblclick", () => { this.setHeadWidth(HEAD_WIDTH); this.rememberView(); });
    corner.append(button("tool", "+ Track", "Add an audio track (Ctrl+T)", () => this.addTrack()),
                  button("tool", "+ MIDI", "Add a MIDI track with a built-in synth (Ctrl+Shift+T). Pick another instrument in the library.", () => this.addMidiTrack()),
                  this.clearButton, html("span", "corner-gap"), ...this.headSwitches, resizer);
    this.ruler = html("div", "ruler");
    this.ruler.title = "Click to move the playhead · Drag to mark a loop";
    this.loopBand = html("div", "loop-band");
    this.ruler.append(this.loopBand);
    rulerRow.append(corner, this.ruler);

    // The tempo track, under the ruler (shown with the Tempo button).
    this.tempoRow = html("div", "tempo-row");
    const tempoCorner = html("div", "corner tempo-corner");
    tempoCorner.append(html("span", "tempo-corner-name", "Tempo"),
                       button("tool", "Steady", "Remove every tempo change: the whole song at the start tempo",
                              () => daw("tempoMap", { points: [[0, this.tempo, false]] })));
    this.tempoLane = createTempoLane(this, daw);
    this.tempoRow.append(tempoCorner, this.tempoLane.element);
    this.tempoRow.hidden = true;

    this.rows = html("div", "track-rows");
    // The returns and the master, pinned to the bottom of the view (sticky).
    this.pinned = html("div", "pinned-rows");
    this.returnRows = html("div", "return-rows");
    this.masterRow = html("div", "master-slot");
    this.pinned.append(this.returnRows, this.masterRow);
    this.loopShade = html("div", "loop-shade");
    this.playhead = html("div", "daw-playhead");
    this.emptyHint = html("div", "arrange-empty");
    this.emptyHint.innerHTML = "Arm a track and press <b>R</b> to record, or <b>Import…</b> a file.";

    this.cutGuide = html("div", "cut-guide");
    this.cutGuide.hidden = true;
    this.inner.append(rulerRow, this.tempoRow, this.rows, this.pinned, this.loopShade, this.playhead, this.cutGuide);

    // Scrolling by hand while playing switches Follow off, so you can look around
    // (and edit) elsewhere while the song keeps playing.
    this.arrange.addEventListener("scroll", () => {
      if (!this.wavesQueued) {
        this.wavesQueued = true;
        requestAnimationFrame(() => { this.wavesQueued = false; this.redrawWaves(); });
      }
      this.rememberView();
      if (performance.now() - this.autoScrollTime < 150) return;
      if (this.status.playing && this.follow) this.setFollow(false);
    });
    this.autoScrollTime = 0;
    this.arrange.append(this.inner, this.emptyHint);

    this.ruler.addEventListener("pointerdown", (event) => {
      const box = this.ruler.getBoundingClientRect();
      const at = (clientX) => (clientX - box.left) / this.zoom;
      const from = this.snapTime(at(event.clientX), event);
      drag(event, (dx, dy, e) => {
        const to = this.snapTime(at(e.clientX), e);
        this.showLoop({ on: true, start: Math.min(from, to), end: Math.max(from, to) });
      }, (moved, e) => {
        if (!moved) return daw("transport", { action: "seek", time: Math.max(0, at(e.clientX)) });
        const to = this.snapTime(at(e.clientX), e);
        if (Math.abs(to - from) > 0.05) daw("loop", { on: true, start: Math.min(from, to), end: Math.max(from, to) });
        else this.showLoop(this.session.loop);
      });
    });

    this.arrange.addEventListener("wheel", (event) => {
      if (event.altKey) { // Alt+scroll: track height
        event.preventDefault();
        return this.setRowHeight(this.rowHeight * Math.exp(-event.deltaY * 0.002));
      }
      if (!event.ctrlKey && !event.metaKey) return;
      event.preventDefault();
      const box = this.arrange.getBoundingClientRect();
      const anchor = event.clientX - box.left - this.headWidth;
      const seconds = (this.arrange.scrollLeft + anchor) / this.zoom;
      this.beginZoom();
      this.zoom = clamp(this.zoom * Math.exp(-event.deltaY * 0.002), MIN_ZOOM, MAX_ZOOM);
      this.renderArrange(true);
      this.scrollTo(seconds * this.zoom - anchor);
      this.renderTransport();
    }, { passive: false });

    // The cut tool shows where a click will split.
    this.arrange.addEventListener("pointermove", (event) => {
      const clip = this.tool === "cut" && event.target.closest?.(".clip");
      this.cutGuide.hidden = !clip;
      if (!clip) return;
      const box = this.inner.getBoundingClientRect();
      const time = this.snapTime((event.clientX - box.left - this.headWidth) / this.zoom, event);
      const clipBox = clip.getBoundingClientRect();
      this.cutGuide.style.left = `${this.headWidth + time * this.zoom}px`;
      this.cutGuide.style.top = `${clipBox.top - box.top}px`;
      this.cutGuide.style.height = `${clipBox.height}px`;
    });
    this.arrange.addEventListener("pointerleave", () => { this.cutGuide.hidden = true; });
  }

  // Keeps the playhead (or the left edge, if it's off screen) in place while zooming.
  setZoom(zoom, fromSlider = false) {
    const before = this.zoom;
    this.beginZoom();
    this.zoom = clamp(zoom, MIN_ZOOM, MAX_ZOOM);
    this.rememberView();
    const visible = this.arrange.clientWidth - this.headWidth;
    const playheadX = this.status.position * before - this.arrange.scrollLeft;
    const anchor = playheadX >= 0 && playheadX <= visible ? playheadX : 0;
    const seconds = (this.arrange.scrollLeft + anchor) / before;
    this.renderArrange(true);
    this.scrollTo(seconds * this.zoom - anchor);
    if (!fromSlider) this.renderTransport();
  }

  // While zooming, waveforms are not redrawn on every step (that is slow at high zoom,
  // where each is a fine-detail picture): the previous picture is kept and stretched
  // to the new scale, and one sharp redraw follows when the zooming stops.
  beginZoom() {
    this.zooming = true;
    clearTimeout(this.zoomTimer);
    this.zoomTimer = setTimeout(() => { this.zooming = false; this.redrawWaves(); }, 160);
  }

  setHeadWidth(width) {
    this.headWidth = Math.round(clamp(width, MIN_HEAD, MAX_HEAD));
    this.root.style.setProperty("--head", `${this.headWidth}px`);
    this.renderArrange();
  }

  setRowHeight(height) {
    this.rowHeight = Math.round(clamp(height, MIN_ROW, MAX_ROW));
    this.rememberView();
    this.renderArrange(true);
    this.renderTransport();
  }

  setFollow(on) {
    this.follow = on;
    this.renderTransport();
    this.rememberView();
    if (on) this.movePlayhead(true);
  }

  setTool(tool) {
    const hitpoints = (tool === "warp") !== (this.tool === "warp"); // the warp tool shows note starts
    this.tool = tool;
    if (hitpoints && this.session) this.renderArrange(true);
    this.arrange.dataset.tool = tool;
    this.cutGuide.hidden = true;
    this.renderTransport();
  }

  scrollTo(left) {
    this.autoScrollTime = performance.now();
    this.arrange.scrollLeft = Math.max(0, left);
  }

  // Seconds the timeline shows: the song plus room to record after it.
  timelineSeconds() {
    let end = this.status.length || 0;
    for (const track of this.session?.tracks ?? [])
      for (const clip of track.clips) end = Math.max(end, clip.start + clip.length);
    const visible = Math.max(0, this.arrange.clientWidth - this.headWidth) / this.zoom;
    return Math.max(end + 30, this.status.position + 10, visible, 60);
  }

  async addTrack() {
    const id = await daw("addTrack");
    if (typeof id === "number") this.showWhenDrawn({ track: id });
  }

  async addMidiTrack() {
    const id = await daw("addMidiTrack");
    if (typeof id === "number") this.showWhenDrawn({ track: id });
  }

  // An empty MIDI clip (one bar) on a MIDI track at a time, opened in the piano roll.
  async addMidiClip(track, start) {
    const id = await daw("addMidiClip", { track: track.id, start: Math.max(0, start), length: this.beatsFrom(Math.max(0, start), this.beatsPerBar) });
    if (typeof id === "number" && id > 0) {
      this.showWhenDrawn({ track: track.id, clip: id });
      this.setDockTab("track");
      this.setDock(true);
    }
  }

  // Import an audio file at the playhead, on the selected track (or a new one).
  async importAudio() {
    const result = await daw("import", { track: this.selectedTrack ?? -1 });
    if (result?.ok && typeof result.track === "number") this.showWhenDrawn({ track: result.track, clip: result.clip, start: result.start });
  }

  // Selects a track (and clip) that was just added, once a session that holds it arrives.
  // The command's reply comes before that session: selecting at once would find no such
  // track yet and fall back to the first one.
  showWhenDrawn(reveal) {
    this.reveal = reveal;
    if (this.session?.tracks.some((t) => t.id === reveal.track)) this.onSession(this.session);
  }

  // Brings a track (and clip) that was just added into view once the session that holds
  // it has been drawn: below a few rows, a new track would otherwise land out of sight.
  revealPending() {
    const reveal = this.reveal;
    const row = reveal && this.rows.querySelector(`.track-row[data-track="${reveal.track}"]`);
    if (!row) return;
    this.reveal = null;
    const arrange = this.arrange.getBoundingClientRect(), box = row.getBoundingClientRect();
    if (box.top < arrange.top + 30 || box.bottom > arrange.bottom) row.scrollIntoView({ block: "nearest" });
    if (reveal.start !== undefined) {
      const x = reveal.start * this.zoom, left = this.arrange.scrollLeft;
      if (x < left || x > left + this.arrange.clientWidth - this.headWidth - 40) this.scrollTo(x - 40);
    }
  }

  renderArrange(force = false) {
    const session = this.session;
    if (!session) return;

    const width = this.timelineSeconds() * this.zoom;
    const signature = JSON.stringify([
      session.tracks.map(({ inserts, ...track }) => [track, inserts.length]),
      (session.returns ?? []).map(({ inserts, ...bus }) => [bus, inserts.length]),
      session.master.volume, session.master.inserts.length, session.tempo, session.tempoMap, this.showTempo, session.loop, session.beatsPerBar, session.markers,
      this.zoom, width, this.selectedTrack, this.selectedClip, this.snap, this.rowHeight, this.takes.version,
      this.headWidth, [...this.trackHeights], [...this.folded],
      this.showRouting, this.showSends, this.pinnedFolded, session.master.pan,
    ]);
    if (!force && signature === this.arrangeSignature) return this.movePlayhead();
    this.arrangeSignature = signature;

    this.inner.style.width = `${this.headWidth + width}px`;
    this.root.style.setProperty("--row", `${this.rowHeight}px`);
    this.root.style.setProperty("--head", `${this.headWidth}px`);
    this.root.style.setProperty("--clip", `${this.rowHeight - 8}px`);
    this.inner.style.setProperty("--bar", `${this.bar * this.zoom}px`);
    this.inner.style.setProperty("--beat", `${this.beat * this.zoom}px`);
    this.inner.classList.toggle("dense", this.beat * this.zoom < 8);
    // A changing tempo: the lanes' grid is drawn line by line instead of repeating.
    const varies = !this.tempoMap.steady;
    this.inner.classList.toggle("tempo-varies", varies);
    if (varies) {
      this.inner.style.setProperty("--grid-image", gridImage(this.tempoMap, this.beatsPerBar, this.zoom, width));
      this.inner.style.setProperty("--grid-width", `${Math.ceil(width)}px`);
    }
    this.tempoRow.hidden = !this.showTempo;
    if (this.showTempo) this.tempoLane.update(session, width);
    this.renderRuler(width);
    this.showLoop(session.loop);
    this.renderMarkers();

    const scroll = this.arrange.scrollLeft;
    this.oldWaves = new Map();
    if (this.zooming)
      for (const canvas of this.rows.querySelectorAll("canvas.clip-wave"))
        if (canvas.clip && canvas.drawn) this.oldWaves.set(canvas.clip.id, canvas);
    this.updateTrackRows(force);
    this.renderPinnedRows(force);
    this.emptyHint.hidden = session.tracks.some((track) => track.clips.length > 0);
    this.clearButton.hidden = ![...session.tracks, ...(session.returns ?? [])].some((track) => track.mute || track.solo);
    this.scrollTo(scroll);
    this.updateRecording();
    this.movePlayhead();
    this.revealPending();
    this.oldWaves = null;
  }

  // Updates the track rows to the session in place, matched by track id: a row is only
  // built again when something update() can't change has changed (its clips, colour,
  // input, kind...). A view change (zoom, height, tempo, tool) or `force` rebuilds all.
  updateTrackRows(force) {
    const session = this.session;
    const viewKey = JSON.stringify([this.zoom, this.rowHeight, session.tempo, session.tempoMap, session.beatsPerBar, this.snap, this.tool, session.inputs,
                                     this.showRouting, this.showSends, (session.returns ?? []).map((b) => [b.id, b.name, b.colour])]);
    const rebuild = force || viewKey !== this.rowsViewKey;
    this.rowsViewKey = viewKey;
    const existing = new Map(rebuild ? [] : [...this.rows.querySelectorAll(":scope > .track-row")].map((row) => [row.dataset.track, row]));
    const trackRows = session.tracks.map((track, index) => {
      const key = this.trackRowKey(track);
      let row = existing.get(String(track.id));
      if (row?.key === key) {
        row.update(track);
      } else {
        row = this.createTrackRow(track, index);
        row.key = key;
      }
      // Its take lanes follow it (built again only when the takes or the comp change).
      const takesKey = JSON.stringify([track.takes, track.takesShown, track.listenTake, this.takes.version,
                                       track.clips.filter((c) => c.take >= 0).map((c) => [c.take, c.start, c.length, c.fadeIn, c.fadeOut])]);
      if (row.takesKey !== takesKey) {
        row.takesKey = takesKey;
        row.takeRows = this.takes.createRows(track, trackColour(track, index));
      }
      return row;
    });
    const rows = trackRows.flatMap((row) => [row, ...row.takeRows]);
    // Move only rows that are out of place: rows left in the document keep their focus.
    rows.forEach((row, i) => {
      if (this.rows.children[i] !== row) this.rows.insertBefore(row, this.rows.children[i] ?? null);
    });
    while (this.rows.children.length > rows.length) this.rows.lastElementChild.remove();
    this.meters.clear();
    for (const row of trackRows) this.meters.set(row.track.id, row.meter);
  }

  renderRuler(width) {
    [...this.ruler.querySelectorAll(".bar-label")].forEach((label) => label.remove());
    const map = this.tempoMap, bars = this.beatsPerBar;
    const x = (bar) => map.toSeconds(bar * bars) * this.zoom;
    // Spacing from the shortest bar (the fastest tempo), so labels never crowd.
    const fastest = Math.max(...map.points.map((p) => p[1]));
    const barPixels = 60 / fastest * bars * this.zoom;
    const every = [1, 2, 4, 8, 16, 32, 64].find((n) => n * barPixels >= 44) ?? 128;

    for (let bar = 0; x(bar) < width; bar += every) {
      const label = html("span", "bar-label", String(bar + 1));
      label.style.left = `${x(bar)}px`;
      this.ruler.append(label);
    }
  }

  showLoop(loop) {
    const visible = loop.end > loop.start;
    for (const element of [this.loopBand, this.loopShade]) {
      element.hidden = !visible;
      element.classList.toggle("is-on", loop.on);
      element.style.width = `${(loop.end - loop.start) * this.zoom}px`;
    }
    this.loopBand.style.left = `${loop.start * this.zoom}px`;
    this.loopShade.style.left = `${this.headWidth + loop.start * this.zoom}px`;
  }

  // A track's height on screen: folded, its own (dragged), or the global height.
  trackHeight(id) {
    if (this.folded.has(id)) return FOLDED_ROW;
    return this.trackHeights.get(id) ?? this.rowHeight;
  }

  // Everything a track row draws apart from what update() can change in place: when
  // this differs, the row is built again. Volume, pan, the switches, the monitor mode,
  // the name, sends, output and the selection are left out, so moving a fader or a send
  // or selecting a track keeps the row (and its focus, hover and the control under the
  // mouse).
  trackRowKey(track) {
    const { volume, pan, mute, solo, arm, monitor, monitorMode, name, inserts, sends, output, ...rest } = track;
    const links = track.link ? this.session.tracks.filter((t) => t.link === track.link && t.id !== track.id).map((t) => t.name) : null;
    // The colour comes from the id, not the index.
    return JSON.stringify([rest, inserts.length, links, this.trackHeight(track.id), this.folded.has(track.id)]);
  }

  // Which header lines fit a row of this height, in order: name, mix, routing, sends.
  headerLines(height, kind = "audio") {
    const fit = Math.max(1, 1 + Math.floor((height - 10 - 22) / 25));
    const lines = ["name"];
    if (fit >= 2) lines.push("mix");
    if (this.showRouting && lines.length < fit) lines.push("routing");
    if (this.showSends && (this.session.returns ?? []).length && lines.length < fit) lines.push("sends");
    if (kind === "midi" && !lines.includes("routing") && lines.length < fit) lines.push("routing"); // the instrument
    return lines;
  }

  // A track's header: its name, then the mix (M, S, arm, monitor, volume, pan), then the
  // routing (input or instrument, output) and the sends when their switches are on and
  // the row is tall enough; a stereo meter down the right edge.
  createTrackRow(track, index) {
    const row = html("div", "track-row");
    row.dataset.track = track.id;
    row.track = track; // the latest session's copy, see update below
    const colour = trackColour(track, index);
    row.style.setProperty("--track-color", colour);
    const height = this.trackHeight(track.id);
    row.style.setProperty("--row", `${height}px`);
    row.style.setProperty("--clip", `${height - 8}px`);
    const folded = this.folded.has(track.id);
    row.classList.toggle("is-folded", folded);
    const current = () => row.track;
    const isMidi = track.kind === "midi";
    row.classList.toggle("is-midi", isMidi);

    const head = html("div", "track-head");
    head.addEventListener("pointerdown", (event) => {
      if (!event.target.closest("button, input, select, .hslider, .send-box")) this.select(track.id, null);
    });
    head.addEventListener("contextmenu", (event) => {
      if (event.target.closest("input")) return;
      event.preventDefault();
      this.select(track.id, null);
      this.openTrackMenu(current(), event.clientX, event.clientY);
    });

    // Name line: fold, name, link, effects.
    const fold = button("th-fold", folded ? "▸" : "▾", folded ? "Unfold the track" : "Fold the track to one line", () => this.toggleFold(track.id));
    const name = html("input", "track-name");
    Object.assign(name, { value: track.name, spellcheck: false, title: "Rename the track" });
    name.addEventListener("change", () => daw("setTrack", { id: track.id, name: name.value.trim() || current().name }));
    name.addEventListener("keydown", (event) => { if (event.key === "Enter") name.blur(); });
    const nameLine = html("div", "th-line th-name");
    nameLine.append(fold, name);
    if (track.link) {
      // Linked tracks warp as one (Pitch & time → Linked tracks).
      const others = this.session.tracks.filter((t) => t.link === track.link && t.id !== track.id).map((t) => t.name);
      const chain = html("span", "track-link", `⛓${track.link}`);
      chain.title = `Linked with ${others.join(", ") || "no other track"}: warping and quantizing apply to all of them, in phase`;
      nameLine.append(chain);
    }
    const fx = button("fx-count", track.inserts.length ? `${track.inserts.length} fx` : "+ fx", "Effects on this track", () => this.select(track.id, null, true, !track.inserts.length));
    nameLine.append(fx);

    // Mix line: M, S, arm, monitor, volume, pan.
    const toggles = html("div", "track-toggles");
    const switches = {};
    const toggle = (key, text, title) => {
      const b = button(`tt tt-${key}`, text, title, (event) => this.toggleTrackKey(current(), key, !current()[key], event));
      switches[key] = b;
      return b;
    };
    toggles.append(toggle("mute", "M", "Mute (0)"), toggle("solo", "S", "Solo (S) · Alt+click: solo only this track"),
                   toggle("arm", "●", isMidi ? "Arm to record from your MIDI keyboard (you hear it while armed)" : "Arm for recording (A)"));
    const monitor = isMidi
      ? toggle("monitor", "In", "Play this track's instrument from your MIDI keyboard")
      : button("tt tt-monitor", "Off", "", () => {
          const next = { off: "in", in: "auto", auto: "off" }[monitorMode(current())];
          daw("setTrack", { id: track.id, monitorMode: next });
        });
    toggles.append(monitor);
    const takesToggle = this.takes.createToggle(track);
    if (takesToggle) toggles.append(takesToggle);
    row.classList.toggle("has-takes", Boolean(takesToggle && track.takesShown));

    const send = (key) => (value, first) => daw("setTrack", { id: track.id, [key]: value, continue: !first });
    const volume = new HSlider({ value: track.volume, defaultValue: 0, toPosition: Fader.toPosition, fromPosition: Fader.toDecibels,
                                 parse: (t) => (/inf/i.test(t) ? FADER_MIN_DB : parseNumber(t)),
                                 format: (v) => `${dbText(v)} dB`, title: "Volume", onChange: send("volume") });
    const pan = new HSlider({ value: track.pan, defaultValue: 0, toPosition: (v) => (v + 1) / 2,
                              fromPosition: (p) => (Math.abs(p - 0.5) < 0.01 ? 0 : Math.round((p * 2 - 1) * 100) / 100),
                              parse: parsePan, format: panText, title: "Pan", bipolar: true, onChange: send("pan") });
    pan.element.classList.add("is-pan");
    const mixLine = html("div", "th-line th-mix");
    mixLine.append(toggles, volume.element, pan.element);

    // Routing line: where the sound comes from (input, or the instrument) and where it goes.
    const routingLine = html("div", "th-line th-routing");
    let input = null;
    if (isMidi) {
      const instrument = track.instrument ?? {};
      const chip = button("instrument-chip", `${instrument.error ? "⚠ " : "♫ "}${instrument.name || "Instrument"}`,
        instrument.error ? instrument.error
          : instrument.hasWindow ? `${instrument.name}: open its window` : `${instrument.name} (built-in). Drag another instrument here from the library to swap it.`,
        () => {
          this.select(track.id, null);
          if (instrument.hasWindow && !instrument.error) daw("showInstrument", { track: track.id });
          else if (instrument.error) toast(instrument.error, "is-error");
          else toast("Double-click the lane to add a MIDI clip; pick another instrument in the library or the piano roll.");
        });
      chip.classList.toggle("has-error", Boolean(instrument.error));
      routingLine.append(chip);
    } else {
      input = this.createInputPicker(track);
      routingLine.append(input.element);
    }
    const output = this.createOutputPicker(track);
    routingLine.append(html("span", "th-arrow", "→"), output.element);

    // Sends line: one box per return.
    const sendsLine = html("div", "th-line th-sends");
    const sendBoxes = new Map();
    (this.session.returns ?? []).forEach((bus, i) => {
      const box = new SendBox({ letter: returnLetter(i), name: bus.name, colour: returnColour(bus, i), value: -100,
        onChange: (db, first) => daw("setSend", { id: track.id, target: bus.id, level: db, continue: !first }) });
      sendBoxes.set(bus.id, box);
      sendsLine.append(box.element);
    });

    const lines = { name: nameLine, mix: mixLine, routing: routingLine, sends: sendsLine };
    const body = html("div", "track-head-body");
    body.append(...this.headerLines(height, track.kind).map((key) => lines[key]));

    const meter = new StereoLevel(track.id);
    meter.element.classList.add("head-meter");
    row.meter = meter;

    head.append(html("span", "track-swatch"), body, meter.element, this.createHeightHandle(row, track.id));

    // Clicking a lane selects the track; it only moves the playhead while stopped, so
    // you can click around while a take plays back. (The ruler always moves it.)
    const lane = html("div", "lane");
    lane.addEventListener("pointerdown", (event) => {
      if (event.target !== lane) return;
      this.select(track.id, null);
      if (this.status.playing) return;
      const box = lane.getBoundingClientRect();
      daw("transport", { action: "seek", time: Math.max(0, (event.clientX - box.left) / this.zoom) });
    });

    if (isMidi) {
      lane.title = "Double-click to add a MIDI clip";
      lane.addEventListener("dblclick", (event) => {
        if (event.target !== lane) return;
        const box = lane.getBoundingClientRect();
        const time = (event.clientX - box.left) / this.zoom;
        const map = this.tempoMap, bars = this.beatsPerBar;
        this.addMidiClip(current(), map.toSeconds(Math.floor(map.toBeats(time) / bars + 1e-6) * bars));
      });
    }

    for (const clip of track.clips) lane.append(this.createClip(clip, track, height));
    if (track.ride?.length) lane.append(this.createRide(track.ride));
    for (const automation of track.automation ?? []) lane.append(this.createAutomation(automation));

    row.append(head, lane);

    // Brings the row up to date with a newer copy of the same track, without rebuilding it.
    row.update = (latest) => {
      row.track = latest;
      row.classList.toggle("is-selected", this.selectedTrack === latest.id);
      row.classList.toggle("is-muted", Boolean(latest.mute));
      row.classList.toggle("is-armed", Boolean(latest.arm));
      for (const [key, b] of Object.entries(switches)) b.setAttribute("aria-pressed", String(Boolean(latest[key])));
      if (!isMidi) {
        const mode = monitorMode(latest);
        monitor.textContent = { in: "In", auto: "Auto", off: "Off" }[mode];
        monitor.dataset.mode = mode;
        monitor.setAttribute("aria-pressed", String(mode !== "off"));
        monitor.title = `Monitor: ${{ in: "In: always hear the input", auto: "Auto: hear the input while the track is armed", off: "Off: don't hear the input" }[mode]}`
          + ` (${inputText(latest.input, this.session.inputs)}, through this track's effects). Click: In → Auto → Off.`;
      }
      if (document.activeElement !== name) name.value = latest.name;
      volume.update(latest.volume);
      pan.update(latest.pan);
      input?.update(latest);
      output.update(latest);
      for (const [target, box] of sendBoxes) {
        const s = (latest.sends ?? []).find((x) => x.target === target);
        box.update(s ? s.level : -100);
      }
      for (const clip of lane.querySelectorAll(":scope > .clip"))
        clip.classList.toggle("is-selected", String(this.selectedClip) === clip.dataset.clip);
    };
    row.update(track);
    return row;
  }

  // Where a track plays: the master, or into a return (a group bus).
  createOutputPicker(track) {
    const returns = this.session.returns ?? [];
    const element = html("select", "dropdown track-output");
    element.title = "Output: where this track plays, the master or into a return (a group bus)";
    element.append(new Option("Master", "0"));
    returns.forEach((bus, index) => element.append(new Option(`${returnLetter(index)} · ${bus.name}`, String(bus.id))));
    element.addEventListener("change", () => daw("setOutput", { id: track.id, output: Number(element.value) }));
    const update = (t) => { if (document.activeElement !== element) element.value = String(t.output ?? 0); };
    update(track);
    return { element, update };
  }

  // The bottom edge of a track's header: drag to set that track's height, double-click
  // to give it the global height back.
  createHeightHandle(row, id) {
    const handle = html("div", "th-resize");
    handle.title = "Drag to change this track's height · Double-click: back to the global height";
    handle.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      event.stopPropagation();
      const start = this.trackHeight(id);
      let height = start;
      this.dragging = true;
      drag(event, (dx, dy) => {
        height = Math.round(clamp(start + dy, MIN_ROW, MAX_ROW));
        row.style.setProperty("--row", `${height}px`);
        row.style.setProperty("--clip", `${height - 8}px`);
      }, (moved) => {
        this.dragging = false;
        if (moved) {
          this.folded.delete(id);
          this.trackHeights.set(id, height);
          this.rememberView();
        }
        this.flushSession();
      });
    });
    handle.addEventListener("dblclick", (event) => {
      event.stopPropagation();
      this.trackHeights.delete(id);
      this.folded.delete(id);
      this.rememberView();
      this.renderArrange();
    });
    return handle;
  }

  toggleFold(id) {
    if (this.folded.has(id)) this.folded.delete(id);
    else this.folded.add(id);
    this.rememberView();
    this.renderArrange();
  }

  // A track's volume ride (automation), drawn over its clips: the middle is 0 dB, the
  // top and bottom ±12 dB. It holds its last value after the last point.
  createRide(points) {
    const width = Math.max(1, (points[points.length - 1][0] + 4) * this.zoom);
    const y = (db) => 50 - Math.max(-12, Math.min(12, db)) / 12 * 46;
    const coords = points.map(([t, db]) => `${(t * this.zoom).toFixed(1)},${y(db).toFixed(2)}`);
    coords.push(`${width.toFixed(1)},${y(points[points.length - 1][1]).toFixed(2)}`);
    const box = svg("svg", { class: "lane-ride", viewBox: `0 0 ${width.toFixed(1)} 100`, preserveAspectRatio: "none" });
    box.style.width = `${width}px`;
    box.append(svg("line", { class: "lane-ride-zero", x1: 0, x2: width.toFixed(1), y1: 50, y2: 50 }),
               svg("polyline", { points: coords.join(" ") }));
    const most = Math.max(...points.map(([, db]) => Math.abs(db)));
    const label = html("span", "lane-ride-label", `ride ±${most.toFixed(1)} dB`);
    label.title = "Volume ride (automation) on top of the fader. Right-click the track's name to clear it.";
    const wrap = html("div", "lane-ride-wrap");
    wrap.append(box, label);
    return wrap;
  }

  // An effect value's automation, drawn over the clips like a ride: bottom to top is the
  // lowest to the highest value it reaches (and 0 for a mix).
  createAutomation({ type, value, points }) {
    const width = Math.max(1, (points[points.length - 1][0] + 4) * this.zoom);
    const values = points.map(([, v]) => v);
    const low = Math.min(0, ...values), high = Math.max(...values, low + 1e-6);
    const y = (v) => 96 - (v - low) / (high - low) * 80;
    const coords = points.map(([t, v]) => `${(t * this.zoom).toFixed(1)},${y(v).toFixed(2)}`);
    coords.push(`${width.toFixed(1)},${y(values[values.length - 1]).toFixed(2)}`);
    const box = svg("svg", { class: "lane-ride lane-automation", viewBox: `0 0 ${width.toFixed(1)} 100`, preserveAspectRatio: "none" });
    box.style.width = `${width}px`;
    box.append(svg("polyline", { points: coords.join(" ") }));
    const name = value.replace(/^delay/, "echo ").replace(/^reverb/, "reverb ").replace(/^chorus/, "chorus ").toLowerCase();
    const label = html("span", "lane-ride-label lane-automation-label", `${name} ${Math.round(low)}–${Math.round(high)}`);
    label.title = `Effect automation (${type}). Right-click the track's name to clear it.`;
    // Where it first moves, clear of the clip's name at its start.
    const moves = points.findIndex(([, v], i) => i > 0 && v !== points[0][1]);
    label.style.left = `${Math.max(4, (moves > 0 ? points[moves][0] : 0) * this.zoom + 6)}px`;
    const wrap = html("div", "lane-ride-wrap");
    wrap.append(box, label);
    return wrap;
  }

  // The return and master rows, pinned under the tracks so they never scroll away: name,
  // M and S (returns), effects, volume, pan and a stereo meter. "+ Return" after them;
  // double-clicking a name folds or unfolds them all. Updated in place like the tracks.
  renderPinnedRows(force) {
    const session = this.session;
    const returns = session.returns ?? [];
    const key = JSON.stringify([force ? Math.random() : 0, this.pinnedFolded, returns.map((b) => [b.id, b.name, b.colour, b.inserts.length]),
                                session.master.inserts.length]);
    if (key !== this.pinnedKey) {
      this.pinnedKey = key;
      this.busRows = [...returns.map((bus, index) => this.createBusRow(bus, index, "return")), this.createBusRow(session.master, 0, "master")];
      const add = html("div", "pinned-add");
      const pick = html("select", "dropdown add-return");
      pick.title = "Add a return track: a shared effect every track can send to";
      pick.append(new Option("+ Return", ""));
      for (const [effect, name] of RETURN_EFFECTS) pick.append(new Option(name, effect));
      pick.addEventListener("change", () => {
        const effect = pick.value;
        pick.value = "";
        if (effect !== "") this.addReturn(effect === "none" ? "" : effect);
      });
      add.append(pick);
      this.returnRows.replaceChildren(...this.busRows.slice(0, -1), add);
      this.masterRow.replaceChildren(this.busRows.at(-1));
      this.pinned.classList.toggle("is-folded", this.pinnedFolded);
    }
    returns.forEach((bus, i) => this.busRows[i].update(bus));
    this.busRows.at(-1).update(session.master);
    for (const row of this.busRows) this.meters.set(row.busId, row.meter);
  }

  createBusRow(bus, index, kind) {
    const isMaster = kind === "master";
    const id = isMaster ? 0 : bus.id;
    const row = html("div", `track-row bus-row ${isMaster ? "master-row" : "return-row"}`);
    row.dataset.track = id;
    row.busId = id;
    if (!isMaster) row.style.setProperty("--track-color", returnColour(bus, index));
    const head = html("div", "track-head");
    head.addEventListener("pointerdown", (event) => { if (!event.target.closest("button, .hslider, select")) this.select(id, null); });
    const nameText = isMaster ? "Master" : bus.name;
    const name = html("span", "track-name is-static", nameText);
    name.title = `${isMaster ? "All tracks sum here" : `Return ${returnLetter(index)}: tracks send to it`} · Double-click to fold or unfold the returns and master`;
    name.addEventListener("dblclick", () => {
      this.pinnedFolded = !this.pinnedFolded;
      this.rememberView();
      this.renderArrange();
    });
    const nameLine = html("div", "th-line th-name");
    nameLine.append(isMaster ? html("span", "bus-tag", "Main") : html("span", "return-letter", returnLetter(index)), name);
    const switches = {};
    if (!isMaster) {
      const toggles = html("div", "track-toggles");
      for (const [key, text, title] of [["mute", "M", "Mute this return"], ["solo", "S", "Solo this return"]]) {
        const b = button(`tt tt-${key}`, text, title, (event) => this.toggleTrackKey(row.bus, key, !row.bus[key], event));
        switches[key] = b;
        toggles.append(b);
      }
      nameLine.append(toggles);
    }

    const send = (key) => (value, first) => daw(isMaster ? "setMaster" : "setTrack", { ...(isMaster ? {} : { id }), [key]: value, continue: !first });
    const volume = new HSlider({ value: bus.volume, defaultValue: 0, toPosition: Fader.toPosition, fromPosition: Fader.toDecibels,
                                 parse: (t) => (/inf/i.test(t) ? FADER_MIN_DB : parseNumber(t)),
                                 format: (v) => `${dbText(v)} dB`, title: isMaster ? "Master volume" : "Return volume", onChange: send("volume") });
    const pan = new HSlider({ value: bus.pan ?? 0, defaultValue: 0, toPosition: (v) => (v + 1) / 2,
                              fromPosition: (p) => (Math.abs(p - 0.5) < 0.01 ? 0 : Math.round((p * 2 - 1) * 100) / 100),
                              parse: parsePan, format: panText, title: isMaster ? "Master balance" : "Pan", bipolar: true, onChange: send("pan") });
    pan.element.classList.add("is-pan");
    if (!this.pinnedFolded) nameLine.append(volume.element, pan.element);
    nameLine.title = `${bus.inserts.length} effect${bus.inserts.length === 1 ? "" : "s"}: click to show ${bus.inserts.length ? "them" : "the effects panel"}`;

    const body = html("div", "track-head-body");
    body.append(nameLine);
    const meter = new StereoLevel(id);
    meter.element.classList.add("head-meter");
    row.meter = meter;
    head.append(html("span", "track-swatch"), body, meter.element);
    row.append(head, html("div", "lane"));

    row.update = (latest) => {
      row.bus = latest;
      row.classList.toggle("is-selected", this.selectedTrack === id);
      row.classList.toggle("is-muted", Boolean(latest.mute));
      for (const [key, b] of Object.entries(switches)) b.setAttribute("aria-pressed", String(Boolean(latest[key])));
      volume.update(latest.volume);
      pan.update(latest.pan ?? 0);
    };
    row.update(bus);
    return row;
  }

  createClip(clip, track, height = this.trackHeight(track.id)) {
    const element = html("div", "clip");
    element.dataset.clip = clip.id;
    element.classList.toggle("is-selected", this.selectedClip === clip.id);
    element.classList.toggle("is-missing", clip.missing);
    element.classList.toggle("is-muted", Boolean(clip.mute));
    element.classList.toggle("is-midi", Boolean(clip.midi));
    if (clip.midi) element.addEventListener("dblclick", () => { this.select(track.id, clip.id); this.setDockTab("track"); this.setDock(true); this.pianoRoll?.element.focus(); });
    element.title = clip.midi ? `${clip.name} (${clip.notes.length} notes)\nDouble-click to edit the notes · Drag to move · Edges to trim` : clip.missing ? `Missing file: ${clip.file}` : `${clip.name}\nDrag to move · Edges to trim · Corners to fade · S splits at the playhead`;

    const place = (start, length) => {
      element.style.left = `${start * this.zoom}px`;
      element.style.width = `${Math.max(2, length * this.zoom)}px`;
    };
    place(clip.start, clip.length);

    // Mid-zoom, reuse the old picture (stretched) instead of drawing a new one.
    const kept = this.zooming ? this.oldWaves?.get(clip.id) : null;
    const canvas = kept ?? html("canvas", "clip-wave");
    canvas.dataset.color = trackColour(track, this.session.tracks.indexOf(track));
    canvas.clip = clip;
    canvas.laneHeight = height - 8; // the track's own height (tracks can differ)
    if (kept) {
      const scale = this.zoom / (kept.zoomAt || this.zoom);
      kept.style.left = `${kept.cssLeft * scale}px`;
      kept.style.width = `${kept.cssWidth * scale}px`;
    }
    const label = html("span", "clip-name", clip.missing ? `⚠ ${clip.name}` : clip.mute ? `🔇 ${clip.name}` : clip.name);
    if (clip.musical) {
      // Musical mode: tied to bars and beats, follows the tempo track.
      const note = html("span", "clip-musical", "♩");
      note.title = `Musical mode: plays at ${Math.round(clip.clipTempo * 100) / 100} BPM, following the tempo track`;
      label.prepend(note);
    }
    if (clip.gain && Math.abs(clip.gain) > 0.05) label.textContent += `  ${dbText(clip.gain)} dB`;
    const fades = svg("svg", { class: "clip-fades", preserveAspectRatio: "none" });
    const fadeIn = svg("path"), fadeOut = svg("path");
    fades.append(fadeIn, fadeOut);
    const drawFades = (length, inSec, outSec) => {
      const w = Math.max(2, length * this.zoom);
      fades.setAttribute("viewBox", `0 0 ${w} 100`);
      fadeIn.setAttribute("d", inSec > 0 ? `M0 0 L${inSec * this.zoom} 0 L0 100 Z` : "");
      fadeOut.setAttribute("d", outSec > 0 ? `M${w} 0 L${w - outSec * this.zoom} 0 L${w} 100 Z` : "");
    };
    drawFades(clip.length, clip.fadeIn, clip.fadeOut);

    element.append(canvas, fades, label,
      html("div", "handle trim-start"), html("div", "handle trim-end"),
      html("div", "handle fade-in"), html("div", "handle fade-out"));

    if (!kept) this.drawWaveform(canvas);
    if (!clip.midi) drawWarpMarkers(element, clip, this.zoom);
    if (!clip.midi && this.tool === "warp") drawHitpoints(element, clip, this.zoom, daw);
    element.classList.toggle("is-warped", Boolean(clip.warp?.length) || Math.abs(clip.transpose || 0) > 1e-4);

    element.addEventListener("contextmenu", (event) => {
      event.preventDefault();
      event.stopPropagation();
      this.select(track.id, clip.id, false);
      this.openClipMenu(clip, event.clientX, event.clientY);
    });

    element.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      event.stopPropagation();
      this.select(track.id, clip.id, false);

      if (this.tool === "warp" && !clip.midi) {
        startWarpDrag(this, event, clip, element, daw);
        return;
      }

      if (this.tool === "cut") {
        const box = this.inner.getBoundingClientRect();
        const time = this.snapTime((event.clientX - box.left - this.headWidth) / this.zoom, event);
        if (time > clip.start + 0.01 && time < clip.start + clip.length - 0.01) daw("split", { id: clip.id, time });
        return;
      }

      if (this.tool === "gain") {
        let gain = clip.gain || 0;
        this.dragging = true;
        element.classList.add("is-dragging");
        drag(event, (dx, dy, e) => {
          gain = clamp(Math.round(((clip.gain || 0) - dy * (e.shiftKey ? 0.02 : 0.1)) * 10) / 10, -60, 24);
          label.textContent = `${clip.name}  ${dbText(gain)} dB`;
          canvas.clip = { ...clip, gain };
          this.drawWaveform(canvas);
        }, (moved) => {
          this.dragging = false;
          element.classList.remove("is-dragging");
          if (moved) daw("clipGain", { id: clip.id, gain });
          this.flushSession();
        });
        return;
      }

      const mode = ["trim-start", "trim-end", "fade-in", "fade-out"].find((c) => event.target.classList.contains(c)) ?? "move";
      const end = clip.start + clip.length;
      const maxEnd = clip.fileLength > 0 ? clip.start - clip.offset + clip.fileLength : Infinity;
      let edit = null;
      let targetTrack = track.id;
      this.dragging = true;
      element.classList.add("is-dragging");

      drag(event, (dx, dy, e) => {
        const dt = dx / this.zoom;

        if (mode === "move") {
          const start = this.snapTime(clip.start + dt, e);
          edit = { command: "moveClip", args: { id: clip.id, start } };
          place(start, clip.length);

          const row = [...this.rows.querySelectorAll(":scope > .track-row")].find((r) => {
            const box = r.getBoundingClientRect();
            return e.clientY >= box.top && e.clientY < box.bottom;
          });
          if (row) {
            targetTrack = Number(row.dataset.track);
            const home = element.closest(".track-row").getBoundingClientRect().top;
            element.style.transform = `translateY(${row.getBoundingClientRect().top - home}px)`;
          }
          if (targetTrack !== track.id) edit.args.track = targetTrack;
        } else if (mode === "trim-start") {
          const start = clamp(this.snapTime(clip.start + dt, e), Math.max(0, clip.start - clip.offset), end - 0.01);
          edit = { command: "trimStart", args: { id: clip.id, start } };
          place(start, end - start);
          canvas.clip = { ...clip, start, offset: clip.offset + (start - clip.start), length: end - start };
          this.drawWaveform(canvas);
        } else if (mode === "trim-end") {
          const newEnd = clamp(this.snapTime(end + dt, e), clip.start + 0.01, maxEnd);
          edit = { command: "trimEnd", args: { id: clip.id, end: newEnd } };
          place(clip.start, newEnd - clip.start);
          canvas.clip = { ...clip, length: newEnd - clip.start };
          this.drawWaveform(canvas);
        } else {
          const fadeInS = mode === "fade-in" ? clamp(clip.fadeIn + dt, 0, clip.length - clip.fadeOut) : clip.fadeIn;
          const fadeOutS = mode === "fade-out" ? clamp(clip.fadeOut - dt, 0, clip.length - clip.fadeIn) : clip.fadeOut;
          edit = { command: "clipFades", args: { id: clip.id, fadeIn: fadeInS, fadeOut: fadeOutS } };
          drawFades(clip.length, fadeInS, fadeOutS);
        }
      }, (moved) => {
        this.dragging = false;
        element.classList.remove("is-dragging");
        if (moved && edit) daw(edit.command, edit.args);
        if (moved && targetTrack !== track.id) this.selectedTrack = targetTrack;
        this.flushSession();
      });
    });

    return element;
  }

  // Draws the part of a clip's waveform that is on screen (plus half a screen either
  // side), at the screen's own resolution. The canvas is placed at that part of the clip
  // in pixels rather than stretched over the whole clip, so trimming an edge shows or
  // hides audio where it is, and zooming in draws finer detail (asked for, for just the
  // visible stretch) instead of scaling up a picture.
  drawWaveform(canvas, clip = canvas.clip) {
    if (clip.midi) return this.drawNotes(canvas, clip);
    const audio = clip.audioKey || clip.file; // a warped or transposed clip shows its rendered copy
    const overview = this.peaks.get(audio);
    if (!overview) {
      if (!clip.missing) this.loadPeaks(audio, clip.id);
      return;
    }
    if (overview === "loading") return;
    if (this.zooming && canvas.drawn) return; // keep the stretched picture until zooming stops

    const zoom = this.zoom;
    const view = Math.max(1, this.arrange.clientWidth - this.headWidth) / zoom;
    const left = this.arrange.scrollLeft / zoom;
    const s0 = Math.max(clip.start, left - view / 2);
    const s1 = Math.min(clip.start + clip.length, left + view * 1.5);
    canvas.hidden = s1 <= s0;
    if (canvas.hidden) return;

    const ratio = window.devicePixelRatio || 1;
    const cssLeft = (s0 - clip.start) * zoom;
    const cssWidth = Math.min((s1 - s0) * zoom, 16000 / ratio);
    const width = Math.max(1, Math.round(cssWidth * ratio));
    const height = Math.max(1, Math.round((canvas.laneHeight ?? this.rowHeight - 8) * ratio));
    const f0 = clip.offset + (s0 - clip.start);       // file time at the canvas's left edge
    const perPixel = cssWidth / zoom / width;         // file seconds per canvas pixel
    const f1 = f0 + width * perPixel;

    // Below the overview's resolution, draw from detail peaks for this stretch.
    let detail = null;
    if (perPixel < overview.seconds) {
      const fileEnd = clip.fileLength > 0 ? clip.fileLength : Infinity;
      detail = (this.detail.get(audio) ?? []).find((d) =>
        d.seconds <= perPixel * 1.05 && d.start <= f0 + 1e-6 && d.end >= Math.min(f1, fileEnd) - 1e-6) ?? null;
      if (!detail) this.loadDetail(audio, f0 - (f1 - f0) / 2, f1 + (f1 - f0) / 2, perPixel, clip.id);
    }

    const key = [audio, cssLeft, cssWidth, width, height, f0, perPixel, clip.gain || 0, canvas.dataset.color, detail?.id ?? 0].join();
    if (canvas.drawn === key) return;
    canvas.drawn = key;

    canvas.style.left = `${cssLeft}px`;
    canvas.style.width = `${cssWidth}px`;
    canvas.zoomAt = zoom; canvas.cssLeft = cssLeft; canvas.cssWidth = cssWidth;
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext("2d");
    context.fillStyle = canvas.dataset.color || "#8c7bff";
    const gain = Math.pow(10, (clip.gain || 0) / 20);
    const middle = height / 2, reach = middle - ratio;

    if (detail) {
      const scale = (v) => clamp(v * gain, -1, 1) * reach;
      for (let x = 0; x < width; ++x) {
        const t = f0 + x * perPixel - detail.start;
        const a = Math.floor(t / detail.seconds);
        const b = Math.max(a + 1, Math.floor((t + perPixel) / detail.seconds));
        let lo = Infinity, hi = -Infinity;
        for (let i = Math.max(0, a); i < Math.min(b, detail.max.length); ++i) {
          lo = Math.min(lo, detail.min[i]);
          hi = Math.max(hi, detail.max[i]);
        }
        if (lo > hi) continue;
        const top = middle - scale(hi), bottom = middle - scale(lo);
        context.fillRect(x, Math.min(top, middle - 0.5 * ratio), 1, Math.max(ratio, bottom - top));
      }
      return;
    }

    const peaks = overview.peaks, step = overview.seconds;
    for (let x = 0; x < width; ++x) {
      const a = Math.floor((f0 + x * perPixel) / step);
      const b = Math.max(a + 1, Math.floor((f0 + (x + 1) * perPixel) / step));
      let peak = 0;
      for (let i = Math.max(0, a); i < Math.min(b, peaks.length); ++i) peak = Math.max(peak, peaks[i]);
      const half = Math.max(0.5 * ratio, Math.min(1, peak * gain) * reach);
      context.fillRect(x, middle - half, 1, half * 2);
    }
  }

  // A MIDI clip's notes, as small bars spanning the clip's pitch range.
  drawNotes(canvas, clip) {
    const zoom = this.zoom;
    const ratio = window.devicePixelRatio || 1;
    const cssWidth = Math.min(Math.max(2, clip.length * zoom), 16000 / ratio);
    const width = Math.max(1, Math.round(cssWidth * ratio));
    const height = Math.max(1, Math.round((canvas.laneHeight ?? this.rowHeight - 8) * ratio));
    const visible = clip.notes.filter((n) => n.time < clip.offset + clip.length && n.time + n.length > clip.offset);
    const key = ["midi", cssWidth, width, height, clip.offset, canvas.dataset.color, visible.map((n) => `${n.pitch}:${n.time}:${n.length}`).join(",")].join();
    if (canvas.drawn === key) return;
    canvas.drawn = key;
    canvas.hidden = false;
    canvas.style.left = "0px";
    canvas.style.width = `${cssWidth}px`;
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext("2d");
    if (!visible.length) return;
    let lo = Math.min(...visible.map((n) => n.pitch)), hi = Math.max(...visible.map((n) => n.pitch));
    if (hi - lo < 12) { const mid = (hi + lo) / 2; lo = Math.floor(mid - 6); hi = Math.ceil(mid + 6); }
    const top = 14 * ratio, usable = Math.max(ratio * 4, height - top - 3 * ratio);
    const rowH = Math.max(ratio, Math.min(6 * ratio, usable / (hi - lo + 1)));
    context.fillStyle = canvas.dataset.color || "#8c7bff";
    for (const n of visible) {
      const x0 = (Math.max(n.time, clip.offset) - clip.offset) * zoom * ratio;
      const x1 = (Math.min(n.time + n.length, clip.offset + clip.length) - clip.offset) * zoom * ratio;
      const y = top + (hi - n.pitch) / Math.max(1, hi - lo) * (usable - rowH);
      context.globalAlpha = 0.5 + 0.5 * (n.velocity / 127);
      context.fillRect(x0, y, Math.max(ratio, x1 - x0 - ratio), rowH);
    }
    context.globalAlpha = 1;
  }

  // Redraws the waveforms on screen (all, or one file's), e.g. after scrolling.
  redrawWaves(file = null) {
    for (const canvas of this.rows.querySelectorAll("canvas.clip-wave"))
      if (canvas.clip && (!file || (canvas.clip.audioKey || canvas.clip.file) === file)) this.drawWaveform(canvas);
  }

  // `file` is the cache key (the clip's audioKey); `clip` asks for that clip's rendered audio.
  async loadPeaks(file, clip = -1) {
    if (this.peaks.has(file)) return;
    this.peaks.set(file, "loading");
    const result = await nativeDaw("peaks", { file, clip });
    if (result?.peaks) {
      this.peaks.set(file, result);
      this.redrawWaves(file);
    } else {
      setTimeout(() => { this.peaks.delete(file); this.redrawWaves(file); }, 400); // still decoding
    }
  }

  // Fetches detail peaks for one stretch of a file; keeps the last few per file.
  async loadDetail(file, start, end, seconds, clip = -1) {
    if (this.detailPending.has(file)) return;
    this.detailPending.add(file);
    let result = null;
    try {
      result = await nativeDaw("peaks", { file, clip, start: Math.max(0, start), end, seconds });
    } finally {
      this.detailPending.delete(file);
    }
    if (!result?.max) return;
    result.id = ++this.detailCount;
    const list = [result, ...(this.detail.get(file) ?? [])].slice(0, 6);
    this.detail.set(file, list);
    this.redrawWaves(file);
  }

  // The tempo box: the tempo at the playhead (the start tempo while it's steady).
  showTempoValue() {
    if (!this.tempoInput || document.activeElement === this.tempoInput) return;
    const bpm = this.tempoMap.steady ? this.tempo : this.tempoMap.bpmAt(Math.max(0, this.status?.position ?? 0));
    const text = String(Math.round(bpm * 100) / 100);
    if (this.tempoInput.value !== text) this.tempoInput.value = text;
  }

  movePlayhead(force = false) {
    if (!this.tempoMap.steady) this.showTempoValue();
    const x = this.headWidth + Math.max(0, this.status.position) * this.zoom;
    this.playhead.style.transform = `translateX(${x}px)`;

    // With Follow on, page along with the playhead while playing.
    if ((this.status.playing || force) && this.follow && !this.dragging && this.view === "arrange") {
      const left = this.arrange.scrollLeft, visible = this.arrange.clientWidth;
      if (x > left + visible - 40 || x < left + this.headWidth) this.scrollTo(x - this.headWidth - 40);
    }
  }

  // While recording, a growing take on every armed track, drawn from the input level.
  updateRecording() {
    const recording = this.status.recording;
    for (const row of this.rows.children) {
      let region = row.querySelector(".rec-region");
      if (!recording || !row.classList.contains("is-armed")) {
        region?.remove();
        continue;
      }
      if (!region) {
        region = html("div", "rec-region");
        region.append(html("canvas"), html("span", "rec-label", "Recording"));
        row.querySelector(".lane").append(region);
      }
      const pass = this.recPass?.number ?? 1;
      region.lastChild.textContent = pass > 1 ? `Recording · take ${pass} of the loop` : "Recording";

      const start = this.recPass?.start ?? this.status.recordStart ?? 0;
      const length = Math.max(0, this.status.position - start);
      region.style.left = `${start * this.zoom}px`;
      region.style.width = `${length * this.zoom}px`;
      region.classList.toggle("is-count-in", this.status.position < start);

      const canvas = region.firstChild;
      const ratio = window.devicePixelRatio || 1;
      const width = Math.max(1, Math.round(Math.min(length * this.zoom, 16000 / ratio) * ratio));
      const height = Math.max(1, Math.round(((row.offsetHeight || this.rowHeight) - 9) * ratio));
      if (canvas.width !== width) canvas.width = width;
      if (canvas.height !== height) canvas.height = height;
      const context = canvas.getContext("2d");
      context.clearRect(0, 0, width, height);
      context.fillStyle = "#ff5d6c";
      const middle = height / 2;
      const perSecond = width / Math.max(1e-6, length);
      let x0 = 0;
      for (const [time, gain] of this.recLevels) {
        const x = Math.round((time - start) * perSecond);
        if (x < 0) continue;
        const half = Math.max(0.5 * ratio, Math.min(1, gain) * (middle - ratio));
        context.fillRect(x0, middle - half, Math.max(1, x - x0), half * 2);
        x0 = x;
      }
    }
  }

  layoutArrange() {
    if (this.session && this.inner.offsetWidth < this.arrange.clientWidth) this.renderArrange();
    else this.redrawWaves();
  }

  //============================================================================ mixer
  buildMixer() {
    this.mixer = this.root.querySelector(".mixer");
    this.strips = new Map(); // id -> { fader, pan }
    this.mixerSignature = "";
  }

  renderMixer() {
    const session = this.session;
    const returns = session.returns ?? [];
    const describe = (t) => `${t.id}:${t.name}:${t.inserts.map((i) => i.type).join("+")}:${t.colour}`;
    const signature = session.tracks.map(describe).join("|") + `|r:${returns.map(describe).join("|")}` + `|in:${(session.inputs ?? []).join(",")}`
                      + `|m:${session.master.inserts.map((i) => i.type).join("+")}`;

    if (signature !== this.mixerSignature) {
      this.mixerSignature = signature;
      this.strips.clear();
      const strips = session.tracks.map((track, index) => this.createStrip(track, index));
      strips.push(html("div", "strip-gap"));
      returns.forEach((bus, index) => strips.push(this.createStrip(bus, index, "return")));
      strips.push(this.createAddReturnStrip());
      // The master stays pinned at the right edge while the strips scroll under it.
      const masterStrip = this.createStrip({ ...session.master, id: 0, name: "Master" }, -1, "master");
      this.mixer.replaceChildren(...strips, masterStrip);
    }

    for (const track of [...session.tracks, ...returns]) {
      const strip = this.strips.get(track.id);
      strip.fader.update(track.volume);
      strip.pan.update(track.pan);
      strip.readout.textContent = dbText(track.volume);
      for (const [key, b] of Object.entries(strip.toggles)) b.setAttribute("aria-pressed", String(Boolean(track[key])));
      strip.element.classList.toggle("is-selected", this.selectedTrack === track.id);
      strip.sends?.update(track);
      strip.input?.update(track);
    }

    const master = this.strips.get(0);
    master.fader.update(session.master.volume);
    master.pan.update(session.master.pan ?? 0);
    master.readout.textContent = dbText(session.master.volume);
    master.element.classList.toggle("is-selected", this.selectedTrack === 0);
  }

  // The dashed strip after the returns: adds a return track, empty or with an effect.
  createAddReturnStrip() {
    const element = html("div", "strip is-add-return");
    const pick = html("select", "dropdown add-return");
    pick.title = "Add a return track: a shared effect every track can send to";
    pick.append(new Option("+ Return…", ""));
    for (const [effect, name] of RETURN_EFFECTS) pick.append(new Option(name, effect));
    pick.addEventListener("change", () => {
      const effect = pick.value;
      pick.value = "";
      if (effect !== "") this.addReturn(effect === "none" ? "" : effect);
    });
    element.append(html("div", "strip-add-caption", "Returns hold shared effects: turn up a track's send to feed one."), pick);
    return element;
  }

  async addReturn(effect) {
    const name = RETURN_EFFECTS.find(([key]) => key === effect)?.[1];
    const id = await daw("addReturn", { effect, name: effect ? name : "" });
    if (typeof id === "number" && id > 0) this.select(id, null);
  }

  // A track's sends to the returns, one small knob each, plus where the track plays.
  createSends(track, { compact = false } = {}) {
    const returns = this.session.returns ?? [];
    const element = html("div", compact ? "strip-sends" : "send-row");
    const knobs = new Map();
    const pres = new Map();
    returns.forEach((bus, index) => {
      const cell = html("div", "send");
      const knob = new ValueKnob({
        label: compact ? returnLetter(index) : `${returnLetter(index)} ${bus.name}`, min: -60, max: 6, skew: 2.2,
        value: -100, defaultValue: -60, format: sendText,
        onChange: (v, first) => daw("setSend", { id: track.id, target: bus.id, level: v <= -59.5 ? -100 : v, continue: !first }),
      });
      knob.element.title = `Send to ${bus.name} · Drag or scroll · Double-click to turn off`;
      knob.element.classList.add("send-knob");
      cell.append(knob.element);
      knobs.set(bus.id, knob);
      if (!compact) {
        const pre = button("mini send-pre", "Pre", "Pre-fader: send before the track's volume and pan (Post when off)",
                           () => daw("setSend", { id: track.id, target: bus.id, pre: pre.getAttribute("aria-pressed") !== "true" }));
        cell.append(pre);
        pres.set(bus.id, pre);
      }
      element.append(cell);
    });

    const output = html("select", "dropdown track-output");
    output.title = "Where this track plays: the master, or into a return (a group bus)";
    output.append(new Option(compact ? "Master" : "Out: Master", "0"));
    returns.forEach((bus, index) => output.append(new Option(`${compact ? "" : "Out: "}${returnLetter(index)} · ${bus.name}`, String(bus.id))));
    output.addEventListener("change", () => daw("setOutput", { id: track.id, output: Number(output.value) }));
    element.append(output);

    const update = (t) => {
      for (const [target, knob] of knobs) {
        const send = (t.sends ?? []).find((x) => x.target === target);
        knob.update(send ? send.level : -100);
        pres.get(target)?.setAttribute("aria-pressed", String(Boolean(send?.pre)));
        knob.element.classList.toggle("is-on", Boolean(send && send.level > -60));
      }
      if (document.activeElement !== output) output.value = String(t.output ?? 0);
    };
    update(track);
    return { element, update };
  }

  // Which audio interface inputs feed a track: one (mono) or a pair (stereo).
  createInputPicker(track, { panel = false } = {}) {
    const names = this.session.inputs ?? [];
    const setInput = (input) => daw("setTrack", { id: track.id, input });

    if (!panel) {
      const select = html("select", "dropdown track-input");
      select.title = "Input: which inputs of your audio interface this track records and monitors. A pair records in stereo.";
      const fill = (current) => {
        select.replaceChildren(...inputChoices(names, current).map((c) => {
          const option = new Option(inputShort(c), inputKey(c));
          option.title = inputText(c, names);
          return option;
        }));
      };
      fill(track.input);
      select.addEventListener("change", () => setInput(select.value === "" ? [] : select.value.split(",").map(Number)));
      const update = (t) => {
        if (document.activeElement === select) return;
        if (![...select.options].some((o) => o.value === inputKey(t.input))) fill(t.input);
        select.value = inputKey(t.input);
      };
      update(track);
      return { element: select, update };
    }

    // The panel: left (or the only one) and right, any two inputs.
    const element = html("div", "input-pick");
    const left = html("select", "dropdown");
    const right = html("select", "dropdown");
    left.title = "The input this track records (the left side when it's stereo)";
    right.title = "Stereo: the input for the right side. Mono: the track records one input.";
    left.append(new Option("No input", "-1"), ...names.map((n, i) => new Option(`${inputNumber(i)} · ${n}`, String(i))));
    right.append(new Option("Mono", "-1"), ...names.map((n, i) => new Option(`${inputNumber(i)} · ${n}`, String(i))));
    const change = () => {
      const l = Number(left.value), r = Number(right.value);
      setInput(l < 0 ? [] : r < 0 || r === l ? [l] : [l, r]);
    };
    left.addEventListener("change", change);
    right.addEventListener("change", change);
    const labelled = (text, select) => {
      const row = html("label", "input-pick-row");
      row.append(html("span", "", text), select);
      return row;
    };
    element.append(labelled("In", left), labelled("Right", right));
    const ensure = (select, value) => {
      if (![...select.options].some((o) => o.value === value)) select.append(new Option(`${Number(value) + 1} · (not connected)`, value));
      select.value = value;
    };
    const update = (t) => {
      if (document.activeElement === left || document.activeElement === right) return;
      const input = t.input ?? [0];
      ensure(left, String(input[0] ?? -1));
      ensure(right, String(input[1] ?? -1));
      right.disabled = input.length === 0;
    };
    update(track);
    return { element, update };
  }

  createStrip(track, index, kind = "track") {
    const isMaster = kind === "master";
    const isReturn = kind === "return";
    const element = html("div", `strip${isMaster ? " is-master" : ""}${isReturn ? " is-return" : ""}`);
    if (!isMaster) element.dataset.track = track.id;
    element.style.setProperty("--track-color", isMaster ? "var(--accent-2)" : isReturn ? returnColour(track, index) : trackColour(track, index));
    element.addEventListener("pointerdown", (event) => {
      if (!event.target.closest(".fader, .knob, button")) this.select(track.id, null);
    });

    const inserts = html("div", "strip-inserts");
    for (const insert of track.inserts) {
      const part = insert.type === "pitch" ? { name: "Pitch" } : MODULES.find((m) => MODULE_KEYS[m.id] === insert.type);
      inserts.append(button("strip-insert", part?.name ?? insert.type, "Edit in the side panel", () => this.select(track.id, null)));
    }
    inserts.append(button("strip-insert is-add", "+ Effect", "Add an effect", () => this.select(track.id, null, true, true)));

    const send = (key) => (value, first) =>
      daw(isMaster ? "setMaster" : "setTrack", isMaster ? { [key]: value, continue: !first } : { id: track.id, [key]: value, continue: !first });

    const pan = new ValueKnob({ label: isMaster ? "Balance" : "Pan", min: -1, max: 1, value: track.pan ?? 0, defaultValue: 0, bipolar: true,
                                format: (v) => (Math.abs(v) < 0.01 ? "C" : `${Math.round(Math.abs(v) * 100)}${v < 0 ? "L" : "R"}`),
                                onChange: send("pan") });
    const fader = new Fader({ value: track.volume, onChange: send("volume") });
    const meter = new StereoLevel(track.id);

    const faderRow = html("div", "strip-fader");
    faderRow.append(fader.element, meter.element);
    const readout = html("div", "strip-readout");

    const toggles = {};
    const buttons = html("div", "strip-toggles");
    const keys = isMaster ? [] : isReturn ? [["mute", "M", "Mute"], ["solo", "S", "Solo"]]
                                          : [["mute", "M", "Mute"], ["solo", "S", "Solo"], ["arm", "●", "Arm"], ["monitor", "In", "Monitor"]];
    for (const [key, text, title] of keys) {
        toggles[key] = button(`tt tt-${key}`, text, key === "solo" ? "Solo (Alt+click: solo only this track)" : title,
                              (event) => this.toggleTrackKey(track, key, toggles[key].getAttribute("aria-pressed") !== "true", event));
        buttons.append(toggles[key]);
      }

    const sends = kind === "track" && (this.session.returns ?? []).length ? this.createSends(track, { compact: true }) : null;
    const name = html("div", "strip-name", isReturn ? `${returnLetter(index)} · ${track.name}` : track.name);
    if (isReturn) {
      const remove = button("mini is-danger strip-remove", "×", `Delete the ${track.name} return (its sends go too)`, async () => {
        await daw("removeReturn", { id: track.id });
        toast(`Deleted the "${track.name}" return.`, "", "undo");
      });
      element.append(remove);
    }
    const input = kind === "track" && track.kind !== "midi" ? this.createInputPicker(track) : null;
    element.append(...(input ? [input.element] : []), inserts, ...(sends ? [sends.element] : []), pan.element, buttons, faderRow, readout, name);
    this.strips.set(track.id, { element, fader, pan, readout, toggles, meter, sends, input });
    return element;
  }

  setView(view) {
    this.view = view;
    this.root.classList.toggle("show-mixer", view === "mixer");
    this.render();
    this.rememberView();
  }

  //============================================================================ view state
  // How the project was being looked at (zoom, scroll, heights, selection, panels) is
  // kept in the project, so reopening a song puts you back where you were. Not an
  // edit: no undo step, and it doesn't make the project unsaved on its own.
  viewState() {
    return {
      zoom: this.zoom, rowHeight: this.rowHeight, left: Math.round(this.arrange.scrollLeft), top: Math.round(this.arrange.scrollTop),
      track: this.selectedTrack, view: this.view, follow: this.follow, grid: this.grid, snap: this.snap,
      dock: { hidden: this.root.classList.contains("dock-hidden"), height: this.dockHeight, tab: this.dockTab },
      head: this.headWidth, heights: Object.fromEntries(this.trackHeights), folded: [...this.folded],
      routing: this.showRouting, sends: this.showSends, pinnedFolded: this.pinnedFolded,
    };
  }

  rememberView() {
    if (!this.session || this.restoringView) return;
    clearTimeout(this.viewTimer);
    this.viewTimer = setTimeout(() => {
      const text = JSON.stringify(this.viewState());
      if (text === this.savedView) return;
      this.savedView = text;
      nativeDaw("viewState", { view: text });
    }, 800);
  }

  // Before the first render of a project just opened: everything but the scroll position.
  restoreView(text) {
    let state = null;
    try { state = text ? JSON.parse(text) : null; } catch { state = null; }
    this.savedView = text || "";
    // Another project: its own heights, folds and sections (none saved: the defaults).
    this.trackHeights = new Map();
    this.folded = new Set();
    this.showRouting = this.showSends = this.pinnedFolded = false;
    this.headSwitches.forEach((b) => b.setAttribute("aria-pressed", "false"));
    if (!state || typeof state !== "object") return null;
    this.restoringView = true;
    const num = (value, fallback) => (Number.isFinite(Number(value)) ? Number(value) : fallback);
    this.zoom = clamp(num(state.zoom, this.zoom), MIN_ZOOM, MAX_ZOOM);
    this.rowHeight = Math.round(clamp(num(state.rowHeight, this.rowHeight), MIN_ROW, MAX_ROW));
    if (typeof state.follow === "boolean") this.follow = state.follow;
    if (typeof state.snap === "boolean") this.snap = state.snap;
    if (state.grid === "bar" || GRIDS.some(([g]) => g === state.grid)) this.grid = state.grid;
    if (state.track === 0 || this.session.tracks.some((t) => t.id === state.track) || (this.session.returns ?? []).some((r) => r.id === state.track)) {
      this.selectedTrack = state.track;
      this.selectedClip = null;
    }
    this.headWidth = Math.round(clamp(num(state.head, HEAD_WIDTH), MIN_HEAD, MAX_HEAD));
    this.trackHeights = new Map(Object.entries(state.heights && typeof state.heights === "object" ? state.heights : {})
      .map(([id, h]) => [Number(id), Math.round(clamp(num(h, this.rowHeight), MIN_ROW, MAX_ROW))]));
    this.folded = new Set(Array.isArray(state.folded) ? state.folded.map(Number) : []);
    this.showRouting = Boolean(state.routing);
    this.showSends = Boolean(state.sends);
    this.pinnedFolded = Boolean(state.pinnedFolded);
    this.headSwitches.forEach((b) => b.setAttribute("aria-pressed", String(this[b.dataset.key])));
    if (state.view === "arrange" || state.view === "mixer") {
      this.view = state.view;
      this.root.classList.toggle("show-mixer", this.view === "mixer");
    }
    if (state.dock && typeof state.dock === "object") {
      if (state.dock.tab === "effects" || state.dock.tab === "track") this.setDockTab(state.dock.tab, false);
      this.setDock(!state.dock.hidden, clamp(num(state.dock.height, this.dockHeight), 140, 900), false);
    }
    this.restoringView = false;
    return { left: num(state.left, 0), top: num(state.top, 0) };
  }

  //============================================================================ inspector
  buildInspector() {
    this.inspector = this.root.querySelector(".inspector");
    this.inspectorKey = "";

    // The bar above the effects panel: drag to resize, double-click or its button to fold away.
    const main = this.root.querySelector(".daw-main");
    const splitter = this.root.querySelector(".dock-splitter");
    this.dockToggle = splitter.querySelector(".dock-toggle");
    const saved = (() => { try { return JSON.parse(localStorage.getItem("micfx.dock") ?? "null"); } catch { return null; } })();
    this.dockHeight = saved?.height ?? 290;
    this.dockTabs = [...splitter.querySelectorAll(".dock-tab")];
    this.setDockTab(saved?.tab === "track" ? "track" : "effects", false);
    this.setDock(saved?.hidden ? false : true, this.dockHeight);

    // Two tabs: Effects (the track's inserts) and Track (the clip, pitch & time, quantize, tempo).
    // Clicking the open tab while the panel is folded away shows it again.
    this.dockTabs.forEach((tab) => tab.addEventListener("click", () => {
      this.setDockTab(tab.dataset.tab);
      if (this.root.classList.contains("dock-hidden")) this.setDock(true);
    }));
    const onControl = (event) => event.target === this.dockToggle || event.target.closest(".dock-tab");
    this.dockToggle.addEventListener("click", () => this.setDock(this.root.classList.contains("dock-hidden")));
    splitter.addEventListener("dblclick", (event) => { if (!onControl(event)) this.setDock(this.root.classList.contains("dock-hidden")); });
    splitter.addEventListener("pointerdown", (event) => {
      if (event.button !== 0 || onControl(event)) return;
      event.preventDefault();
      splitter.setPointerCapture(event.pointerId);
      splitter.classList.add("is-dragging");
      const startY = event.clientY;
      const startHeight = this.root.classList.contains("dock-hidden") ? 0 : this.dockHeight;
      const move = (e) => {
        const height = startHeight + startY - e.clientY;
        if (height < 60) this.setDock(false, undefined, false);
        else this.setDock(true, clamp(height, 140, Math.max(140, main.clientHeight - 160)), false);
      };
      const up = () => {
        splitter.classList.remove("is-dragging");
        splitter.removeEventListener("pointermove", move);
        splitter.removeEventListener("pointerup", up);
        splitter.removeEventListener("pointercancel", up);
        this.setDock(!this.root.classList.contains("dock-hidden"));
      };
      splitter.addEventListener("pointermove", move);
      splitter.addEventListener("pointerup", up);
      splitter.addEventListener("pointercancel", up);
    });
    splitter.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowUp" && event.key !== "ArrowDown") return;
      event.preventDefault();
      event.stopPropagation();
      const delta = event.key === "ArrowUp" ? 20 : -20;
      this.setDock(true, clamp(this.dockHeight + delta, 140, Math.max(140, main.clientHeight - 160)));
    });
  }

  // Shows or hides the effects panel, optionally at a new height; remembered between runs.
  setDock(show, height = this.dockHeight, save = true) {
    this.dockHeight = height;
    this.root.style.setProperty("--dock", `${height}px`);
    this.root.classList.toggle("dock-hidden", !show);
    this.dockToggle.textContent = show ? "Hide" : "Show";
    this.dockToggle.setAttribute("aria-expanded", String(show));
    if (save) this.saveDock();
    if (this.session) this.watchInserts(show && this.chainGains ? this.chainGains.id : -1);
  }
  saveDock() {
    this.rememberView();
    try {
      localStorage.setItem("micfx.dock", JSON.stringify({ height: this.dockHeight, hidden: this.root.classList.contains("dock-hidden"), tab: this.dockTab }));
    } catch { /* not kept */ }
  }

  // Picks the bottom panel's tab: "effects" or "track".
  setDockTab(tab, save = true) {
    if (tab === this.dockTab) return;
    this.dockTab = tab;
    this.dockTabs.forEach((t) => {
      const on = t.dataset.tab === tab;
      t.classList.toggle("is-active", on);
      t.setAttribute("aria-selected", String(on));
    });
    this.inspector.dataset.tab = tab;
    if (save) this.saveDock();
    this.rememberView();
    if (this.session) {
      this.inspectorKey = "";
      this.renderInspector();
    }
  }


  select(trackId, clipId, render = true, openAdd = false) {
    const changed = this.selectedTrack !== trackId || this.selectedClip !== clipId;
    this.selectedTrack = trackId;
    this.selectedClip = clipId;
    if (changed) this.rememberView();
    if (changed && render) this.render();
    else if (changed) {
      this.root.querySelectorAll(".clip.is-selected, .track-row.is-selected").forEach((e) => e.classList.remove("is-selected"));
      this.root.querySelector(`.clip[data-clip="${clipId}"]`)?.classList.add("is-selected");
      this.root.querySelector(`.track-row[data-track="${trackId}"]`)?.classList.add("is-selected");
      this.renderInspector();
    }
    if (openAdd) {
      this.setDockTab("effects");
      this.setDock(true);
      this.inspector.querySelector(".add-effect")?.focus();
    }
  }

  findClip(id) {
    for (const track of this.session?.tracks ?? []) {
      const clip = track.clips.find((c) => c.id === id);
      if (clip) return { clip, track };
    }
    return null;
  }

  renderInspector() {
    const session = this.session;
    const selectedReturn = (session.returns ?? []).find((r) => r.id === this.selectedTrack);
    let owner = this.selectedTrack === 0 ? { ...session.master, id: 0, name: "Master" }
              : selectedReturn ? { ...selectedReturn, isReturn: true }
              : session.tracks.find((t) => t.id === this.selectedTrack);
    if (!owner && session.tracks.length) {
      owner = session.tracks[0];
      this.selectedTrack = owner.id;
    }
    const found = this.findClip(this.selectedClip);
    if (!found) this.selectedClip = null;

    // Rebuild only when what's shown changes, so knobs keep working mid-drag.
    // Effects tab: the track's insert chain. Track tab: the clip, then pitch & time.
    const onTrackTab = this.dockTab === "track";
    const returnsKey = (session.returns ?? []).map((r) => `${r.id}:${r.name}`).join(",");
    const key = `${this.dockTab}|${owner?.id}|${found?.clip.id}|${owner?.name}|${returnsKey}|${(this.session.inputs ?? []).join(",")}`;
    if (key !== this.inspectorKey) {
      this.inspectorKey = key;
      this.inspector.replaceChildren();
      this.clipPanel?.dispose?.();
      this.clipPanel = this.rhythmPanel = null;
      this.rack = null;
      this.chainGains = null;
      this.panelSends = null;
      if (onTrackTab) {
        this.clipPanel = !found ? null : found.clip.midi ? this.createPianoRoll(found.clip, found.track) : this.createClipPanel(found.clip);
        this.inspector.append(this.clipPanel?.element ?? this.createNoClipPanel(owner));
        this.rhythmPanel = owner && owner.id !== 0 && !owner.isReturn && owner.kind !== "midi" // audio quantize/warp only
          ? createRhythmPanel(this, owner, found?.clip ?? null, { daw, ValueKnob, button, toast: (...a) => toast(...a) }) : null;
        if (this.rhythmPanel) this.inspector.append(this.rhythmPanel.element);
      } else if (owner) {
        this.inspector.append(this.createTrackPanel(owner));
      }
    }

    if (this.rhythmPanel && owner) this.rhythmPanel.update(owner, found?.clip ?? null);

    if (found && this.clipPanel) this.clipPanel.update(found.clip, found.track);
    if (owner) this.rack?.update(owner);
    if (owner && this.panelSends?.id === owner.id) this.panelSends.update(owner);
    if (owner && this.panelInput?.id === owner.id) this.panelInput.update(owner);
    if (owner && this.chainGains?.id === owner.id) {
      this.chainGains.input.knob.update(owner.inGain ?? 0);
      this.chainGains.output.knob.update(owner.outGain ?? 0);
    }
    this.watchInserts(owner && !onTrackTab && !this.root.classList.contains("dock-hidden") ? owner.id : -1);
  }

  // The Track tab's clip card when no clip is selected.
  createNoClipPanel(owner) {
    const element = html("section", "panel clip-panel is-empty");
    const head = html("div", "clip-head");
    head.append(html("h3", "", "Clip"));
    element.append(head, html("p", "panel-caption",
      owner?.id === 0 ? "The master has no clips. Select a clip on a track to edit it here."
      : owner?.isReturn ? "A return track has no clips: tracks send to it. Select a clip on a track to edit it here."
      : owner?.kind === "midi" ? "Select a MIDI clip to edit its notes here, or double-click the lane for a new one."
      : "Select a clip to change its gain and fades, split, loop or strip silence."));
    return element;
  }

  // Asks for live readings of the effects on screen (or none, -1, while the panel is hidden).
  watchInserts(id) {
    if (id === this.watchedInserts) return;
    this.watchedInserts = id;
    daw("watchInserts", { track: id });
    if (id < 0) this.rack?.live(null);
  }

  // A MIDI clip's notes, in the dock (pianoroll.js).
  createPianoRoll(clip, track) {
    const roll = new PianoRoll({ clip, track, session: this.session, daw, toast: (text) => toast(text),
                                 colour: trackColour(track, this.session.tracks.indexOf(track)) });
    this.pianoRoll = roll;
    return {
      element: roll.element,
      update: (c, t) => roll.update(c, t, this.session),
      dispose: () => { roll.dispose(); if (this.pianoRoll === roll) this.pianoRoll = null; },
    };
  }

  createClipPanel(clip) {
    const element = html("section", "panel clip-panel");
    const head = html("div", "clip-head");
    const id = clip.id;
    const nameInput = html("input", "clip-name-input");
    Object.assign(nameInput, { value: clip.name, spellcheck: false, title: "Rename the clip" });
    nameInput.addEventListener("change", () => { if (nameInput.value.trim()) daw("setClip", { id, name: nameInput.value.trim() }); });
    nameInput.addEventListener("keydown", (event) => { if (event.key === "Enter") nameInput.blur(); });
    head.append(html("h3", "", "Clip"), nameInput);
    const row = html("div", "insert-controls clip-controls");
    const current = () => this.findClip(id)?.clip ?? clip;

    const gain = new ValueKnob({ label: "Gain", min: -24, max: 24, value: clip.gain, defaultValue: 0, bipolar: true, unit: "dB",
                                 onChange: (v, first) => daw("clipGain", { id, gain: v, continue: !first }) });
    const fadeFormat = (v) => `${v.toFixed(2)} s`;
    const fadeIn = new ValueKnob({ label: "Fade in", min: 0, max: Math.max(0.01, clip.length), skew: 0.5, value: clip.fadeIn, defaultValue: 0,
                                   format: fadeFormat, onChange: (v, first) => daw("clipFades", { id, fadeIn: v, fadeOut: current().fadeOut, continue: !first }) });
    const fadeOut = new ValueKnob({ label: "Fade out", min: 0, max: Math.max(0.01, clip.length), skew: 0.5, value: clip.fadeOut, defaultValue: 0,
                                    format: fadeFormat, onChange: (v, first) => daw("clipFades", { id, fadeIn: current().fadeIn, fadeOut: v, continue: !first }) });
    row.append(gain.element, fadeIn.element, fadeOut.element);

    const muteButton = button("toggle", "Mute", "Silence this clip without deleting it (Shift+M)", () => this.run("muteClip"));

    // All clip controls sit in one compact column: name, knobs, then three rows of buttons.
    const toolRow = (...items) => { const r = html("div", "clip-tool-row"); r.append(...items); return r; };
    const tools = html("div", "clip-tools");
    tools.append(
      toolRow(button("tool", "Split", "Split at the playhead (Ctrl+E)", () => this.splitSelected()),
              button("tool", "Duplicate", "Copy after itself (Ctrl+D)", () => this.duplicateSelected()),
              button("tool is-danger", "Delete", "Delete the clip (Delete)", () => this.deleteSelected())),
      toolRow(muteButton,
              button("tool", "Loop clip", "Loop the clip's range (Ctrl+L)", () => this.run("loopClip")),
              button("tool", "Zoom to clip", "Fit the clip in the view (Z)", () => this.run("zoomClip"))),
      ...(clip.midi ? [] : [toolRow(button("tool", "Keep as a take", "Copy this clip into a new take lane, so you can record over it and still pick parts of it", () => daw("takes", { action: "fromClip", clip: id })))]),
      html("p", "clip-tools-hint", "Normalise, Strip silence and Split into stems change the audio itself: they are in the Audio menu and on the clip's right-click menu."));
    element.append(head, row, tools);

    return { element, update: (c) => {
      gain.update(c.gain); fadeIn.update(c.fadeIn); fadeOut.update(c.fadeOut);
      if (document.activeElement !== nameInput) nameInput.value = c.name;
      muteButton.setAttribute("aria-pressed", String(Boolean(c.mute)));
    } };
  }

  createTrackPanel(owner) {
    const element = html("section", "panel track-panel");
    const isMaster = owner.id === 0;
    const isReturn = Boolean(owner.isReturn);
    const head = html("div", "panel-head");
    element.append(head);
    head.append(html("h3", "", isMaster ? "Master effects" : isReturn ? `${owner.name} return effects` : `${owner.name} effects`),
                   html("p", "panel-caption", isMaster ? "Applied to the whole mix, after the tracks and returns."
                     : isReturn ? "Shared by every track that sends here. Keep effects fully wet: the dry sound stays on the tracks."
                     : owner.kind === "midi" ? `After the instrument (${owner.instrument?.name ?? "none"}). Change them any time.` : "The take is recorded dry. These play on top, and you can change them any time."));

    if (!isMaster) {
      const colours = html("div", "colour-row");
      TRACK_COLOURS.forEach((colour) => {
        const swatch = button("colour-swatch", "", "Track colour", () => daw("setTrack", { id: owner.id, colour }));
        swatch.style.background = colour;
        colours.append(swatch);
      });
      head.append(colours);
    }

    const list = html("div", "insert-list");
    this.rack = new InsertRack(list, owner);

    const add = html("select", "dropdown add-effect");
    add.append(new Option("+ Add effect…", ""));
    for (const [categoryId, category] of Object.entries(CATEGORIES)) {
      const group = document.createElement("optgroup");
      group.label = category.name;
      for (const module of MODULES.filter((m) => m.category === categoryId)) group.append(new Option(module.name, module.id));
      add.append(group);
    }
    add.addEventListener("change", () => {
      const moduleId = add.value;
      add.value = "";
      this.addEffect(owner.id, moduleId);
    });

    const preset = html("select", "dropdown load-preset");
    preset.append(new Option("Load a preset…", ""));
    (this.presets ?? []).forEach((p, index) => preset.append(new Option(`${p.category}: ${p.name}`, index)));
    preset.title = "Replace this chain with a preset's effects";
    preset.addEventListener("change", () => {
      if (preset.value !== "") daw("loadPreset", { track: owner.id, preset: Number(preset.value) });
      preset.value = "";
    });

    const footer = html("div", "panel-actions");
    footer.append(add, preset);
    const clipboard = this.effectsClipboard;
    const effectsRow = html("div", "panel-actions");
    effectsRow.append(button("tool", "Copy effects", "Copy this chain to paste on another track", () => this.copyEffects(owner.id)));
    if (clipboard && clipboard.id !== owner.id) {
      effectsRow.append(button("tool", `Paste from ${clipboard.name}`, `Replace this chain with the ${clipboard.count} effects copied from ${clipboard.name}`, () => this.pasteEffects(owner.id)));
    }
    if (!isMaster && !isReturn) effectsRow.append(button("tool", "Duplicate track", "A copy of this track with its clips and effects (Ctrl+Shift+D)", () => this.duplicateTrack(owner.id)));
    head.append(effectsRow);
    head.append(footer);



    // The chain's own input and output gain, with meters, either side of the effects.
    const chainGain = (name, key, caption) => {
      const column = html("div", `chain-gain is-${key}`);
      const knob = new ValueKnob({
        label: "Gain", min: -24, max: 24, value: owner[key] ?? 0, defaultValue: 0, bipolar: true, unit: "dB",
        onChange: (v, first) => daw(isMaster ? "setMaster" : "setTrack", { ...(isMaster ? {} : { id: owner.id }), [key]: v, continue: !first }),
      });
      const meter = new Level(true);
      column.title = caption;
      column.append(html("span", "chain-gain-title", name), knob.element, meter.element);
      return { column, knob, meter, key };
    };
    const input = chainGain("In", "inGain", "Chain input: the level going into the first effect");
    const output = chainGain("Out", "outGain", "Chain output: the level after the last effect, before the fader");
    this.chainGains = { id: owner.id, input, output };

    const chain = html("div", "chain-row");
    // Before the chain: the inputs an audio track records from.
    this.panelInput = null;
    if (!isMaster && !isReturn && owner.kind !== "midi") {
      const picker = this.createInputPicker(owner, { panel: true });
      this.panelInput = { id: owner.id, update: picker.update };
      const box = html("div", "panel-input");
      box.title = "Input: which inputs of your audio interface this track records and monitors";
      box.append(html("span", "chain-gain-title", "Input"), picker.element);
      chain.append(box);
    }
    chain.append(input.column, list, output.column);

    // After the chain: the track's sends to the returns, and where it plays.
    if (!isMaster && !isReturn && (this.session.returns ?? []).length) {
      const sends = this.createSends(owner);
      this.panelSends = { id: owner.id, update: sends.update };
      const box = html("div", "panel-sends");
      box.title = "Sends: feed the return tracks' shared effects";
      box.append(html("span", "chain-gain-title", "Sends"), sends.element);
      chain.append(box);
    }
    element.append(chain);
    this.rack.update(owner);
    return element;
  }

  //============================================================================ clip commands
  async splitSelected() {
    const found = this.findClip(this.selectedClip);
    if (!found) return toast("Select a clip to split.");
    const t = this.status.position;
    if (t <= found.clip.start || t >= found.clip.start + found.clip.length) return toast("Move the playhead inside the clip to split it.");
    await daw("split", { id: found.clip.id, time: t });
  }

  async duplicateSelected() {
    if (this.selectedClip == null) return;
    const id = await daw("duplicate", { id: this.selectedClip });
    if (id >= 0) this.select(this.selectedTrack, id);
  }

  deleteSelected() {
    if (this.selectedClip == null) return;
    daw("removeClip", { id: this.selectedClip });
    this.selectedClip = null;
  }

  //============================================================================ banners, export, toasts
  buildBanners() {
    this.banner = this.root.querySelector(".daw-banner");
    this.toastArea = this.root.querySelector(".daw-toast");
    this.exportPanel = this.root.querySelector(".export-panel");
    // toast (message, kind, action): action { label, run }, or "undo" for an Undo button
    // (the message then says what Ctrl+Z would bring back).
    toast = (message, kind = "", action = null) => {
      if (action === "undo") action = { label: "Undo", run: () => daw("undo") };
      this.toastArea.textContent = keyText(message);
      this.toastArea.className = `daw-toast is-visible ${kind}${action ? " has-action" : ""}`;
      if (action) {
        const b = button("toast-action", action.label, "", () => {
          this.toastArea.classList.remove("is-visible");
          action.run();
        });
        this.toastArea.append(b);
      }
      clearTimeout(this.toastTimer);
      this.toastTimer = setTimeout(() => this.toastArea.classList.remove("is-visible"), action ? 8000 : 5000);
    };

    const panel = this.exportPanel;
    panel.querySelector(".export-go").addEventListener("click", () => {
      const value = (name) => panel.querySelector(`[name=${name}]`);
      daw("export", {
        kind: value("kind").value,
        bitDepth: Number(value("bitDepth").value),
        normalise: value("normalise").checked,
        tail: Number(value("tail").value),
        loopOnly: value("loopOnly").checked,
      });
    });
    panel.querySelector(".export-cancel").addEventListener("click", () => daw("cancelExport"));
    panel.querySelector(".export-close").addEventListener("click", () => this.toggleExportPanel(false));
  }

  toggleExportPanel(show = this.exportPanel.hidden) {
    this.exportPanel.hidden = !show;
    if (show && this.autoMixPanel) this.autoMixPanel.hidden = true;
    if (show && this.stemsPanel) this.stemsPanel.hidden = true;
    if (show && this.keyPanel) this.keyPanel.hidden = true;
    if (show) {
      this.exportPanel.querySelector("[name=loopOnly]").disabled = !this.session?.loop.on;
      this.exportPanel.querySelector(".export-result").replaceChildren();
    }
  }

  onExport(progress) {
    if (progress.done) this.endJob("export");
    else this.setJob("export", { label: "Export", progress: progress.progress ?? 0, cancel: () => daw("cancelExport"), open: () => this.toggleExportPanel(true) });
    const panel = this.exportPanel;
    panel.hidden = false;
    panel.classList.toggle("is-running", !progress.done);
    panel.querySelector(".export-bar").style.setProperty("--progress", progress.done ? 1 : progress.progress ?? 0);

    if (!progress.done) return;
    const result = panel.querySelector(".export-result");
    result.replaceChildren();

    if (progress.cancelled) result.append(html("span", "", "Export cancelled."));
    else if (progress.error) result.append(html("span", "is-error", progress.error));
    else {
      const count = progress.files.length;
      result.append(html("span", "", `Exported ${count === 1 ? "the mix" : `${count} stems`} (peak ${progress.peak.toFixed(1)} dBFS). `),
                    button("text-button", "Show in folder", "", () => daw("reveal", { path: progress.files[0] })));
    }
  }

  //============================================================================ Split into stems
  buildStems() {
    const panel = (this.stemsPanel = this.root.querySelector(".stems-panel"));
    this.stems = { clip: null, running: false };
    panel.querySelector(".stems-close").addEventListener("click", () => this.toggleStems(false));
    panel.querySelector(".stems-cancel").addEventListener("click", () => daw("stems", { action: "cancel" }));
    panel.querySelector(".stems-go").addEventListener("click", async () => {
      if (!this.stems.clip) return;
      this.setStemsStatus("Starting…");
      const result = await daw("stems", { action: "start", id: this.stems.clip.id });
      if (result?.ok === false) this.setStemsStatus(result.error, true);
    });
  }

  toggleStems(show, clip = null) {
    const panel = this.stemsPanel;
    panel.hidden = !show;
    if (!show) return;
    if (this.exportPanel) this.exportPanel.hidden = true;
    if (this.autoMixPanel) this.autoMixPanel.hidden = true;
    if (!this.stems.running) {
      this.stems.clip = clip;
      panel.querySelector("strong").textContent = clip ? `Split “${clip.name}” into stems` : "Split into stems";
      this.setStemsStatus("");
    }
  }

  setStemsStatus(text, isError = false) {
    const status = this.stemsPanel.querySelector(".stems-status");
    status.replaceChildren(html("span", isError ? "is-error" : "", text));
  }

  onStems(event) {
    const panel = this.stemsPanel;
    const running = ["downloading", "loading", "splitting"].includes(event.state);
    this.stems.running = running;
    panel.classList.toggle("is-running", running);
    if (running) this.setJob("stems", { label: "Split into stems", progress: event.state === "splitting" ? event.progress ?? 0 : null,
                                        cancel: () => daw("stems", { action: "cancel" }), open: () => { panel.hidden = false; } });
    else this.endJob("stems");

    if (running) {
      panel.hidden = false;
      const progress = event.progress ?? 0;
      panel.querySelector(".export-bar").style.setProperty("--progress", event.state === "loading" ? 0 : progress);
      const percent = `${Math.round(progress * 100)}%`;
      let left = "";
      if (event.state === "splitting") {
        // Time left, from how long the first part took, once there's enough to go on.
        this.stems.splitStart ??= performance.now();
        const elapsed = (performance.now() - this.stems.splitStart) / 1000;
        if (progress > 0.03 && elapsed > 5) left = `, about ${spokenDuration(elapsed * (1 - progress) / progress)} left`;
      } else {
        this.stems.splitStart = null;
      }
      this.setStemsStatus(event.state === "downloading" ? `Downloading the separation model… ${percent}`
                        : event.state === "loading" ? "Loading the separation model…"
                        : `Separating${event.gpu ? " on the GPU" : ""}… ${percent}${left}`);
      return;
    }

    panel.querySelector(".export-bar").style.setProperty("--progress", 0);
    if (event.state === "done") {
      panel.hidden = true;
      this.stems.splitStart = null;
      const took = event.seconds ? ` (separating took ${spokenDuration(event.seconds)})` : "";
      toast(`Added ${event.tracks?.length ?? 0} stem tracks and muted the original clip${took}${event.gpu ? ", on the GPU" : ""}.${event.note ? ` ${event.note}` : ""}`, "", "undo");
    } else if (event.state === "cancelled") {
      this.setStemsStatus("Cancelled.");
    } else if (event.state === "error") {
      this.setStemsStatus(event.error || "The split failed.", true);
    }
  }

  //============================================================================ Auto Mix
  buildAutoMix() {
    const panel = (this.autoMixPanel = this.root.querySelector(".automix-panel"));
    const $ = (selector) => panel.querySelector(selector);
    this.autoMix = { plan: null, applied: false, after: true };

    const style = () => $("[name=style]").value;
    const amount = () => Number($("[name=amount]").value) / 100;
    const roles = () => Object.fromEntries([...panel.querySelectorAll(".automix-role select")].map((s) => [s.dataset.track, s.value]));
    const effects = () => Object.fromEntries([...panel.querySelectorAll(".automix-effects input")].map((c) => [c.name, c.checked]));
    this.autoMixEffects = effects;
    const mode = () => ({ mode: $("[name=mode]").value, focus: Number($("[name=focus]").value), masterLufs: Number($("[name=masterLufs]").value) });
    this.autoMixMode = mode;
    const planArgs = () => ({ style: style(), roles: roles(), effects: effects(), ...mode() });

    $(".automix-close").addEventListener("click", () => this.toggleAutoMix(false));
    $(".automix-analyse").addEventListener("click", async () => {
      this.setAutoMixResult("");
      // First the comps: the best parts of every track's takes, then the mix of those.
      if (effects().comp && this.session?.tracks.some((t) => (t.takes?.length ?? 0) >= 2)) {
        this.setAutoMixResult("Picking the best takes…");
        if (!(await this.takes.autoComp(-1))) return this.setAutoMixResult("");
        this.setAutoMixResult("");
      }
      daw("autoMix", { action: "analyse", style: style(), effects: effects(), ...mode() });
    });
    $(".automix-reference").addEventListener("click", () => {
      this.setAutoMixResult("");
      daw("autoMix", { action: "reference" });
    });
    $(".automix-cancel").addEventListener("click", () => daw("autoMix", { action: "cancel" }));
    $("[name=style]").addEventListener("change", () => {
      if (this.autoMix.plan?.kind === "mix") this.replanAutoMix(planArgs());
    });
    // Mix the song, fit one track into the backing, or clean one up: the plan follows.
    for (const name of ["mode", "focus", "masterLufs"])
      $(`[name=${name}]`).addEventListener("change", () => {
        this.renderAutoMixMode();
        if (this.autoMix.plan?.kind === "mix") this.replanAutoMix(planArgs());
      });
    // Which effects it may add: the plan follows at once.
    for (const box of panel.querySelectorAll(".automix-effects input"))
      box.addEventListener("change", () => {
        if (this.autoMix.plan?.kind === "mix") this.replanAutoMix(planArgs());
      });

    $(".automix-apply").addEventListener("click", async () => {
      const result = await daw("autoMix", { action: "apply", amount: amount() });
      if (result?.ok === false) return;
      Object.assign(this.autoMix, { applied: true, after: true });
      this.renderAutoMixControls();
      this.setAutoMixResult("Applied. Play the song, then switch between before and after.");
    });

    // After applying, the amount re-applies the same plan, gentler or stronger.
    const slider = $("[name=amount]");
    slider.addEventListener("input", () => { $(".automix-amount-value").textContent = `${slider.value}%`; });
    slider.addEventListener("change", () => {
      if (!this.autoMix.applied) return;
      daw("autoMix", { action: "apply", amount: amount(), replace: true });
      this.autoMix.after = true;
      this.renderAutoMixControls();
    });

    $(".automix-compare").addEventListener("click", async () => {
      const result = await daw("autoMix", { action: "compare", after: !this.autoMix.after });
      if (result?.ok) this.autoMix.after = result.after;
      this.renderAutoMixControls();
    });

    window.__JUCE__.backend.addEventListener("dawAutoMix", (event) => this.onAutoMix(event));
  }

  toggleAutoMix(show = this.autoMixPanel.hidden) {
    this.autoMixPanel.hidden = !show;
    if (show) {
      this.toggleExportPanel(false);
      this.renderAutoMixMode();
      if (!this.autoMix.plan) this.refreshAutoMixTags(true);
    }
  }

  // The mode's own line, and the track it works on (fit and clean only), listed from the session.
  renderAutoMixMode() {
    const panel = this.autoMixPanel;
    const mode = panel.querySelector("[name=mode]").value;
    const focus = panel.querySelector("[name=focus]");
    const selected = focus.value;
    focus.replaceChildren(new Option("The lead voice", "0"));
    for (const track of this.session?.tracks ?? []) focus.append(new Option(track.name, String(track.id)));
    focus.value = [...focus.options].some((o) => o.value === selected) ? selected : "0";

    panel.querySelector(".automix-focus").hidden = mode === "mix" || mode === "master";
    panel.querySelector(".automix-target").hidden = mode !== "master";
    panel.classList.toggle("is-clean", mode === "clean");
    panel.classList.toggle("is-master", mode === "master");
    const intro = panel.querySelector(".automix-mode-intro");
    intro.hidden = mode === "mix";
    intro.textContent = mode === "fit"
      ? "For a backing that's already mixed: this track gets its EQ, compression, space and level; the others only make room for it (a dip where they cover it, a little lower while it plays). The master stays as it is."
      : mode === "clean"
        ? "Cleans up this track only: rumble, mains hum, noise between phrases, mud, harshness, S's and uneven level. No reverb, no level changes, no other tracks."
        : mode === "master"
          ? "Masters the song as it plays now: a mastering EQ towards the balance of finished records, a gentle multiband, width with a mono low end, and a limiter up to the loudness you pick. Listen again after changing the mix."
          : "";
  }

  // Before listening: what each track is, guessed from its name or tagged by you. A tag
  // is saved with the project, and the plan uses it instead of guessing.
  async refreshAutoMixTags(force = false) {
    const key = (this.session?.tracks ?? []).map((t) => `${t.id}:${t.name}`).join("|");
    if (!force && key === this.autoMixTagsKey) return;
    this.autoMixTagsKey = key;
    const result = await daw("autoMix", { action: "tags" });
    if (!result?.ok || this.autoMix.plan) return;
    this.autoMixRoleNames = result.roles;
    this.renderAutoMixRoles(result.tracks.map((t) => ({ ...t, detail: t.tagged ? "tagged" : t.role ? (t.midi ? "from its name or instrument" : "from its name") : "" })), true);
  }

  // One row per track with its role. Before listening a pick tags the track; after, it
  // also replans.
  renderAutoMixRoles(tracks, beforeListening) {
    const panel = this.autoMixPanel;
    const rolesBox = panel.querySelector(".automix-roles");
    rolesBox.replaceChildren();
    if (tracks.length === 0) return;
    rolesBox.append(html("div", "automix-roles-head", beforeListening
      ? "What's on each track? Tag them so Auto Mix treats each one right."
      : "Tracks (change a role and the plan follows)"));

    const names = this.autoMixRoleNames ?? AUTOMIX_ROLES;
    for (const track of tracks) {
      const row = html("label", "automix-role");
      const name = html("span", "", track.name);
      if (track.detail) name.append(html("small", "", track.detail));
      const select = html("select", "dropdown");
      select.dataset.track = track.id;
      if (beforeListening) select.append(new Option("Not sure: listen", "auto"));
      for (const role of names) select.append(new Option(role.name, role.key));
      select.value = track.role || "auto";
      select.addEventListener("change", async () => {
        if (beforeListening) {
          await daw("autoMix", { action: "tag", track: track.id, role: select.value });
          this.refreshAutoMixTags(true);
          return;
        }
        const roles = Object.fromEntries([...rolesBox.querySelectorAll("select")].map((s) => [s.dataset.track, s.value]));
        this.replanAutoMix({ style: panel.querySelector("[name=style]").value, roles, effects: this.autoMixEffects(), ...this.autoMixMode() });
      });
      row.append(name, select);
      rolesBox.append(row);
    }
  }

  //============================================================================ Song key
  buildKeyPanel() {
    const panel = (this.keyPanel = this.root.querySelector(".key-panel"));
    const $ = (selector) => panel.querySelector(selector);
    this.keyResult = null;

    const tonic = $("[name=tonic]"), mode = $("[name=mode]");
    tonic.append(new Option("Not set", "-1"), ...NOTE_NAMES.map((name, i) => new Option(name, String(i))));
    const pick = () => daw("key", { action: "set", key: Number(tonic.value), minor: mode.value === "minor" });
    tonic.addEventListener("change", pick);
    mode.addEventListener("change", pick);

    $(".key-close").addEventListener("click", () => this.toggleKeyPanel(false));
    $(".key-detect").addEventListener("click", () => {
      this.setKeyMessage("");
      daw("key", { action: "detect" });
    });
    $(".key-cancel").addEventListener("click", () => daw("key", { action: "cancel" }));
    $(".key-autotune").addEventListener("click", async () => {
      const result = await daw("key", { action: "autotune" });
      if (!result?.ok) return;
      const name = this.session?.key?.name ?? "the key";
      this.setKeyMessage(result.count === 0
        ? `No autotune yet. Add Autotune to your vocal track's effects: it starts in ${name}.`
        : `${result.count === 1 ? "The autotune is" : `${result.count} autotunes are`} now in ${name}. Ctrl+Z undoes it.`);
    });

    window.__JUCE__.backend.addEventListener("dawKey", (event) => this.onSongKey(event));
  }

  toggleKeyPanel(show = this.keyPanel.hidden) {
    this.keyPanel.hidden = !show;
    if (show) {
      this.toggleExportPanel(false);
      this.autoMixPanel.hidden = true;
      this.renderKeyPanel();
    }
  }

  setKeyMessage(text, isError = false) {
    this.keyPanel.querySelector(".key-message").replaceChildren(text ? html("span", isError ? "is-error" : "", text) : "");
  }

  onSongKey(event) {
    const panel = this.keyPanel;
    panel.hidden = false;
    panel.classList.toggle("is-running", event.state === "analysing");
    panel.querySelector(".export-bar").style.setProperty("--progress", event.progress ?? 1);
    if (event.state === "analysing")
      this.setJob("key", { label: "Key detection", progress: event.progress ?? null, cancel: () => daw("key", { action: "cancel" }), open: () => this.toggleKeyPanel(true) });
    else this.endJob("key");

    if (event.state === "analysing") return this.setKeyMessage("Listening…");
    if (event.state === "cancelled") return this.setKeyMessage("Cancelled.");
    if (event.state !== "ready") return;

    this.keyResult = event;
    this.setKeyMessage(event.key < 0 ? "No clear key: the tracks have little harmony (drums or speech only?). Pick it by hand." : "");
    this.renderKeyPanel();
  }

  // The session's key (detected or picked), with the last detection's details.
  renderKeyPanel() {
    const panel = this.keyPanel;
    if (!panel || panel.hidden || !this.session) return;
    const key = this.session.key ?? { key: -1, minor: false, name: "" };
    const $ = (selector) => panel.querySelector(selector);

    if (document.activeElement !== $("[name=tonic]")) $("[name=tonic]").value = String(key.key);
    $("[name=mode]").value = key.minor ? "minor" : "major";
    $("[name=mode]").disabled = key.key < 0;
    $(".key-autotune").disabled = key.key < 0;
    $(".key-detect").textContent = this.keyResult ? "Detect again" : "Detect";

    const detected = this.keyResult?.state === "ready" && this.keyResult.key >= 0 ? this.keyResult : null;
    const summary = $(".key-summary");
    summary.replaceChildren();
    summary.append(html("div", "key-name", key.key >= 0 ? key.name : "Key not set"));

    if (detected) {
      const sure = detected.confidence >= 0.6 ? "Clear" : detected.confidence >= 0.25 ? "Likely" : "Unsure";
      const same = detected.key === key.key && detected.minor === key.minor;
      let detail = same ? `${sure} match from the music` : `Detected ${detected.name} (${sure.toLowerCase()}); you picked another key`;
      if (detected.confidence < 0.6 && detected.runnerUp) detail += `. Could also be ${detected.runnerUp}`;
      summary.append(html("div", "key-detail", detail + "."));
      if (Math.abs(detected.tuningCents) >= 8) {
        const a = 440 * Math.pow(2, detected.tuningCents / 1200);
        summary.append(html("div", "key-detail",
          `Tuned ${detected.tuningCents > 0 ? "+" : "−"}${Math.abs(detected.tuningCents).toFixed(0)} cents from standard (A ≈ ${a.toFixed(1)} Hz). The autotune uses A = 440 Hz.`));
      }
    } else if (key.key < 0) {
      summary.append(html("div", "key-detail", "Press Detect to listen to the tracks, or pick it below."));
    }

    // Which notes the music uses, with the key's scale lit up.
    const chroma = $(".key-chroma");
    chroma.replaceChildren();
    chroma.hidden = !detected;
    if (detected) {
      const steps = key.key >= 0 ? SCALE_STEPS[key.minor ? "minor" : "major"].map((step) => (step + key.key) % 12) : [];
      NOTE_NAMES.forEach((name, note) => {
        const column = html("div", `key-note${steps.includes(note) ? " in-scale" : ""}${note === key.key ? " is-tonic" : ""}`);
        const bar = html("div", "key-bar");
        bar.style.setProperty("--level", Math.max(0.02, Number(detected.chroma[note]) || 0));
        column.title = `${name}: ${Math.round((Number(detected.chroma[note]) || 0) * 100)}%`;
        column.append(bar, html("span", "", name));
        chroma.append(column);
      });
    }

    const tracks = $(".key-tracks");
    tracks.replaceChildren();
    for (const track of detected?.tracks ?? []) {
      const row = html("div", `key-track${track.used ? "" : " is-unused"}`);
      row.append(html("span", "", track.name),
                 html("small", "", !track.key ? "no harmony" : track.used ? track.key : `${track.key} · voice, left out`));
      tracks.append(row);
    }
  }

  async replanAutoMix(args) {
    const result = await daw("autoMix", { action: "replan", ...args });
    if (result?.state === "ready") this.onAutoMix(result);
  }

  setAutoMixResult(text, isError = false) {
    const result = this.autoMixPanel.querySelector(".automix-result");
    result.replaceChildren(text ? html("span", isError ? "is-error" : "", text) : "");
  }

  onAutoMix(event) {
    const panel = this.autoMixPanel;
    panel.hidden = false;
    panel.classList.toggle("is-running", event.state === "analysing");
    panel.querySelector(".export-bar").style.setProperty("--progress", event.progress ?? 1);

    panel.classList.toggle("is-running", event.state === "analysing" || event.state === "splitting");
    if (event.state === "analysing" || event.state === "splitting")
      this.setJob("automix", { label: "Auto Mix", progress: event.progress ?? null, cancel: () => daw("autoMix", { action: "cancel" }), open: () => this.toggleAutoMix(true) });
    else this.endJob("automix");
    if (event.state === "splitting")
      return this.setAutoMixResult(event.phase === 0 ? "Downloading the stem model…" : event.phase === 1 ? "Loading the stem model…" : "Splitting the song into stems…");
    if (event.state === "analysing") return this.setAutoMixResult("Listening…");
    if (event.state === "cancelled") return this.setAutoMixResult("Cancelled.");
    if (event.state === "error") return this.setAutoMixResult(event.error, true);
    if (event.state !== "ready") return;

    Object.assign(this.autoMix, { plan: event, applied: false, after: true });
    this.setAutoMixResult("");
    if (event.style) panel.querySelector("[name=style]").value = event.style;
    if (event.mode) {
      panel.querySelector("[name=mode]").value = event.mode;
      this.renderAutoMixMode();
      const focus = panel.querySelector("[name=focus]");
      if ([...focus.options].some((o) => o.value === String(event.focus ?? 0))) focus.value = String(event.focus ?? 0);
      if (event.masterLufs) panel.querySelector("[name=masterLufs]").value = String(event.masterLufs);
    }
    for (const [name, on] of Object.entries(event.effects ?? {})) {
      const box = panel.querySelector(`.automix-effects [name=${name}]`);
      if (box) box.checked = Boolean(on);
    }

    // Who is who: change a role and the plan follows.
    if (event.kind === "mix")
      this.renderAutoMixRoles(event.tracks.map((t) => ({ ...t, detail: t.active > 0 ? `${t.lufs.toFixed(1)} LUFS` : "silent" })), false);
    else
      this.autoMixPanel.querySelector(".automix-roles").replaceChildren();

    // What it will change, and why.
    const planBox = panel.querySelector(".automix-plan");
    planBox.replaceChildren();
    if (event.kind === "reference") planBox.append(html("div", "automix-summary", `Matching the master to “${event.reference}”.`));
    for (const note of event.notes) planBox.append(html("div", "automix-note", note));
    for (const move of event.moves) {
      const card = html("section", "automix-move");
      const head = html("header");
      head.append(html("span", "", move.name), html("span", "", move.effect));
      const list = html("ul");
      for (const reason of move.reasons) list.append(html("li", "", reason));
      card.append(head, list);
      planBox.append(card);
    }
    if (event.kind === "mix" && event.mode === "master" && event.moves.length > 0)
      planBox.append(html("div", "automix-summary", `Master about ${event.mixLufs.toFixed(1)} LUFS, peaks under -1 dB`));
    else if (event.kind === "mix" && event.mode === "clean") {
      // No levels to sum up: it only cleaned one track.
    } else if (event.kind === "mix" && event.mode === "fit" && event.moves.length > 0)
      planBox.append(html("div", "automix-summary",
        `${event.voiceOverBackingLu !== 0 ? `Voice ${event.voiceOverBackingLu >= 0 ? "+" : ""}${event.voiceOverBackingLu.toFixed(1)} LU over the backing · ` : ""}mix about ${event.mixLufs.toFixed(1)} LUFS, master unchanged`));
    else if (event.kind === "mix" && event.moves.length > 0)
      planBox.append(html("div", "automix-summary",
        `Voice ${event.voiceOverBackingLu >= 0 ? "+" : ""}${event.voiceOverBackingLu.toFixed(1)} LU over the backing · mix about ${event.mixLufs.toFixed(1)} LUFS`));
    else if (event.kind === "reference" && event.mixLufs > -150)
      planBox.append(html("div", "automix-summary", `Mix about ${event.mixLufs.toFixed(1)} LUFS`));

    this.renderAutoMixControls();
  }

  renderAutoMixControls() {
    const panel = this.autoMixPanel;
    const { plan, applied, after } = this.autoMix;
    const apply = panel.querySelector(".automix-apply");
    apply.disabled = !plan || plan.moves.length === 0 || applied;
    apply.textContent = applied ? "Applied" : "Apply";
    panel.querySelector(".automix-analyse").textContent = plan ? "Listen again" : "Listen";
    panel.querySelector(".automix-amount").hidden = !plan || plan.moves.length === 0;
    const compare = panel.querySelector(".automix-compare");
    compare.hidden = !applied;
    compare.textContent = after ? "Hear before" : "Hear after";
    compare.setAttribute("aria-pressed", String(!after));
  }

  renderBanner() {
    const project = this.session.project;
    this.banner.replaceChildren();
    this.banner.hidden = true;

    if (project.recoverable) {
      this.banner.hidden = false;
      this.banner.append(html("span", "", "MicFX didn't close properly last time. There's an autosave newer than your last save."),
                         button("tool is-accent", "Restore autosave", "", () => daw("restore")),
                         button("tool", "Keep saved version", "", () => daw("dismissRestore")));
    } else if (project.missing > 0) {
      this.banner.hidden = false;
      this.banner.append(html("span", "", `${project.missing} audio file${project.missing === 1 ? " is" : "s are"} missing (moved or deleted outside MicFX). Those clips play silence.`),
                         button("tool", "Show project folder", "", () => daw("reveal", { path: project.path })));
    }
  }

  //============================================================================ events
  onSession(session) {
    if (this.dragging) {
      this.pendingSession = session; // redraw after the drag, not under the mouse
      return;
    }
    this.session = session;
    // After New or Open the old selection points at tracks that are gone: fall back to the
    // first track, so Import and the effects panel act on something you can see.
    const tracks = session.tracks ?? [];
    if (this.reveal && tracks.some((t) => t.id === this.reveal.track)) {
      this.selectedTrack = this.reveal.track;
      this.selectedClip = this.reveal.clip ?? null;
    } else if (this.selectedTrack !== 0 && !tracks.some((t) => t.id === this.selectedTrack)) {
      this.selectedTrack = tracks[0]?.id ?? null;
      this.selectedClip = null;
    }
    // Another project: start at its top-left, not where the last one was scrolled to.
    const path = session.project?.path ?? "";
    const pathChanged = this.projectPath !== undefined && path !== this.projectPath;
    if (this.projectPath !== undefined && path !== this.projectPath) this.arrange.scrollTop = this.arrange.scrollLeft = 0;
    const opened = path !== this.projectPath; // also the first session after starting
    this.projectPath = path;
    const scroll = opened ? this.restoreView(session.view) : null;
    if (pathChanged) this.library?.refresh();
    document.dispatchEvent(new CustomEvent("daw-session", { detail: session }));
    if (this.autoMixPanel && !this.autoMixPanel.hidden && !this.autoMix?.plan) this.refreshAutoMixTags();
    if (this.active) this.render();
    if (scroll && this.active) {
      this.renderArrange(true);
      this.restoringView = true;
      this.arrange.scrollTop = scroll.top;
      this.scrollTo(scroll.left);
      this.restoringView = false;
    }
    for (const [id, resolve] of this.trackWaiters) if (tracks.some((t) => t.id === id)) { this.trackWaiters.delete(id); resolve(); }
  }

  // Resolves once a session holding the track has arrived (a command's reply comes
  // before the session that shows its result).
  waitForTrack(id) {
    if (this.session?.tracks.some((t) => t.id === id)) return Promise.resolve();
    return new Promise((resolve) => {
      this.trackWaiters.set(id, resolve);
      setTimeout(() => { if (this.trackWaiters.get(id) === resolve) { this.trackWaiters.delete(id); resolve(); } }, 2000);
    });
  }

  //============================================================================ library
  // The sidebar on the left (ui/browser.js): it sits beside the tracks, mixer and
  // effects panel.
  buildLibrary() {
    const main = this.root.querySelector(".daw-main");
    const body = html("div", "daw-body");
    main.replaceWith(body);
    this.library = new LibraryBrowser(this, {
      call: (command, args = {}) => nativeDaw(command, args),
      run: (command, args) => {
        if (command === "scanPlugins") {
          this.setJob("scan", { label: "Plug-in scan", progress: null });
          clearTimeout(this.scanTimer);
          this.scanTimer = setTimeout(() => this.endJob("scan"), 180000); // in case no reply comes
        }
        return daw(command, args);
      },
      toast: (message, kind) => toast(message, kind),
      onEvent: (name, handler) => window.__JUCE__.backend.addEventListener(name, handler),
    });
    body.append(this.library.element, main);
    window.__JUCE__.backend.addEventListener("dawPlugins", () => this.endJob("scan"));
    // The toolbar's library switch follows the library however it is folded.
    const setCollapsed = this.library.setCollapsed.bind(this.library);
    this.library.setCollapsed = (collapsed) => { setCollapsed(collapsed); this.renderTransport(); this.rememberView(); };
  }

  // Adds an effect (a modules.js id) at the end of a track's (or the master's) chain.
  async addEffect(trackId, moduleId) {
    const module = MODULES.find((m) => m.id === moduleId);
    if (!module) return -1;
    const id = await daw("addInsert", { track: trackId, type: MODULE_KEYS[module.id] });
    // The pitch insert switches on autotune by default; switch to the part that was picked.
    if (id >= 0 && MODULE_KEYS[module.id] === "pitch" && module.id !== "tune") {
      await daw("setInsert", { id, param: "tuneOn", value: 0, continue: true });
      await daw("setInsert", { id, param: module.power, value: 1, continue: true });
    }
    return id;
  }

  flushSession() {
    if (this.pendingSession) {
      const session = this.pendingSession;
      this.pendingSession = null;
      this.onSession(session);
    } else if (this.session) {
      this.render();
    }
  }

  render() {
    if (!this.session) return;
    this.root.classList.toggle("show-mixer", this.view === "mixer");
    this.renderTransport();
    this.renderBanner();
    if (!this.dragging) this.renderArrange();
    if (this.view === "mixer") this.renderMixer();
    this.renderInspector();
  }

  onStatus(status) {
    const lengthChanged = Math.abs((status.length ?? 0) - (this.status.length ?? 0)) > 1e-3;
    if (status.recording && !this.status.recording) {
      this.recLevels = [];
      this.recPass = { number: 1, start: null };
    }
    // Loop recording: the playhead jumped back to the loop start, so a new pass began.
    if (status.recording && this.status.recording && status.position < this.status.position - 0.05
        && this.status.position >= (status.recordStart ?? 0) && this.session?.loop?.on) {
      this.recLevels = [];
      this.recPass = { number: (this.recPass?.number ?? 1) + 1, start: this.session.loop.start };
    }
    if (status.recording && status.position >= (this.recPass?.start ?? status.recordStart ?? 0))
      this.recLevels.push([status.position, Math.pow(10, (status.input ?? -100) / 20)]);
    if (!status.recording) this.recLevels = [];
    this.status = status;
    if (this.watchedInserts >= 0) this.rack?.live(status.inserts);
    this.playButton.classList.toggle("is-playing", status.playing);
    this.root.classList.toggle("is-playing", status.playing);
    this.recordButton.setAttribute("aria-pressed", String(status.recording));
    this.root.classList.toggle("is-recording", status.recording);
    this.clockBars.textContent = this.barsBeats(status.position);
    this.clockTime.textContent = formatDuration(status.position);
    this.loading = status.loading;

    const neededWidth = this.headWidth + this.timelineSeconds() * this.zoom;
    if ((lengthChanged || neededWidth > this.inner.offsetWidth + 1) && !this.dragging) this.renderArrange();
    else this.movePlayhead();
    if (status.recording || this.rows.querySelector(".rec-region")) this.updateRecording();
    this.pianoRoll?.setStatus(status);
  }

  tickMeters(elapsed) {
    const level = (id) => (id === 0 ? this.status.master : this.status.tracks?.[id]);
    const stereo = (id) => this.status.stereo?.[id] ?? [level(id), level(id)];
    for (const [id, meter] of this.meters) meter.push(meter instanceof StereoLevel ? stereo(id) : level(id), elapsed);
    this.inputMeter.push(this.status.input, elapsed);
    if (this.status.input !== undefined) {
      this.inputPeak = Math.max(this.status.input, (this.inputPeak ?? -100) - elapsed * 0.01);
      this.inputReadout.textContent = this.inputPeak <= METER_FLOOR_DB ? "−∞" : this.inputPeak.toFixed(0);
    }
    this.status.input = undefined;
    if (this.chainGains) {
      const chain = this.status.chains?.[this.chainGains.id];
      this.chainGains.input.meter.push(chain?.in, elapsed);
      this.chainGains.output.meter.push(chain?.out, elapsed);
      this.status.chains = undefined;
    }
    if (this.view === "mixer") for (const [id, strip] of this.strips) strip.meter.push(stereo(id), elapsed);
    this.status.master = undefined;
    if (this.status.tracks) this.status.tracks = {};
    this.status.stereo = undefined;
  }

  //============================================================================ keyboard
  // Every shortcut, for the "?" overlay. Keep in step with onKey. The bindings follow
  // what Ableton Live, Cubase and Logic Pro share (Space, M, S, R, Ctrl+Z/S/D/N/O, Q...);
  // where they differ, the most obvious one wins. Ctrl is Cmd on a Mac (keyText).
  static SHORTCUTS = [
    ["Transport", [["Space", "Play / pause"], ["Enter / Num 0", "Stop and go back to the start"], ["R / Num *", "Record on the armed tracks"],
                   ["← / →", "Move the playhead a beat (Shift: a bar)"], ["Home / End", "Go to the start / end of the song"],
                   ["L / Num /", "Loop on / off"], ["C", "Click (metronome) on / off"], ["F", "Follow the playhead on / off"],
                   ["K / Insert", "Add a marker at the playhead"], ["[ / ]", "Previous / next marker"], ["T", "Tap tempo"],
                   ["Click the clock", "Type a bar or time to go to"]]],
    ["Tracks", [["↑ / ↓", "Select the track above / below"], ["Ctrl+T", "New track"], ["Ctrl+Shift+T", "New MIDI track"], ["Ctrl+Shift+D", "Duplicate the selected track"],
                ["Ctrl+R", "Rename the selected track"], ["Ctrl+Delete", "Delete the selected track"],
                ["0", "Mute the selected track"], ["S", "Solo the selected track"], ["Alt+click S", "Solo only that track"],
                ["A", "Arm the selected track"], ["I", "Monitor the mic on the selected track (In / Off; the header button also has Auto)"],
                ["Double-click a value / Enter", "Type a value (volume, pan, sends, knobs)"], ["Ctrl+click a slider", "Reset it"],
                ["Q / Ctrl+U", "Quantize the selected clip (or track), with the Pitch & time settings"]]],
    ["Computer keyboard piano", [["M", "Turn the computer keyboard into a MIDI keyboard on / off (toolbar piano button)"],
                                 ["A S D F G H J K", "White keys, C to C (; adds one more)"], ["W E T Y U O P", "Black keys"],
                                 ["Z / X", "Octave down / up"], ["C / V", "Velocity down / up"],
                                 ["While on", "Opens a floating piano and plays the armed (or selected) MIDI track. Single-key shortcuts (R, Space, S...) are off; Ctrl / Cmd ones still work. Esc closes it."]]],
    ["Clips", [[", / .", "Select the previous / next clip on the track"], ["Ctrl+E", "Split the selected clip at the playhead"],
               ["Alt+← / →", "Nudge the selected clip a beat (Shift: a bar)"], ["Ctrl+D", "Duplicate"],
               ["Ctrl+X / C / V", "Cut, copy, paste (pastes at the playhead on the selected track)"], ["Delete", "Delete the clip"],
               ["V / B / G / W", "Select, cut, gain and warp tools"], ["Shift+M", "Mute / unmute the selected clip"],
               ["Ctrl+L", "Loop the selected clip"], ["Alt+drag", "Move or trim without snapping"]]],
    ["File", [["Ctrl+N", "New project"], ["Ctrl+O", "Open a project"], ["Ctrl+S", "Save (Shift: save as)"],
              ["Ctrl+I", "Import audio"], ["Ctrl+Shift+R / Ctrl+B", "Export (bounce)"], ["Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"]]],
    ["View", [["Tab / X", "Tracks / mixer"], ["E / Shift+Tab", "Show / hide the effects panel"], ["Shift+B / Ctrl+Alt+B", "Show / hide the library"],
              ["Ctrl+F", "Search the library"], ["+ / −", "Zoom in / out (or Ctrl+scroll)"],
              ["Z / Shift+Z", "Zoom to the selected clip / the whole song"], ["Shift+↑ / ↓", "Taller / shorter tracks (or Alt+scroll)"],
              ["?", "This list"], [IS_MAC ? "Ctrl+Cmd+F" : "F11", "Full screen"], ["Esc", "Close, deselect, back to the select tool"]]],
  ];

  toggleShortcuts(show) {
    let overlay = this.root.querySelector(".shortcuts");
    if (!overlay) {
      overlay = html("div", "shortcuts");
      overlay.hidden = true;
      const panel = html("div", "shortcuts-panel");
      const head = html("div", "export-head");
      head.append(html("strong", "", "Keyboard shortcuts"), button("mini", "×", "Close", () => this.toggleShortcuts(false)));
      const grid = html("div", "shortcuts-grid");
      for (const [group, rows] of DawScreen.SHORTCUTS) {
        const section = html("section");
        section.append(html("h4", "", group));
        for (const [keys, action] of rows) {
          const row = html("div", "shortcut");
          row.append(html("kbd", "", keyText(keys)), html("span", "", action));
          section.append(row);
        }
        grid.append(section);
      }
      panel.append(head, grid);
      overlay.append(panel);
      overlay.addEventListener("pointerdown", (event) => { if (event.target === overlay) this.toggleShortcuts(false); });
      this.root.append(overlay);
    }
    overlay.hidden = !(show ?? overlay.hidden);
  }

  selectedTrackObject() {
    return this.session?.tracks.find((t) => t.id === this.selectedTrack);
  }

  seek(seconds) {
    daw("transport", { action: "seek", time: Math.max(0, seconds) });
    this.status.position = Math.max(0, seconds); // so repeated key presses add up before the next status
    this.movePlayhead(true);
  }

  songEnd() {
    let end = 0;
    for (const track of this.session?.tracks ?? []) for (const clip of track.clips) end = Math.max(end, clip.start + clip.length);
    return end;
  }

  selectTrackBy(step) {
    const tracks = this.session?.tracks ?? [];
    if (!tracks.length) return;
    const index = tracks.findIndex((t) => t.id === this.selectedTrack);
    const next = tracks[clamp(index < 0 ? 0 : index + step, 0, tracks.length - 1)];
    this.select(next.id, null);
    this.rows.querySelector(`.track-row[data-track="${next.id}"]`)?.scrollIntoView({ block: "nearest" });
  }

  selectClipBy(step) {
    const track = this.selectedTrackObject();
    if (!track?.clips.length) return;
    const clips = [...track.clips].sort((a, b) => a.start - b.start);
    const index = clips.findIndex((c) => c.id === this.selectedClip);
    const next = index < 0 ? clips[step > 0 ? 0 : clips.length - 1] : clips[clamp(index + step, 0, clips.length - 1)];
    this.select(track.id, next.id);
    const x = next.start * this.zoom, left = this.arrange.scrollLeft;
    if (x < left || x > left + this.arrange.clientWidth - this.headWidth - 40) this.scrollTo(x - 40);
  }

  //============================================================================ computer keyboard as MIDI
  // Ableton's layout: A S D F G H J K are the white keys from C, W E T Y U (and O P) the
  // black ones, Z / X change the octave, C / V the velocity. M turns it on and off.
  static PIANO = { KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11,
                   KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16 };
  static PIANO_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

  togglePiano(on) {
    this.piano ??= { on: false, octave: 0, velocity: 100, held: new Map() };
    this.piano.on = on ?? !this.piano.on;
    if (!this.piano.on) this.releasePianoKeys();
    this.pianoButton?.classList.toggle("is-accent", this.piano.on);
    this.pianoButton?.setAttribute("aria-pressed", String(this.piano.on));
    this.pianoWindow(this.piano.on);
    toast(this.piano.on ? "Computer keyboard piano on. Single-key shortcuts are off until you turn it off (M or Esc)." : "Computer keyboard piano off.");
  }

  // The floating piano: the keys, labelled with the computer keys that play them, lighting up
  // while held. Drag it by its title; the keys also play with the mouse.
  pianoWindow(show) {
    let win = this.pianoWin;
    if (!win) {
      win = this.pianoWin = html("div", "piano-window");
      win.hidden = true;
      const head = html("div", "piano-head");
      head.append(html("strong", "", "Keyboard piano"));
      const controls = html("span", "piano-controls");
      const step = (label, title, fn) => controls.append(button("mini", label, title, fn));
      step("−", "Octave down (Z)", () => this.pianoSet("octave", -1));
      this.pianoOctave = html("span", "piano-readout");
      controls.append(this.pianoOctave);
      step("+", "Octave up (X)", () => this.pianoSet("octave", 1));
      step("−", "Velocity down (C)", () => this.pianoSet("velocity", -20));
      this.pianoVelocity = html("span", "piano-readout");
      controls.append(this.pianoVelocity);
      step("+", "Velocity up (V)", () => this.pianoSet("velocity", 20));
      head.append(controls, button("mini", "×", "Turn the keyboard piano off (M)", () => this.togglePiano(false)));
      const keys = html("div", "piano-keys");
      const names = Object.fromEntries(Object.entries(DawScreen.PIANO).map(([code, n]) => [n, code]));
      const label = (code) => (code === "Semicolon" ? ";" : code.slice(3));
      this.pianoKeyElements = new Map();
      const whites = [0, 2, 4, 5, 7, 9, 11, 12, 14, 16];
      const blackAfter = { 1: 0, 3: 1, 6: 3, 8: 4, 10: 5, 13: 7, 15: 8 }; // black offset -> index of the white key to its left
      const add = (offset, cls, left) => {
        const code = names[offset];
        const key = html("div", `piano-key ${cls}`);
        key.style.left = `${left}%`;
        key.append(html("span", "", label(code)));
        key.dataset.offset = String(offset);
        key.addEventListener("pointerdown", (event) => {
          event.preventDefault();
          key.setPointerCapture(event.pointerId);
          this.pianoPress(code);
          key.addEventListener("pointerup", () => this.pianoRelease(code), { once: true });
          key.addEventListener("pointercancel", () => this.pianoRelease(code), { once: true });
        });
        this.pianoKeyElements.set(code, key);
        keys.append(key);
      };
      const w = 100 / whites.length;
      whites.forEach((offset, i) => add(offset, "white", i * w));
      for (const [offset, i] of Object.entries(blackAfter)) add(Number(offset), "black", (i + 1) * w - w * 0.3);
      for (const key of keys.querySelectorAll(".white")) key.style.width = `${w}%`;
      for (const key of keys.querySelectorAll(".black")) key.style.width = `${w * 0.6}%`;
      win.append(head, keys, html("div", "piano-hint", "A–K and W E T Y U O P play. Z / X octave, C / V velocity, M or Esc to close."));

      head.addEventListener("pointerdown", (event) => {
        if (event.target.closest("button")) return;
        const rect = win.getBoundingClientRect(), host = this.root.getBoundingClientRect();
        const dx = event.clientX - rect.left, dy = event.clientY - rect.top;
        head.setPointerCapture(event.pointerId);
        const move = (e) => {
          win.style.left = `${clamp(e.clientX - dx - host.left, 0, host.width - rect.width)}px`;
          win.style.top = `${clamp(e.clientY - dy - host.top, 0, host.height - rect.height)}px`;
          win.style.right = win.style.bottom = "auto";
        };
        head.addEventListener("pointermove", move);
        head.addEventListener("pointerup", () => head.removeEventListener("pointermove", move), { once: true });
      });
      this.root.append(win);
    }
    win.hidden = !show;
    this.updatePianoWindow();
  }

  updatePianoWindow() {
    if (!this.pianoWin || !this.piano) return;
    const { octave, velocity, held } = this.piano;
    this.pianoOctave.textContent = `C${3 + octave}`;
    this.pianoVelocity.textContent = `vel ${velocity}`;
    for (const [code, key] of this.pianoKeyElements) key.classList.toggle("is-down", held.has(code));
  }

  pianoSet(what, delta) {
    const piano = this.piano;
    if (what === "octave") piano.octave = clamp(piano.octave + delta, -4, 4);
    else piano.velocity = clamp(piano.velocity + delta, 1, 127);
    this.updatePianoWindow();
  }

  releasePianoKeys() {
    if (!this.piano) return;
    for (const note of this.piano.held.values()) daw("liveNote", { note, velocity: 0 });
    this.piano.held.clear();
    this.updatePianoWindow();
  }

  // The armed or monitored MIDI tracks play it. If none is, the selected MIDI track starts monitoring.
  playPianoNote(note, velocity) {
    const tracks = this.session?.tracks ?? [];
    const listening = tracks.some((t) => t.kind === "midi" && (t.arm || t.monitor));
    const selected = this.selectedTrackObject();
    if (!listening && selected?.kind === "midi") daw("setTrack", { id: selected.id, monitor: true });
    daw("liveNote", { note, velocity });
  }

  pianoPress(code) {
    const piano = this.piano;
    if (piano.held.has(code)) return;
    const pitch = clamp(60 + piano.octave * 12 + DawScreen.PIANO[code], 0, 127);
    piano.held.set(code, pitch);
    this.playPianoNote(pitch, piano.velocity);
    this.updatePianoWindow();
  }

  pianoRelease(code) {
    const held = this.piano.held;
    if (!held.has(code)) return false;
    daw("liveNote", { note: held.get(code), velocity: 0 });
    held.delete(code);
    this.updatePianoWindow();
    return true;
  }

  onPianoKey(event, down) {
    if (!this.active || !this.piano) return;
    const code = event.code;
    if (!down) {
      if (this.pianoRelease(code)) event.preventDefault();
      return;
    }
    if (!this.piano.on) {
      if (code === "KeyM" && !event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey && !isTyping(event)) {
        event.preventDefault(); event.stopPropagation(); if (!event.repeat) this.togglePiano(true);
      }
      return;
    }
    if (isTyping(event) || event.ctrlKey || event.metaKey) return; // Ctrl / Cmd shortcuts still work
    event.stopPropagation(); // every other single key is the piano's while it's on
    if (event.altKey || event.shiftKey) return;
    if (code in DawScreen.PIANO) {
      event.preventDefault();
      if (!event.repeat) this.pianoPress(code);
    } else if (["KeyZ", "KeyX", "KeyC", "KeyV", "KeyM", "Escape"].includes(code)) {
      event.preventDefault();
      if (event.repeat) return;
      if (code === "KeyM" || code === "Escape") return this.togglePiano(false);
      if (code === "KeyZ") this.pianoSet("octave", -1);
      else if (code === "KeyX") this.pianoSet("octave", 1);
      else this.pianoSet("velocity", code === "KeyC" ? -20 : 20);
    }
  }

  toggleTrack(key) {
    const track = this.selectedTrackObject();
    if (track) daw("setTrack", { id: track.id, [key]: !track[key] });
  }

  copySelected(cut) {
    const found = this.findClip(this.selectedClip);
    if (!found) return toast("Select a clip first.");
    const { clip } = found;
    if (clip.midi) {
      this.clipboard = { midi: true, offset: clip.offset, length: clip.length, name: clip.name,
                         notes: clip.notes.map(({ pitch, time, length, velocity }) => ({ pitch, time, length, velocity })) };
      if (cut) this.deleteSelected();
      return;
    }
    this.clipboard = { file: clip.file, offset: clip.offset, length: clip.length, gain: clip.gain, fadeIn: clip.fadeIn, fadeOut: clip.fadeOut, name: clip.name };
    if (cut) this.deleteSelected();
  }

  async paste() {
    if (!this.clipboard) return toast("Copy a clip first (Ctrl+C).");
    const track = this.selectedTrackObject() ?? this.session?.tracks[0];
    if (!track) return;
    if (this.clipboard.midi && track.kind !== "midi") return toast("Select a MIDI track to paste a MIDI clip on.");
    const id = await daw("pasteClip", { ...this.clipboard, track: track.id, start: Math.max(0, this.status.position) });
    if (id >= 0) this.select(track.id, id);
  }

  toggleTrackKey(track, key, on, event) {
    if (key === "solo" && event?.altKey) return daw("soloExclusive", { id: track.solo && session_onlySolo(this.session, track.id) ? -1 : track.id });
    return daw("setTrack", { id: track.id, [key]: on });
  }

  // Right-click menu on a track header.
  openTrackMenu(track, x, y) {
    const tracks = this.session.tracks, index = tracks.findIndex((t) => t.id === track.id);
    const firstClip = track.clips?.[0];
    popMenu(x, y, ({ item, rule, menu }) => {
      item("Rename", () => this.renameTrack(), { shortcut: "Ctrl+R" });
      item("Duplicate", () => this.duplicateTrack(track.id), { shortcut: "Ctrl+Shift+D" });
      item("Delete", () => this.deleteTrack(), { danger: true, shortcut: "Ctrl+Delete" });
      rule();
      item("Mute", () => daw("setTrack", { id: track.id, mute: !track.mute }), { checked: track.mute, shortcut: "0" });
      item("Solo", () => daw("setTrack", { id: track.id, solo: !track.solo }), { checked: track.solo, shortcut: "S" });
      item("Arm for recording", () => daw("setTrack", { id: track.id, arm: !track.arm }), { checked: track.arm, shortcut: "A" });
      rule();
      item("Move up", () => daw("moveTrack", { id: track.id, index: index - 1 }), { disabled: index <= 0 });
      item("Move down", () => daw("moveTrack", { id: track.id, index: index + 1 }), { disabled: index >= tracks.length - 1 });
      rule();
      item("Add effect…", () => { this.select(track.id, null); this.setDockTab?.("effects"); toast("Pick an effect from Add effect, or drag one from the library."); });
      item("Clear volume ride", () => daw("setTrack", { id: track.id, clearRide: true }), { disabled: !track.ride?.length });
      item("Clear effect automation", () => daw("setTrack", { id: track.id, clearAutomation: true }), { disabled: !track.automation?.length });
      item("Copy effects", () => this.copyEffects(track.id), { disabled: !track.inserts.length });
      item("Paste effects", () => this.pasteEffects(track.id), { disabled: !this.effectsClipboard || this.effectsClipboard.id === track.id });
      if (track.kind !== "midi")
        item("Split into stems…", () => this.toggleStems(true, firstClip), { disabled: !firstClip });
      rule();
      const colours = html("div", "colour-row track-menu-colours");
      TRACK_COLOURS.forEach((colour) => {
        const swatch = button("colour-swatch", "", "Track colour", () => { closeMenu(); daw("setTrack", { id: track.id, colour }); });
        swatch.style.background = colour;
        colours.append(swatch);
      });
      menu.append(colours);
    });
  }

  closeTrackMenu() { closeMenu(); }

  // A clip's right-click menu: editing, then the Audio processes (the same as the menu
  // bar's Audio menu, which the plug-in doesn't have).
  openClipMenu(clip, x, y) {
    const audio = !clip.midi;
    popMenu(x, y, ({ item, rule, heading }) => {
      item("Split at the playhead", () => this.splitSelected(), { shortcut: "Ctrl+E" });
      item("Duplicate", () => this.duplicateSelected(), { shortcut: "Ctrl+D" });
      item("Copy", () => this.copySelected(false), { shortcut: "Ctrl+C" });
      item("Cut", () => this.copySelected(true), { shortcut: "Ctrl+X" });
      item("Delete", () => this.deleteSelected(), { danger: true, shortcut: "Delete" });
      rule();
      item("Mute", () => this.run("muteClip"), { checked: Boolean(clip.mute), shortcut: "Shift+M" });
      item("Loop the clip", () => this.run("loopClip"), { shortcut: "Ctrl+L" });
      item("Zoom to the clip", () => this.run("zoomClip"), { shortcut: "Z" });
      if (audio) {
        heading("Audio");
        for (const [label, command, hint] of AUDIO_PROCESSES)
          item(label, () => this.run(command), { hint, indent: true });
      }
      if (clip.midi) item("Edit notes", () => { this.setDockTab("track"); this.setDock(true); });
    });
  }

  async duplicateTrack(id = this.selectedTrack) {
    if (!id) return toast("Select a track first.");
    const copy = await daw("duplicateTrack", { id });
    if (typeof copy === "number" && copy > 0) this.select(copy, null);
  }

  copyEffects(owner = this.selectedTrack) {
    if (owner == null) return toast("Select a track first.");
    const from = owner === 0 ? this.session.master : [...this.session.tracks, ...(this.session.returns ?? [])].find((t) => t.id === owner);
    if (!from?.inserts.length) return toast("This track has no effects to copy.");
    this.effectsClipboard = { id: owner, name: owner === 0 ? "Master" : from.name, count: from.inserts.length };
    toast(`Copied ${from.inserts.length} effect${from.inserts.length === 1 ? "" : "s"} from "${this.effectsClipboard.name}".`);
    this.inspectorKey = "";
    this.renderInspector();
  }

  pasteEffects(owner = this.selectedTrack) {
    if (!this.effectsClipboard) return toast("Copy a track's effects first.");
    if (owner == null || owner === this.effectsClipboard.id) return;
    daw("pasteInserts", { from: this.effectsClipboard.id, to: owner });
  }

  withClip(action) {
    const found = this.findClip(this.selectedClip);
    if (!found) return toast("Select a clip first.");
    return action(found.clip);
  }

  // Zooms so start..end fills the view, with a little room on both sides.
  zoomToRange(start, end) {
    const visible = Math.max(100, this.arrange.clientWidth - this.headWidth);
    const span = Math.max(0.05, end - start);
    this.zoom = clamp((visible * 0.92) / span, MIN_ZOOM, MAX_ZOOM);
    this.renderArrange(true);
    this.scrollTo((start - span * 0.04) * this.zoom);
    this.renderTransport();
  }

  nudgeSelected(seconds) {
    const found = this.findClip(this.selectedClip);
    if (found) daw("moveClip", { id: found.clip.id, start: Math.max(0, found.clip.start + seconds) });
  }

  // What the buttons, the keyboard shortcuts and the app's menu bar (File, Edit, View,
  // Transport) can do, by name, so all three always do the same thing.
  get commands() {
    return {
      new: () => daw("new"),
      open: () => daw("open"),
      save: () => daw("save"),
      saveAs: () => daw("saveAs"),
      import: () => this.importAudio(),
      export: () => this.toggleExportPanel(true),
      autoMix: () => this.toggleAutoMix(true),
      songKey: () => this.toggleKeyPanel(true),
      undo: () => daw("undo"),
      redo: () => daw("redo"),
      cut: () => this.copySelected(true),
      copy: () => this.copySelected(false),
      paste: () => this.paste(),
      duplicate: () => this.duplicateSelected(),
      split: () => this.splitSelected(),
      delete: () => this.deleteSelected(),
      addTrack: () => this.addTrack(),
      toggleView: () => this.setView(this.view === "arrange" ? "mixer" : "arrange"),
      toggleDock: () => this.setDock(this.root.classList.contains("dock-hidden")),
      toggleLibrary: () => this.library.toggle(),
      searchLibrary: () => this.library.focusSearch(),
      follow: () => this.setFollow(!this.follow),
      zoomIn: () => this.setZoom(this.zoom * 1.5),
      zoomOut: () => this.setZoom(this.zoom / 1.5),
      taller: () => this.setRowHeight(this.rowHeight + 12),
      shorter: () => this.setRowHeight(this.rowHeight - 12),
      shortcuts: () => this.toggleShortcuts(),
      play: () => this.togglePlay(),
      stop: () => daw("transport", { action: "stop" }),
      record: () => this.toggleRecord(),
      start: () => this.seek(0),
      end: () => this.seek(this.songEnd()),
      loop: () => this.toggleLoop(),
      metronome: () => daw("metronome", { on: !this.session?.metronome }),
      addMarker: () => this.addMarker(),
      duplicateTrack: () => this.duplicateTrack(),
      copyEffects: () => this.copyEffects(),
      pasteEffects: () => this.pasteEffects(),
      clearMuteSolo: () => daw("clearMuteSolo"),
      muteClip: () => this.withClip((clip) => daw("setClip", { id: clip.id, mute: !clip.mute })),
      normaliseClip: () => this.withClip(async (clip) => {
        const result = await daw("normaliseClip", { id: clip.id });
        if (result?.ok) toast(`Clip gain set to ${dbText(result.gain)} dB (peak at −1 dB).`);
      }),
      stripSilenceQuiet: () => { this.stripThreshold = -55; this.run("stripSilence"); },
      stripSilenceNormal: () => { this.stripThreshold = -45; this.run("stripSilence"); },
      stripSilenceNoisy: () => { this.stripThreshold = -35; this.run("stripSilence"); },
      stripSilence: () => this.withClip(async (clip) => {
        const result = await daw("stripSilence", { id: clip.id, threshold: this.stripThreshold ?? -45 });
        if (result?.ok) {
          this.selectedClip = null;
          toast(`Kept ${result.clips} phrase${result.clips === 1 ? "" : "s"}.`, "", "undo");
        }
      }),
      splitStems: () => this.withClip((clip) => this.toggleStems(true, clip)),
      loopClip: () => this.withClip((clip) => daw("loop", { on: true, start: clip.start, end: clip.start + clip.length })),
      zoomClip: () => this.withClip((clip) => this.zoomToRange(clip.start, clip.start + clip.length)),
      zoomFit: () => this.zoomToRange(0, Math.max(this.songEnd(), 10)),
      previousMarker: () => this.jumpMarker(-1),
      nextMarker: () => this.jumpMarker(1),
      tapTempo: () => this.tapTempo(),
      goTo: () => this.editClock(),
      soloTrack: () => this.toggleTrack("solo"),
      muteTrack: () => this.toggleTrack("mute"),
      renameTrack: () => this.renameTrack(),
      deleteTrack: () => this.deleteTrack(),
      quantize: () => this.quantizeSelected(),
      zoomSelection: () => this.run(this.findClip(this.selectedClip) ? "zoomClip" : "zoomFit"),
    };
  }

  run(command) {
    if (this.active) this.commands[command]?.();
  }

  renameTrack() {
    const input = this.rows.querySelector(`.track-row[data-track="${this.selectedTrack}"] .track-name`);
    if (!input) return toast("Select a track first.");
    if (this.view !== "arrange") this.setView("arrange");
    input.scrollIntoView({ block: "nearest" });
    input.focus();
    input.select();
  }

  async deleteTrack() {
    const track = this.selectedTrackObject();
    if (!track) return toast("Select a track first.");
    const tracks = this.session.tracks, index = tracks.indexOf(track);
    const next = tracks[index + 1] ?? tracks[index - 1];
    await daw("removeTrack", { id: track.id });
    if (next) this.select(next.id, null);
    toast(`Deleted "${track.name}".`, "", "undo");
  }

  // Q: quantize the selected clip, or the whole selected track, with the settings last
  // used in the Pitch & time panel.
  async quantizeSelected() {
    const track = this.selectedTrackObject();
    if (!track) return toast("Select a track or clip first.");
    const clip = this.findClip(this.selectedClip)?.clip;
    const args = savedQuantizeArgs(this.session, track.id);
    const result = await daw("quantize", clip ? { clips: [clip.id], ...args } : { track: track.id, ...args });
    if (!result?.ok) return;
    const what = clip ? "Quantize" : `Quantize ${track.name}`;
    toast(result.onsets ? `${what}: ${result.moved} of ${result.onsets} notes moved onto the grid.` : `${what}: no clear note starts found.`);
  }

  onKey(event) {
    if (!this.active || isTyping(event)) return;
    if (event.ctrlKey && event.metaKey) return; // Ctrl+Cmd+F is full screen on a Mac
    const mod = event.ctrlKey || event.metaKey;
    const key = event.key.toLowerCase();
    const code = event.code; // for Alt (Option on a Mac) combinations, which change event.key
    const handled = () => event.preventDefault();
    const onSlider = event.target.closest?.("[role=slider]");
    const step = this.beatsFrom(this.status.position, event.shiftKey ? this.beatsPerBar : 1);
    const command = (name) => { handled(); this.run(name); };

    if (mod && event.altKey) { if (code === "KeyB") command("toggleLibrary"); }
    else if (mod && event.shiftKey) {
      if (key === "z") command("redo");
      else if (key === "s") command("saveAs");
      else if (key === "d") command("duplicateTrack");
      else if (key === "t") { handled(); this.addMidiTrack(); }
      else if (key === "r") command("export");
      else if (key === "i") command("import");
    }
    else if (mod) {
      if (key === "z") command("undo");
      else if (key === "y") command("redo");
      else if (key === "s") command("save");
      else if (key === "n") command("new");
      else if (key === "o") command("open");
      else if (key === "i") command("import");
      else if (key === "b") command("export");
      else if (key === "d") command("duplicate");
      else if (key === "e") command("split");
      else if (key === "t") command("addTrack");
      else if (key === "r") command("renameTrack");
      else if (key === "l") command("loopClip");
      else if (key === "u") command("quantize");
      else if (key === "x") command("cut");
      else if (key === "c") command("copy");
      else if (key === "v") command("paste");
      else if (key === "f") command("searchLibrary");
      else if (event.key === "Delete" || event.key === "Backspace") command("deleteTrack");
    }
    else if (event.altKey && (event.key === "ArrowLeft" || event.key === "ArrowRight")) { handled(); this.nudgeSelected(event.key === "ArrowLeft" ? -step : step); }
    else if (event.altKey) return;
    else if (code === "Numpad0") command("stop");
    else if (code === "NumpadMultiply") command("record");
    else if (code === "NumpadDivide") command("loop");
    else if (onSlider && event.key.startsWith("Arrow")) return;
    else if (event.key === " ") { if (event.target.closest?.("button, [role=slider]")) return; command("play"); }
    else if (event.key === "Enter") { if (event.target.closest?.("button")) return; command("stop"); }
    else if (event.key === "ArrowLeft") { handled(); this.seek(Math.ceil(this.status.position / step - 1e-6) * step - step); }
    else if (event.key === "ArrowRight") { handled(); this.seek(Math.floor(this.status.position / step + 1e-6) * step + step); }
    else if (event.key === "ArrowUp") { handled(); event.shiftKey ? this.setRowHeight(this.rowHeight + 12) : this.selectTrackBy(-1); }
    else if (event.key === "ArrowDown") { handled(); event.shiftKey ? this.setRowHeight(this.rowHeight - 12) : this.selectTrackBy(1); }
    else if (event.key === "Home") command("start");
    else if (event.key === "End") command("end");
    else if (event.key === "Insert") command("addMarker");
    else if (event.key === "?" || event.key === "/") command("shortcuts");
    else if (event.key === "Tab") command(event.shiftKey ? "toggleDock" : "toggleView");
    else if (event.key === "Delete" || event.key === "Backspace") { if (onSlider) return; command("delete"); }
    else if (event.key === "+" || event.key === "=") command("zoomIn");
    else if (event.key === "-" || event.key === "_") command("zoomOut");
    else if (event.key === ",") { handled(); this.selectClipBy(-1); }
    else if (event.key === ".") { handled(); this.selectClipBy(1); }
    else if (event.key === "Escape") {
      this.toggleExportPanel(false);
      this.toggleAutoMix(false);
      this.toggleShortcuts(false);
      this.setTool("select");
      this.select(this.selectedTrack, null);
    }
    else if (event.shiftKey) {
      if (key === "m") command("muteClip");
      else if (key === "z") command("zoomFit");
      else if (key === "b") command("toggleLibrary");
    }
    else if (key === "r") command("record");
    else if (key === "k") command("addMarker");
    else if (key === "z") command("zoomSelection");
    else if (event.key === "[") command("previousMarker");
    else if (event.key === "]") command("nextMarker");
    else if (key === "t") command("tapTempo");
    else if (key === "s") command("soloTrack");
    else if (key === "0") command("muteTrack");
    else if (key === "q") command("quantize");
    else if (key === "l") command("loop");
    else if (key === "c") command("metronome");
    else if (key === "f") command("follow");
    else if (key === "x") command("toggleView");
    else if (key === "a") { handled(); this.toggleTrack("arm"); }
    else if (key === "i") { handled(); this.toggleTrack("monitor"); }
    else if (key === "v") { handled(); this.setTool("select"); }
    else if (key === "b") { handled(); this.setTool("cut"); }
    else if (key === "g") { handled(); this.setTool("gain"); }
    else if (key === "w") { handled(); this.setTool("warp"); }
    else if (key === "e") command("toggleDock");
  }
}

//==============================================================================
// Wires the Live/DAW switch in the header. Returns nothing; DAW mode is a
// standalone-app feature, so in a plugin host the switch stays hidden.
export function initDaw() {
  const root = document.querySelector(".daw");
  const modeSwitch = document.querySelector(".mode-switch");
  const title = document.querySelector(".app-title");
  const projectLine = document.querySelector(".app-project");
  const liveTracks = document.querySelector(".live-tracks");
  const liveTracksButton = liveTracks.querySelector(".live-tracks-button");
  const liveTracksMenu = liveTracks.querySelector(".live-tracks-menu");
  const backdrop = document.querySelector(".confirm-backdrop");
  let sessionState = null;

  // Asks before leaving one mode for the other; resolves true on "Switch".
  const confirmSwitch = (toDaw) => new Promise((resolve) => {
    const recording = screen.status?.recording;
    backdrop.querySelector("h2").textContent = toDaw ? "Switch to MicFX DAW?" : "Switch to MicFX Live?";
    backdrop.querySelector(".confirm-text").textContent = (toDaw
      ? "The multitrack recorder takes over the window. Your live effects keep their settings."
      : "Back to the live effects chain. The DAW project stays open and its tracks stay listed in Live.")
      + (recording ? " A recording is in progress; switching stops it." : "");
    const ok = backdrop.querySelector(".confirm-ok"), stay = backdrop.querySelector(".confirm-stay");
    ok.textContent = toDaw ? "Switch to DAW" : "Switch to Live";
    const done = (value) => {
      backdrop.hidden = true;
      ok.removeEventListener("click", yes); stay.removeEventListener("click", no);
      backdrop.removeEventListener("click", away); document.removeEventListener("keydown", key, true);
      resolve(value);
    };
    const yes = () => done(true), no = () => done(false);
    const away = (event) => { if (event.target === backdrop) done(false); };
    const key = (event) => {
      if (event.key === "Escape") { event.stopPropagation(); done(false); }
      else if (event.key === "Enter") { event.stopPropagation(); event.preventDefault(); done(true); }
    };
    ok.addEventListener("click", yes); stay.addEventListener("click", no);
    backdrop.addEventListener("click", away); document.addEventListener("keydown", key, true);
    backdrop.hidden = false;
    ok.focus();
  });
  const requestMode = async (toDaw) => {
    if (toDaw === screen.active) return;
    if (await confirmSwitch(toDaw)) daw("mode", { daw: toDaw });
  };

  const renderLiveTracks = () => {
    const tracks = sessionState?.tracks ?? [];
    liveTracks.hidden = !sessionState?.available || screen.active || tracks.length === 0;
    liveTracks.querySelector(".live-tracks-count").textContent = String(tracks.length);
    if (liveTracks.hidden) { liveTracksMenu.hidden = true; liveTracksButton.setAttribute("aria-expanded", "false"); }
    const anySolo = tracks.some((t) => t.solo);
    liveTracksMenu.replaceChildren(
      html("div", "live-tracks-head", sessionState?.project?.name ?? ""),
      ...tracks.map((t) => {
        const row = html("div", "live-track" + (t.mute || (anySolo && !t.solo) ? " is-off" : ""));
        const dot = html("span", "live-track-dot"); dot.style.background = t.colour || "var(--accent)";
        const flags = [t.kind === "midi" ? "MIDI" : "", t.arm ? "armed" : "", t.solo ? "solo" : "", t.mute ? "muted" : "",
                       `${t.clips.length} clip${t.clips.length === 1 ? "" : "s"}`].filter(Boolean).join(" · ");
        row.append(dot, html("span", "live-track-name", t.name), html("span", "live-track-flags", flags));
        return row;
      }),
      html("div", "live-tracks-foot", "Read only. Switch to DAW to edit."));
  };
  liveTracksButton.addEventListener("click", () => {
    liveTracksMenu.hidden = !liveTracksMenu.hidden;
    liveTracksButton.setAttribute("aria-expanded", String(!liveTracksMenu.hidden));
  });
  document.addEventListener("click", (event) => {
    if (!liveTracks.contains(event.target)) { liveTracksMenu.hidden = true; liveTracksButton.setAttribute("aria-expanded", "false"); }
  });
  const screen = new DawScreen(root);

  Juce.getNativeFunction("getParameterInfo")().then((info) => {
    parameterInfo = info ?? {};
    screen.inspectorKey = ""; // rebuild the inserts with real ranges
    if (screen.active) screen.render();
  });
  Juce.getNativeFunction("getPresets")().then((presets) => {
    screen.presets = presets ?? [];
    screen.inspectorKey = "";
    if (screen.active) screen.render();
  });

  const applyMode = (session) => {
    modeSwitch.hidden = !session.available;
    const isDaw = session.available && session.mode === "daw";
    const changed = document.body.classList.contains("is-daw") !== isDaw;
    document.body.classList.toggle("is-daw", isDaw);
    title.textContent = isDaw ? "MicFX DAW" : "MicFX Live";
    document.title = title.textContent;
    // The active project's name sits in the top bar in both modes.
    const project = session.project;
    projectLine.hidden = !session.available;
    projectLine.querySelector(".app-project-label").textContent = session.available ? project.name : "Live voice processor";
    projectLine.querySelector(".dirty").textContent = session.available && project.dirty ? "•" : "";
    projectLine.title = project?.open ? project.path : "Not saved yet. Save to keep your work in a project folder.";
    sessionState = session;
    renderLiveTracks();
    root.hidden = !isDaw;
    [...modeSwitch.children].forEach((b) => b.classList.toggle("is-selected", (b.dataset.mode === "daw") === isDaw));
    if (changed && isDaw) screen.render();
  };

  for (const b of modeSwitch.children)
    b.addEventListener("click", () => requestMode(b.dataset.mode === "daw"));

  document.addEventListener("daw-session", (event) => applyMode(event.detail));

  // The app's menu bar (standalone only) sends the same commands as the buttons and keys.
  window.__JUCE__.backend.addEventListener("appMenu", (action) => {
    if (action === "mode:live" || action === "mode:daw") requestMode(action === "mode:daw");
    else screen.run(action);
  });

  // Full screen: a header button, F11, or Ctrl+Cmd+F (the macOS shortcut). Standalone only.
  const fullScreen = document.querySelector(".fullscreen-button");
  const nativeWindow = Juce.getNativeFunction("window");
  const applyWindow = (state) => {
    fullScreen.hidden = !state?.available;
    const on = Boolean(state?.fullScreen);
    fullScreen.setAttribute("aria-pressed", String(on));
    fullScreen.title = on ? "Leave full screen (F11)" : "Full screen (F11)";
  };
  const toggleFullScreen = () => nativeWindow("fullScreen").then(applyWindow);
  fullScreen.addEventListener("click", toggleFullScreen);
  window.__JUCE__.backend.addEventListener("window", applyWindow);
  document.addEventListener("keydown", (event) => {
    if (event.key === "F11" || (event.ctrlKey && event.metaKey && event.key.toLowerCase() === "f")) {
      event.preventDefault();
      if (!fullScreen.hidden) toggleFullScreen();
    }
  });
  nativeWindow("state").then(applyWindow);
  nativeDaw("session", {}).then((session) => session && screen.onSession(session));
}
