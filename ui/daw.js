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
import { createRhythmPanel, drawWarpMarkers, startWarpDrag } from "./rhythm.js";
import { ARC_END, ARC_START, arcPath, clamp, formatDuration, formatValue, html, polar, svg } from "./widgets.js";

const nativeDaw = Juce.getNativeFunction("daw");

const HEAD_WIDTH = 210;           // track header column, px (matches daw.css)
const MIN_ROW = 44, MAX_ROW = 200; // track height range, px (vertical zoom)
const MIN_ZOOM = 4, MAX_ZOOM = 2000; // pixels per second
const METER_FLOOR_DB = -60;
const FADER_MIN_DB = -60, FADER_MAX_DB = 6;
const GRIDS = [["bar", "Bar"], [1, "Beat"], [0.5, "1/2 beat"], [0.25, "1/4 beat"]];
const SCALE_STEPS = { major: [0, 2, 4, 5, 7, 9, 11], minor: [0, 2, 3, 5, 7, 8, 10] };
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

const isTyping = (event) => Boolean(event.target.closest?.("input:not([type=range]):not([type=checkbox]), select, textarea, [contenteditable]"));
const trackColour = (track, index) => track.colour || TRACK_COLOURS[(track.id ?? index) % TRACK_COLOURS.length];
const dbText = (value) => {
  const db = Number(value) || 0;
  return db <= FADER_MIN_DB ? "−∞" : `${db > 0.05 ? "+" : ""}${db.toFixed(1)}`;
};

function button(className, text, title, onClick) {
  const element = html("button", className, text);
  if (title) element.title = title;
  if (onClick) element.addEventListener("click", onClick);
  return element;
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
class ValueKnob {
  constructor({ label, min, max, skew = 1, step = 0, unit = "", value, defaultValue, bipolar = false, format, onChange }) {
    Object.assign(this, { min, max, skew, step, unit, bipolar, onChange, format });
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
    this.element.title = "Drag or scroll · Shift for fine control · Double-click to reset";

    this.element.addEventListener("pointerdown", (event) => {
      this.element.focus();
      let start = this.normalised, first = true;
      this.dragging = true;
      drag(event, (dx, dy, e) => {
        this.set(this.fromNormalised(clamp(start - dy / (e.shiftKey ? 900 : 220))), first);
        first = false;
      }, () => { this.dragging = false; });
    });
    this.element.addEventListener("wheel", (event) => {
      event.preventDefault();
      this.set(this.fromNormalised(clamp(this.normalised - event.deltaY * (event.shiftKey ? 0.0004 : 0.002))), true);
    }, { passive: false });
    this.element.addEventListener("keydown", (event) => {
      const step = event.shiftKey ? 0.002 : 0.02;
      const delta = { ArrowUp: step, ArrowRight: step, ArrowDown: -step, ArrowLeft: -step }[event.key];
      if (delta === undefined) return;
      event.preventDefault();
      event.stopPropagation();
      this.set(this.fromNormalised(clamp(this.normalised + delta)), true);
    });
    this.element.addEventListener("dblclick", () => this.set(this.defaultValue, true));
    this.render();
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
    this.element.title = "Drag to set the volume · Double-click for 0 dB";
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
      event.preventDefault();
      this.set(Fader.toDecibels(clamp(Fader.toPosition(this.value) - event.deltaY * 0.001)), true);
    }, { passive: false });
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

//==============================================================================
// A small horizontal slider for the track headers (volume, pan). Click or drag to
// set, double-click to reset. onChange (value, first) like the knobs.
class HSlider {
  constructor({ value, defaultValue, toPosition, fromPosition, format, title, bipolar = false, onChange }) {
    Object.assign(this, { value, defaultValue, toPosition, fromPosition, format, bipolar, onChange });
    this.dragging = false;
    this.element = html("div", bipolar ? "hslider is-bipolar" : "hslider");
    this.element.title = `${title} · Drag to change · Shift for fine · Double-click to reset`;
    this.element.setAttribute("role", "slider");
    this.element.setAttribute("aria-label", title);
    this.element.tabIndex = 0;
    this.fill = html("div", "hslider-fill");
    this.text = html("span", "hslider-text");
    this.element.append(this.fill, this.text);

    this.element.addEventListener("pointerdown", (event) => {
      event.stopPropagation();
      const box = this.element.getBoundingClientRect();
      const start = this.toPosition(this.value);
      let first = true;
      this.dragging = true;
      drag(event, (dx, dy, e) => {
        this.set(this.fromPosition(clamp(start + dx / (box.width * (e.shiftKey ? 5 : 1)))), first);
        first = false;
      }, (moved, e) => {
        this.dragging = false;
        if (!moved) this.set(this.fromPosition(clamp((e.clientX - box.left) / box.width)), true);
      });
    });
    this.element.addEventListener("dblclick", () => this.set(this.defaultValue, true));
    this.element.addEventListener("keydown", (event) => {
      const step = event.shiftKey ? 0.005 : 0.03;
      const delta = { ArrowRight: step, ArrowUp: step, ArrowLeft: -step, ArrowDown: -step }[event.key];
      if (delta === undefined) return;
      event.preventDefault();
      event.stopPropagation();
      this.set(this.fromPosition(clamp(this.toPosition(this.value) + delta)), true);
    });
    this.render();
  }

  set(value, first) {
    if (value === this.value) return;
    this.value = value;
    this.render();
    this.onChange(value, first);
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

    this.element = html("section", insert.type === "pitch" || insert.type === "paraEq" ? "insert is-wide" : "insert");
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
    this.rowHeight = 84;       // track height, px (vertical zoom)
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

    window.__JUCE__.backend.addEventListener("dawSession", (session) => this.onSession(session));
    window.__JUCE__.backend.addEventListener("dawStatus", (status) => this.onStatus(status));
    window.__JUCE__.backend.addEventListener("dawExport", (progress) => this.onExport(progress));
    window.__JUCE__.backend.addEventListener("dawStems", (event) => this.onStems(event));
    document.addEventListener("keydown", (event) => this.onKey(event));
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
  get gridSeconds() { return this.grid === "bar" ? this.bar : this.beat * this.grid; }
  get markers() { return this.session?.markers ?? []; }

  // Seconds snapped to the grid (unless snap is off or Alt is held).
  snapTime(seconds, event) {
    if (!this.snap || event?.altKey) return Math.max(0, seconds);
    const step = this.gridSeconds;
    return Math.max(0, Math.round(seconds / step) * step);
  }

  barsBeats(seconds) {
    const beats = Math.max(0, seconds) / this.beat;
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
      return (bar - 1) * this.bar + (beat - 1) * this.beat;
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
    daw("tempo", { bpm: clamp(bpm, 20, 400), continue: this.taps.length > 3 });
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
          line.style.left = `${HEAD_WIDTH + time * this.zoom}px`;
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
      line.style.left = `${HEAD_WIDTH + marker.time * this.zoom}px`;
      this.ruler.append(flag);
      this.inner.append(line);
    }
  }

  //============================================================================ transport
  buildTransport() {
    const bar = this.root.querySelector(".daw-transport");

    const files = html("div", "daw-group");
    files.append(
      button("tool", "New", "Start a new project", () => daw("new")),
      button("tool", "Open…", "Open a project", () => daw("open")),
      (this.recentSelect = html("select", "dropdown recent-projects")),
      (this.saveButton = button("tool", "Save", "Save the project (Ctrl+S)", () => daw("save"))),
      button("tool", "Save as…", "Save a copy under a new name", () => daw("saveAs")),
      button("tool", "Import…", "Add an audio file at the playhead", () => this.importAudio()),
      button("tool", "Auto Mix…", "Listen to the voice and backing and set EQ, compression and levels", () => this.toggleAutoMix()),
      (this.exportButton = button("tool is-accent", "Export…", "Render the mix or stems to WAV", () => this.toggleExportPanel())));

    this.recentSelect.title = "Open a recent project";
    this.recentSelect.addEventListener("change", () => {
      const path = this.recentSelect.value;
      this.recentSelect.value = "";
      if (path) daw("openRecent", { path });
    });

    this.projectName = html("div", "daw-project");

    const transport = html("div", "daw-group daw-transport-buttons");
    transport.append(
      button("transport-button", "", "Back to the start (Enter)", () => daw("transport", { action: "stop" })),
      (this.playButton = button("transport-button play-button", "", "Play / pause (Space)", () => this.togglePlay())),
      (this.recordButton = button("record-button", "", "Record on the armed tracks (R)", () => this.toggleRecord())));
    this.inputMeter = new Level(false, "input");
    this.inputReadout = html("span", "mic-readout", "−∞");
    const mic = html("div", "mic-meter");
    mic.title = "Mic input level. Aim for peaks around −12 dB; red means it went over 0 dB (click to reset).";
    mic.append(html("span", "mic-label", "Mic"), this.inputMeter.element, this.inputReadout);
    transport.append(mic);
    transport.children[0].innerHTML = `<svg viewBox="0 0 16 16" aria-hidden="true"><rect x="3" y="3" width="2" height="10" rx="0.5"/><path d="M13 3.5v9a.5.5 0 0 1-.8.4L6.3 8.4a.5.5 0 0 1 0-.8l5.9-4.5a.5.5 0 0 1 .8.4z"/></svg>`;
    this.playButton.innerHTML = `<svg class="icon-play" viewBox="0 0 16 16" aria-hidden="true"><path d="M4 2.8v10.4a.6.6 0 0 0 .9.5l8.2-5.2a.6.6 0 0 0 0-1L4.9 2.3a.6.6 0 0 0-.9.5z"/></svg><svg class="icon-pause" viewBox="0 0 16 16" aria-hidden="true"><rect x="3.5" y="2.5" width="3" height="11" rx="0.8"/><rect x="9.5" y="2.5" width="3" height="11" rx="0.8"/></svg>`;
    this.recordButton.innerHTML = `<span class="record-dot"></span><span class="record-label">Rec</span>`;
    this.recordButton.setAttribute("aria-label", "Record");
    this.playButton.setAttribute("aria-label", "Play");

    const clock = (this.clock = html("div", "daw-clock"));
    clock.title = "Click to type a bar or time to go to";
    clock.addEventListener("click", (event) => { if (event.target === clock || !event.target.closest("input")) this.editClock(); });
    clock.append((this.clockBars = html("span", "clock-bars", "1.1")), (this.clockTime = html("span", "clock-time", "0:00.0")));

    const timing = html("div", "daw-group");
    this.tempoInput = html("input", "tempo-input");
    Object.assign(this.tempoInput, { type: "number", min: 20, max: 400, step: 1, title: "Tempo (BPM)" });
    this.tempoInput.addEventListener("change", () => daw("tempo", { bpm: Number(this.tempoInput.value) || 120 }));
    const tempo = html("label", "tempo");
    tempo.append(this.tempoInput, html("span", "", "BPM"));
    const tap = button("tool tap-tempo", "Tap", "Tap tempo: click (or press T) on the beat a few times", () => this.tapTempo());

    this.signature = html("select", "dropdown signature");
    this.signature.title = "Time signature (beats in a bar). In x/8 the tempo counts eighth notes.";
    SIGNATURES.forEach(([beats, text]) => this.signature.append(new Option(text, beats)));
    this.signature.addEventListener("change", () => daw("beatsPerBar", { beats: Number(this.signature.value) }));

    this.clickVolume = html("input", "click-volume");
    Object.assign(this.clickVolume, { type: "range", min: 0, max: 200, step: 1, title: "Click volume" });
    let clickFirst = true;
    this.clickVolume.addEventListener("input", () => {
      daw("metronomeVolume", { volume: Number(this.clickVolume.value) / 100, continue: !clickFirst });
      clickFirst = false;
    });
    this.clickVolume.addEventListener("change", () => { clickFirst = true; });

    this.gridSelect = html("select", "dropdown grid-size");
    this.gridSelect.title = "Snap grid";
    GRIDS.forEach(([value, text]) => this.gridSelect.append(new Option(text, value)));
    this.gridSelect.addEventListener("change", () => {
      this.grid = this.gridSelect.value === "bar" ? "bar" : Number(this.gridSelect.value);
      prefs.set("grid", this.grid);
    });

    this.countIn = html("select", "dropdown count-in");
    this.countIn.title = "Count-in before recording";
    ["No count-in", "1 bar count-in", "2 bar count-in", "4 bar count-in"].forEach((text, i) => this.countIn.append(new Option(text, [0, 1, 2, 4][i])));
    this.countIn.addEventListener("change", () => daw("countIn", { bars: Number(this.countIn.value) }));

    const click = html("div", "click-group");
    click.append((this.metronomeButton = button("toggle", "Click", "Metronome (C)", () => daw("metronome", { on: !this.session?.metronome }))),
                 this.clickVolume);
    this.keyButton = button("tool key-button", "Key", "The song's key: detect it, pick it, and set the autotune to it", () => this.toggleKeyPanel());
    timing.append(tempo, tap, this.signature, this.keyButton, click,
      this.countIn,
      (this.loopButton = button("toggle", "Loop", "Loop the marked range (L). Drag on the ruler to mark it.", () => this.toggleLoop())),
      (this.snapButton = button("toggle", "Snap", "Snap edits to the beat (hold Alt to drag freely)", () => {
        this.snap = !this.snap;
        this.renderTransport();
      })),
      this.gridSelect,
      (this.returnButton = button("toggle", "Return", "Return on stop: pausing goes back to where you pressed play", () => {
        this.returnOnStop = !this.returnOnStop;
        prefs.set("returnOnStop", this.returnOnStop);
        this.renderTransport();
      })),
      (this.followButton = button("toggle", "Follow", "Scroll with the playhead while playing (F). Scrolling yourself switches it off.", () => this.setFollow(!this.follow))));

    this.tools = html("div", "segmented tools");
    for (const [tool, text, title] of [["select", "Select", "Select, move, trim and fade clips (V)"],
                                       ["cut", "Cut", "Click a clip to split it there (B)"],
                                       ["gain", "Gain", "Drag a clip up or down to change its gain (G)"],
                                       ["warp", "Warp", "Drag inside a clip to stretch its timing; double-click a marker to remove it (W)"]]) {
      const b = button("", text, title, () => this.setTool(tool));
      b.dataset.tool = tool;
      this.tools.append(b);
    }

    const zoom = html("div", "daw-group zoom-sliders");
    const slider = (label, title, onInput) => {
      const wrapper = html("label", "zoom-slider");
      wrapper.title = title;
      const input = html("input");
      Object.assign(input, { type: "range", min: 0, max: 1000, step: 1 });
      input.addEventListener("input", () => onInput(Number(input.value) / 1000));
      wrapper.append(html("span", "", label), input);
      zoom.append(wrapper);
      return input;
    };
    // Logarithmic, so each step of the slider zooms by the same factor.
    zoom.append(button("tool fit-button", "Fit", "Zoom to fit the whole song (Z)", () => this.run("zoomFit")));
    this.zoomSlider = slider("↔", "Horizontal zoom (+ / − or Ctrl+scroll)", (p) => this.setZoom(MIN_ZOOM * Math.pow(MAX_ZOOM / MIN_ZOOM, p), true));
    this.heightSlider = slider("↕", "Track height (Shift+↑ / Shift+↓)", (p) => this.setRowHeight(MIN_ROW + (MAX_ROW - MIN_ROW) * p));

    const right = html("div", "daw-group");
    right.append(
      this.tools,
      zoom,
      (this.undoButton = button("tool", "Undo", "Undo (Ctrl+Z)", () => daw("undo"))),
      (this.redoButton = button("tool", "Redo", "Redo (Ctrl+Shift+Z)", () => daw("redo"))));

    this.viewTabs = html("div", "segmented view-tabs");
    for (const view of ["arrange", "mixer"]) {
      const tab = button("", view === "arrange" ? "Tracks" : "Mixer", view === "arrange" ? "Tracks and clips (Tab)" : "Mixer (Tab)", () => this.setView(view));
      tab.dataset.view = view;
      this.viewTabs.append(tab);
    }
    right.append(this.viewTabs, button("tool icon", "?", "Keyboard shortcuts (?)", () => this.toggleShortcuts()));

    bar.append(files, this.projectName, transport, clock, timing, right);
  }

  renderTransport() {
    const s = this.session;
    if (!s) return;

    this.projectName.replaceChildren(html("strong", "", s.project.name), html("span", "dirty", s.project.dirty ? "•" : ""));
    this.projectName.title = s.project.open ? s.project.path : "Not saved yet. Save to keep your work in a project folder.";
    this.saveButton.classList.toggle("is-dirty", s.project.dirty || !s.project.open);
    const recent = (s.recent ?? []).filter((r) => r.path !== s.project.path);
    const recentKey = JSON.stringify(recent);
    if (recentKey !== this.recentKey) {
      this.recentKey = recentKey;
      this.recentSelect.replaceChildren(new Option("Recent…", ""), ...recent.map((r) => new Option(r.name, r.path)));
      this.recentSelect.hidden = recent.length === 0;
    }
    if (document.activeElement !== this.tempoInput) this.tempoInput.value = Math.round(s.tempo * 100) / 100;
    this.countIn.value = String(s.countIn);
    this.signature.value = String(s.beatsPerBar ?? 4);
    const songKey = s.key?.key ?? -1;
    this.keyButton.textContent = songKey >= 0 ? `Key ${NOTE_NAMES[songKey]}${s.key.minor ? "m" : ""}` : "Key";
    this.keyButton.classList.toggle("is-set", songKey >= 0);
    this.renderKeyPanel();
    if (document.activeElement !== this.clickVolume) this.clickVolume.value = String(Math.round((s.metronomeVolume ?? 1) * 100));
    this.gridSelect.value = String(this.grid);
    this.gridSelect.disabled = !this.snap;
    this.returnButton.setAttribute("aria-pressed", String(this.returnOnStop));
    this.metronomeButton.setAttribute("aria-pressed", String(s.metronome));
    this.loopButton.setAttribute("aria-pressed", String(s.loop.on));
    this.snapButton.setAttribute("aria-pressed", String(this.snap));
    this.followButton.setAttribute("aria-pressed", String(this.follow));
    [...this.tools.children].forEach((b) => b.classList.toggle("is-selected", b.dataset.tool === this.tool));
    if (document.activeElement !== this.zoomSlider)
      this.zoomSlider.value = String(Math.round(1000 * Math.log(this.zoom / MIN_ZOOM) / Math.log(MAX_ZOOM / MIN_ZOOM)));
    if (document.activeElement !== this.heightSlider)
      this.heightSlider.value = String(Math.round(1000 * (this.rowHeight - MIN_ROW) / (MAX_ROW - MIN_ROW)));
    this.undoButton.disabled = !s.canUndo;
    this.redoButton.disabled = !s.canRedo;
    [...this.viewTabs.children].forEach((tab) => tab.classList.toggle("is-selected", tab.dataset.view === this.view));
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
      daw("loop", { on: true, start, end: start + 4 * this.bar });
    }
  }

  //============================================================================ arrange
  buildArrange() {
    this.arrange = this.root.querySelector(".arrange");
    this.inner = html("div", "arrange-inner");

    const rulerRow = html("div", "ruler-row");
    const corner = html("div", "corner");
    this.clearButton = button("tool clear-mute-solo", "Clear M/S", "Turn every mute and solo off", () => this.run("clearMuteSolo"));
    corner.append(button("tool", "+ Track", "Add an audio track (Ctrl+T)", () => this.addTrack()),
                  button("tool", "+ MIDI", "Add a MIDI track with a built-in synth (Ctrl+Shift+T). Pick another instrument in the library.", () => this.addMidiTrack()),
                  this.clearButton);
    this.ruler = html("div", "ruler");
    this.ruler.title = "Click to move the playhead · Drag to mark a loop";
    this.loopBand = html("div", "loop-band");
    this.ruler.append(this.loopBand);
    rulerRow.append(corner, this.ruler);

    this.rows = html("div", "track-rows");
    this.masterRow = html("div", "track-row master-row");
    this.loopShade = html("div", "loop-shade");
    this.playhead = html("div", "daw-playhead");
    this.emptyHint = html("div", "arrange-empty");
    this.emptyHint.innerHTML = "Arm a track and press <b>R</b> to record, or <b>Import…</b> a file.";

    this.cutGuide = html("div", "cut-guide");
    this.cutGuide.hidden = true;
    this.inner.append(rulerRow, this.rows, this.masterRow, this.loopShade, this.playhead, this.cutGuide);

    // Scrolling by hand while playing switches Follow off, so you can look around
    // (and edit) elsewhere while the song keeps playing.
    this.arrange.addEventListener("scroll", () => {
      if (!this.wavesQueued) {
        this.wavesQueued = true;
        requestAnimationFrame(() => { this.wavesQueued = false; this.redrawWaves(); });
      }
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
      const anchor = event.clientX - box.left - HEAD_WIDTH;
      const seconds = (this.arrange.scrollLeft + anchor) / this.zoom;
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
      const time = this.snapTime((event.clientX - box.left - HEAD_WIDTH) / this.zoom, event);
      const clipBox = clip.getBoundingClientRect();
      this.cutGuide.style.left = `${HEAD_WIDTH + time * this.zoom}px`;
      this.cutGuide.style.top = `${clipBox.top - box.top}px`;
      this.cutGuide.style.height = `${clipBox.height}px`;
    });
    this.arrange.addEventListener("pointerleave", () => { this.cutGuide.hidden = true; });
  }

  // Keeps the playhead (or the left edge, if it's off screen) in place while zooming.
  setZoom(zoom, fromSlider = false) {
    const before = this.zoom;
    this.zoom = clamp(zoom, MIN_ZOOM, MAX_ZOOM);
    const visible = this.arrange.clientWidth - HEAD_WIDTH;
    const playheadX = this.status.position * before - this.arrange.scrollLeft;
    const anchor = playheadX >= 0 && playheadX <= visible ? playheadX : 0;
    const seconds = (this.arrange.scrollLeft + anchor) / before;
    this.renderArrange(true);
    this.scrollTo(seconds * this.zoom - anchor);
    if (!fromSlider) this.renderTransport();
  }

  setRowHeight(height) {
    this.rowHeight = Math.round(clamp(height, MIN_ROW, MAX_ROW));
    this.renderArrange(true);
    this.renderTransport();
  }

  setFollow(on) {
    this.follow = on;
    this.renderTransport();
    if (on) this.movePlayhead(true);
  }

  setTool(tool) {
    this.tool = tool;
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
    const visible = Math.max(0, this.arrange.clientWidth - HEAD_WIDTH) / this.zoom;
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
    const id = await daw("addMidiClip", { track: track.id, start: Math.max(0, start), length: this.bar });
    if (typeof id === "number" && id > 0) {
      this.showWhenDrawn({ track: track.id, clip: id });
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
      if (x < left || x > left + this.arrange.clientWidth - HEAD_WIDTH - 40) this.scrollTo(x - 40);
    }
  }

  renderArrange(force = false) {
    const session = this.session;
    if (!session) return;

    const width = this.timelineSeconds() * this.zoom;
    const signature = JSON.stringify([
      session.tracks.map(({ inserts, ...track }) => [track, inserts.length]),
      session.master.volume, session.master.inserts.length, session.tempo, session.loop, session.beatsPerBar, session.markers,
      this.zoom, width, this.selectedTrack, this.selectedClip, this.snap, this.rowHeight,
    ]);
    if (!force && signature === this.arrangeSignature) return this.movePlayhead();
    this.arrangeSignature = signature;

    this.inner.style.width = `${HEAD_WIDTH + width}px`;
    this.root.style.setProperty("--row", `${this.rowHeight}px`);
    this.root.style.setProperty("--clip", `${this.rowHeight - 8}px`);
    this.inner.classList.toggle("rows-medium", this.rowHeight < 76);
    this.inner.classList.toggle("rows-small", this.rowHeight < 58);
    this.inner.style.setProperty("--bar", `${this.bar * this.zoom}px`);
    this.inner.style.setProperty("--beat", `${this.beat * this.zoom}px`);
    this.inner.classList.toggle("dense", this.beat * this.zoom < 8);
    this.renderRuler(width);
    this.showLoop(session.loop);
    this.renderMarkers();

    this.meters.clear();
    const scroll = this.arrange.scrollLeft;
    this.rows.replaceChildren(...session.tracks.map((track, index) => this.createTrackRow(track, index)));
    this.renderMasterRow();
    this.emptyHint.hidden = session.tracks.some((track) => track.clips.length > 0);
    this.clearButton.hidden = !session.tracks.some((track) => track.mute || track.solo);
    this.scrollTo(scroll);
    this.updateRecording();
    this.movePlayhead();
    this.revealPending();
  }

  renderRuler(width) {
    [...this.ruler.querySelectorAll(".bar-label")].forEach((label) => label.remove());
    const barSeconds = this.bar;
    const barPixels = barSeconds * this.zoom;
    const every = [1, 2, 4, 8, 16, 32, 64].find((n) => n * barPixels >= 44) ?? 128;

    for (let bar = 0; bar * barSeconds * this.zoom < width; bar += every) {
      const label = html("span", "bar-label", String(bar + 1));
      label.style.left = `${bar * barPixels}px`;
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
    this.loopShade.style.left = `${HEAD_WIDTH + loop.start * this.zoom}px`;
  }

  createTrackRow(track, index) {
    const row = html("div", "track-row");
    row.dataset.track = track.id;
    row.style.setProperty("--track-color", trackColour(track, index));
    row.classList.toggle("is-selected", this.selectedTrack === track.id);
    row.classList.toggle("is-muted", track.mute);
    row.classList.toggle("is-armed", track.arm);

    const head = html("div", "track-head");
    head.addEventListener("pointerdown", (event) => {
      if (!event.target.closest("button, input, .hslider")) this.select(track.id, null);
    });

    const name = html("input", "track-name");
    Object.assign(name, { value: track.name, spellcheck: false, title: "Rename the track" });
    name.addEventListener("change", () => daw("setTrack", { id: track.id, name: name.value.trim() || track.name }));
    name.addEventListener("keydown", (event) => { if (event.key === "Enter") name.blur(); });

    const toggles = html("div", "track-toggles");
    const toggle = (key, text, title) => {
      const b = button(`tt tt-${key}`, text, title, (event) => this.toggleTrackKey(track, key, !track[key], event));
      b.setAttribute("aria-pressed", String(Boolean(track[key])));
      return b;
    };
    const isMidi = track.kind === "midi";
    row.classList.toggle("is-midi", isMidi);
    toggles.append(toggle("mute", "M", "Mute"), toggle("solo", "S", "Solo (Alt+click: solo only this track)"),
                   toggle("arm", "●", isMidi ? "Arm to record from your MIDI keyboard (you hear it while armed)" : "Arm for recording"),
                   toggle("monitor", "In", isMidi ? "Play this track's instrument from your MIDI keyboard" : "Hear the mic through this track's effects"));

    const meter = new Level(false, track.id);
    this.meters.set(track.id, meter);

    const fx = button("fx-count", track.inserts.length ? `${track.inserts.length} fx` : "+ fx", "Effects on this track", () => this.select(track.id, null));
    const remove = button("mini is-danger track-remove", "×", "Delete the track", async () => {
      await daw("removeTrack", { id: track.id });
      if (track.clips.length > 0) toast(`Deleted "${track.name}". Ctrl+Z brings it back.`);
    });

    const send = (key) => (value, first) => daw("setTrack", { id: track.id, [key]: value, continue: !first });
    const volume = new HSlider({ value: track.volume, defaultValue: 0, toPosition: Fader.toPosition, fromPosition: Fader.toDecibels,
                                 format: (v) => `${dbText(v)} dB`, title: "Volume", onChange: send("volume") });
    const pan = new HSlider({ value: track.pan, defaultValue: 0, toPosition: (v) => (v + 1) / 2,
                              fromPosition: (p) => (Math.abs(p - 0.5) < 0.01 ? 0 : Math.round((p * 2 - 1) * 100) / 100),
                              format: panText, title: "Pan", bipolar: true, onChange: send("pan") });
    pan.element.classList.add("is-pan");

    const top = html("div", "track-head-top");
    const duplicate = button("mini track-duplicate", "⧉", "Duplicate the track with its clips and effects (Ctrl+Shift+D)", () => this.duplicateTrack(track.id));
    top.append(html("span", "track-swatch"), name, duplicate, remove);
    const bottom = html("div", "track-head-bottom");
    bottom.append(toggles, fx);
    let chip = null;
    if (isMidi) {
      const instrument = track.instrument ?? {};
      chip = button("instrument-chip", `${instrument.error ? "⚠ " : "♫ "}${instrument.name || "Instrument"}`,
        instrument.error ? instrument.error
          : instrument.hasWindow ? `${instrument.name}: open its window` : `${instrument.name} (built-in). Drag another instrument here from the library to swap it.`,
        () => {
          this.select(track.id, null);
          if (instrument.hasWindow && !instrument.error) daw("showInstrument", { track: track.id });
          else if (instrument.error) toast(instrument.error, "is-error");
          else toast("Double-click the lane to add a MIDI clip; pick another instrument in the library or the piano roll.");
        });
      chip.classList.toggle("has-error", Boolean(instrument.error));
    }
    const mix = html("div", "track-head-mix");
    mix.append(volume.element, pan.element);
    head.append(top, ...(chip ? [chip] : []), bottom, mix, meter.element);

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
        const bar = this.bar;
        this.addMidiClip(track, Math.floor(time / bar + 1e-6) * bar);
      });
    }

    for (const clip of track.clips) lane.append(this.createClip(clip, track));

    row.append(head, lane);
    return row;
  }

  renderMasterRow() {
    const master = this.session.master;
    const head = html("div", "track-head");
    head.addEventListener("pointerdown", () => this.select(0, null));
    const meter = new Level(false, 0);
    this.meters.set(0, meter);
    const top = html("div", "track-head-top");
    top.append(html("span", "track-swatch"), html("span", "track-name is-static", "Master"),
               html("span", "master-volume", `${dbText(master.volume)} dB`));
    const fx = button("fx-count", master.inserts.length ? `${master.inserts.length} fx` : "+ fx", "Effects on the master bus", () => this.select(0, null));
    const bottom = html("div", "track-head-bottom");
    bottom.append(html("span", "track-caption", "All tracks sum here"), fx);
    head.append(top, bottom, meter.element);
    this.masterRow.classList.toggle("is-selected", this.selectedTrack === 0);
    this.masterRow.replaceChildren(head, html("div", "lane"));
  }

  createClip(clip, track) {
    const element = html("div", "clip");
    element.dataset.clip = clip.id;
    element.classList.toggle("is-selected", this.selectedClip === clip.id);
    element.classList.toggle("is-missing", clip.missing);
    element.classList.toggle("is-muted", Boolean(clip.mute));
    element.classList.toggle("is-midi", Boolean(clip.midi));
    if (clip.midi) element.addEventListener("dblclick", () => { this.select(track.id, clip.id); this.setDock(true); this.pianoRoll?.element.focus(); });
    element.title = clip.midi ? `${clip.name} (${clip.notes.length} notes)\nDouble-click to edit the notes · Drag to move · Edges to trim` : clip.missing ? `Missing file: ${clip.file}` : `${clip.name}\nDrag to move · Edges to trim · Corners to fade · S splits at the playhead`;

    const place = (start, length) => {
      element.style.left = `${start * this.zoom}px`;
      element.style.width = `${Math.max(2, length * this.zoom)}px`;
    };
    place(clip.start, clip.length);

    const canvas = html("canvas", "clip-wave");
    canvas.dataset.color = trackColour(track, this.session.tracks.indexOf(track));
    canvas.clip = clip;
    const label = html("span", "clip-name", clip.missing ? `⚠ ${clip.name}` : clip.mute ? `🔇 ${clip.name}` : clip.name);
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

    this.drawWaveform(canvas);
    if (!clip.midi) drawWarpMarkers(element, clip, this.zoom);
    element.classList.toggle("is-warped", Boolean(clip.warp?.length) || Math.abs(clip.transpose || 0) > 1e-4);

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
        const time = this.snapTime((event.clientX - box.left - HEAD_WIDTH) / this.zoom, event);
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

          const row = [...this.rows.children].find((r) => {
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

    const zoom = this.zoom;
    const view = Math.max(1, this.arrange.clientWidth - HEAD_WIDTH) / zoom;
    const left = this.arrange.scrollLeft / zoom;
    const s0 = Math.max(clip.start, left - view / 2);
    const s1 = Math.min(clip.start + clip.length, left + view * 1.5);
    canvas.hidden = s1 <= s0;
    if (canvas.hidden) return;

    const ratio = window.devicePixelRatio || 1;
    const cssLeft = (s0 - clip.start) * zoom;
    const cssWidth = Math.min((s1 - s0) * zoom, 16000 / ratio);
    const width = Math.max(1, Math.round(cssWidth * ratio));
    const height = Math.max(1, Math.round((this.rowHeight - 8) * ratio));
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
    const height = Math.max(1, Math.round((this.rowHeight - 8) * ratio));
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

  movePlayhead(force = false) {
    const x = HEAD_WIDTH + Math.max(0, this.status.position) * this.zoom;
    this.playhead.style.transform = `translateX(${x}px)`;

    // With Follow on, page along with the playhead while playing.
    if ((this.status.playing || force) && this.follow && !this.dragging && this.view === "arrange") {
      const left = this.arrange.scrollLeft, visible = this.arrange.clientWidth;
      if (x > left + visible - 40 || x < left + HEAD_WIDTH) this.scrollTo(x - HEAD_WIDTH - 40);
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

      const start = this.status.recordStart ?? 0;
      const length = Math.max(0, this.status.position - start);
      region.style.left = `${start * this.zoom}px`;
      region.style.width = `${length * this.zoom}px`;
      region.classList.toggle("is-count-in", this.status.position < start);

      const canvas = region.firstChild;
      const ratio = window.devicePixelRatio || 1;
      const width = Math.max(1, Math.round(Math.min(length * this.zoom, 16000 / ratio) * ratio));
      const height = Math.max(1, Math.round((this.rowHeight - 8) * ratio));
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
    const signature = session.tracks.map((t) => `${t.id}:${t.name}:${t.inserts.map((i) => i.type).join("+")}:${t.colour}`).join("|")
                      + `|m:${session.master.inserts.map((i) => i.type).join("+")}`;

    if (signature !== this.mixerSignature) {
      this.mixerSignature = signature;
      this.strips.clear();
      const strips = session.tracks.map((track, index) => this.createStrip(track, index));
      strips.push(html("div", "strip-gap"), this.createStrip({ ...session.master, id: 0, name: "Master" }, -1));
      this.mixer.replaceChildren(...strips);
    }

    for (const track of session.tracks) {
      const strip = this.strips.get(track.id);
      strip.fader.update(track.volume);
      strip.pan.update(track.pan);
      strip.readout.textContent = dbText(track.volume);
      for (const [key, b] of Object.entries(strip.toggles)) b.setAttribute("aria-pressed", String(Boolean(track[key])));
      strip.element.classList.toggle("is-selected", this.selectedTrack === track.id);
    }

    const master = this.strips.get(0);
    master.fader.update(session.master.volume);
    master.readout.textContent = dbText(session.master.volume);
    master.element.classList.toggle("is-selected", this.selectedTrack === 0);
  }

  createStrip(track, index) {
    const isMaster = track.id === 0;
    const element = html("div", isMaster ? "strip is-master" : "strip");
    if (!isMaster) element.dataset.track = track.id;
    element.style.setProperty("--track-color", isMaster ? "var(--accent-2)" : trackColour(track, index));
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
      daw(isMaster ? "setMaster" : "setTrack", isMaster ? { volume: value, continue: !first } : { id: track.id, [key]: value, continue: !first });

    const pan = isMaster ? { update() {}, element: html("div", "knob-spacer") }
                         : new ValueKnob({ label: "Pan", min: -1, max: 1, value: track.pan, defaultValue: 0, bipolar: true,
                                           format: (v) => (Math.abs(v) < 0.01 ? "C" : `${Math.round(Math.abs(v) * 100)}${v < 0 ? "L" : "R"}`),
                                           onChange: send("pan") });
    const fader = new Fader({ value: track.volume, onChange: send("volume") });
    const meter = new Level(true, track.id);

    const faderRow = html("div", "strip-fader");
    faderRow.append(fader.element, meter.element);
    const readout = html("div", "strip-readout");

    const toggles = {};
    const buttons = html("div", "strip-toggles");
    if (!isMaster)
      for (const [key, text, title] of [["mute", "M", "Mute"], ["solo", "S", "Solo"], ["arm", "●", "Arm"], ["monitor", "In", "Monitor"]]) {
        toggles[key] = button(`tt tt-${key}`, text, key === "solo" ? "Solo (Alt+click: solo only this track)" : title,
                              (event) => this.toggleTrackKey(track, key, toggles[key].getAttribute("aria-pressed") !== "true", event));
        buttons.append(toggles[key]);
      }

    element.append(inserts, pan.element, buttons, faderRow, readout, html("div", "strip-name", track.name));
    this.strips.set(track.id, { element, fader, pan, readout, toggles, meter });
    return element;
  }

  setView(view) {
    this.view = view;
    this.root.classList.toggle("show-mixer", view === "mixer");
    this.render();
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
    this.setDock(saved?.hidden ? false : true, this.dockHeight);

    this.dockToggle.addEventListener("click", () => this.setDock(this.root.classList.contains("dock-hidden")));
    splitter.addEventListener("dblclick", (event) => { if (event.target !== this.dockToggle) this.setDock(this.root.classList.contains("dock-hidden")); });
    splitter.addEventListener("pointerdown", (event) => {
      if (event.button !== 0 || event.target === this.dockToggle) return;
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
    if (save) try { localStorage.setItem("micfx.dock", JSON.stringify({ height, hidden: !show })); } catch { /* not kept */ }
    if (this.session) this.watchInserts(show && this.chainGains ? this.chainGains.id : -1);
  }

  select(trackId, clipId, render = true, openAdd = false) {
    const changed = this.selectedTrack !== trackId || this.selectedClip !== clipId;
    this.selectedTrack = trackId;
    this.selectedClip = clipId;
    if (changed && render) this.render();
    else if (changed) {
      this.root.querySelectorAll(".clip.is-selected, .track-row.is-selected").forEach((e) => e.classList.remove("is-selected"));
      this.root.querySelector(`.clip[data-clip="${clipId}"]`)?.classList.add("is-selected");
      this.root.querySelector(`.track-row[data-track="${trackId}"]`)?.classList.add("is-selected");
      this.renderInspector();
    }
    if (openAdd) {
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
    let owner = this.selectedTrack === 0 ? { ...session.master, id: 0, name: "Master" }
                                         : session.tracks.find((t) => t.id === this.selectedTrack);
    if (!owner && session.tracks.length) {
      owner = session.tracks[0];
      this.selectedTrack = owner.id;
    }
    const found = this.findClip(this.selectedClip);
    if (!found) this.selectedClip = null;

    // Rebuild only when what's shown changes, so knobs keep working mid-drag.
    const key = `${owner?.id}|${found?.clip.id}|${owner?.name}`;
    if (key !== this.inspectorKey) {
      this.inspectorKey = key;
      this.inspector.replaceChildren();
      this.clipPanel?.dispose?.();
      this.clipPanel = !found ? null : found.clip.midi ? this.createPianoRoll(found.clip, found.track) : this.createClipPanel(found.clip);
      if (this.clipPanel) this.inspector.append(this.clipPanel.element);
      this.rhythmPanel = owner && owner.id !== 0 && owner.kind !== "midi" // audio quantize/warp only
        ? createRhythmPanel(this, owner, found?.clip ?? null, { daw, ValueKnob, button, toast: (...a) => toast(...a) }) : null;
      if (this.rhythmPanel) this.inspector.append(this.rhythmPanel.element);
      if (owner) this.inspector.append(this.createTrackPanel(owner));
    }

    if (this.rhythmPanel && owner) this.rhythmPanel.update(owner, found?.clip ?? null);

    if (found && this.clipPanel) this.clipPanel.update(found.clip, found.track);
    if (owner) this.rack?.update(owner);
    if (owner && this.chainGains?.id === owner.id) {
      this.chainGains.input.knob.update(owner.inGain ?? 0);
      this.chainGains.output.knob.update(owner.outGain ?? 0);
    }
    this.watchInserts(owner && !this.root.classList.contains("dock-hidden") ? owner.id : -1);
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
    const threshold = html("select", "dropdown strip-threshold");
    threshold.title = "What counts as silence for Strip silence";
    [[-55, "Quiet room (−55 dB)"], [-45, "Normal (−45 dB)"], [-35, "Noisy (−35 dB)"]].forEach(([v, t]) => threshold.append(new Option(t, v)));
    threshold.value = String(this.stripThreshold ?? -45);
    threshold.addEventListener("change", () => { this.stripThreshold = Number(threshold.value); });

    // All clip controls sit in one compact column: name, knobs, then three rows of buttons.
    const toolRow = (...items) => { const r = html("div", "clip-tool-row"); r.append(...items); return r; };
    const tools = html("div", "clip-tools");
    tools.append(
      toolRow(button("tool", "Split", "Split at the playhead (S)", () => this.splitSelected()),
              button("tool", "Duplicate", "Copy after itself (Ctrl+D)", () => this.duplicateSelected()),
              button("tool is-danger", "Delete", "Delete the clip (Delete)", () => this.deleteSelected())),
      toolRow(muteButton,
              button("tool", "Normalise", "Set the clip gain so its loudest peak is −1 dB", () => this.run("normaliseClip")),
              button("tool", "Loop clip", "Loop the clip's range (Shift+L)", () => this.run("loopClip")),
              button("tool", "Zoom to clip", "Fit the clip in the view (Shift+Z)", () => this.run("zoomClip"))),
      toolRow(button("tool", "Strip silence", "Split the clip into phrases and remove the silent gaps between them", () => this.run("stripSilence")),
              threshold),
      toolRow(button("tool", "Split into stems", "Separate the song into vocals, drums, bass and other, each on its own track", () => this.run("splitStems"))));
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
    const head = html("div", "panel-head");
    element.append(head);
    head.append(html("h3", "", isMaster ? "Master effects" : `${owner.name} effects`),
                   html("p", "panel-caption", isMaster ? "Applied to the whole mix, after the tracks."
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
    if (!isMaster) effectsRow.append(button("tool", "Duplicate track", "A copy of this track with its clips and effects (Ctrl+Shift+D)", () => this.duplicateTrack(owner.id)));
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
    chain.append(input.column, list, output.column);
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
    toast = (message, kind = "") => {
      this.toastArea.textContent = message;
      this.toastArea.className = `daw-toast is-visible ${kind}`;
      clearTimeout(this.toastTimer);
      this.toastTimer = setTimeout(() => this.toastArea.classList.remove("is-visible"), 5000);
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

    if (running) {
      panel.hidden = false;
      const progress = event.progress ?? 0;
      panel.querySelector(".export-bar").style.setProperty("--progress", event.state === "loading" ? 0 : progress);
      const percent = `${Math.round(progress * 100)}%`;
      this.setStemsStatus(event.state === "downloading" ? `Downloading the separation model… ${percent}`
                        : event.state === "loading" ? "Loading the separation model…"
                        : `Separating… ${percent}`);
      return;
    }

    panel.querySelector(".export-bar").style.setProperty("--progress", 0);
    if (event.state === "done") {
      panel.hidden = true;
      toast(`Added ${event.tracks?.length ?? 0} stem tracks and muted the original clip. Ctrl+Z undoes it.`);
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

    $(".automix-close").addEventListener("click", () => this.toggleAutoMix(false));
    $(".automix-analyse").addEventListener("click", () => {
      this.setAutoMixResult("");
      daw("autoMix", { action: "analyse", style: style(), effects: effects() });
    });
    $(".automix-reference").addEventListener("click", () => {
      this.setAutoMixResult("");
      daw("autoMix", { action: "reference" });
    });
    $(".automix-cancel").addEventListener("click", () => daw("autoMix", { action: "cancel" }));
    $("[name=style]").addEventListener("change", () => {
      if (this.autoMix.plan?.kind === "mix") this.replanAutoMix({ style: style(), roles: roles(), effects: effects() });
    });
    // Which effects it may add: the plan follows at once.
    for (const box of panel.querySelectorAll(".automix-effects input"))
      box.addEventListener("change", () => {
        if (this.autoMix.plan?.kind === "mix") this.replanAutoMix({ style: style(), roles: roles(), effects: effects() });
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
    if (show) this.toggleExportPanel(false);
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

    if (event.state === "analysing") return this.setAutoMixResult("Listening…");
    if (event.state === "cancelled") return this.setAutoMixResult("Cancelled.");
    if (event.state === "error") return this.setAutoMixResult(event.error, true);
    if (event.state !== "ready") return;

    Object.assign(this.autoMix, { plan: event, applied: false, after: true });
    this.setAutoMixResult("");
    if (event.style) panel.querySelector("[name=style]").value = event.style;
    for (const [name, on] of Object.entries(event.effects ?? {})) {
      const box = panel.querySelector(`.automix-effects [name=${name}]`);
      if (box) box.checked = Boolean(on);
    }

    // Who is who: change a role and the plan follows.
    const rolesBox = panel.querySelector(".automix-roles");
    rolesBox.replaceChildren();
    for (const track of event.tracks) {
      const row = html("label", "automix-role");
      const name = html("span", "", track.name);
      name.append(html("small", "", track.active > 0 ? `${track.lufs.toFixed(1)} LUFS` : "silent"));
      const select = html("select", "dropdown");
      select.dataset.track = track.id;
      for (const [value, label] of [["voice", "Voice"], ["instrument", "Instrument"], ["backing", "Backing"], ["other", "Leave as is"]]) select.append(new Option(label, value));
      select.value = track.role;
      select.addEventListener("change", () => {
        const roles = Object.fromEntries([...rolesBox.querySelectorAll("select")].map((s) => [s.dataset.track, s.value]));
        this.replanAutoMix({ style: panel.querySelector("[name=style]").value, roles, effects: this.autoMixEffects() });
      });
      row.append(name, select);
      rolesBox.append(row);
    }

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
    if (event.kind === "mix" && event.moves.length > 0)
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
    this.projectPath = path;
    if (pathChanged) this.library?.refresh();
    document.dispatchEvent(new CustomEvent("daw-session", { detail: session }));
    if (this.active) this.render();
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
      run: daw,
      toast: (message, kind) => toast(message, kind),
      onEvent: (name, handler) => window.__JUCE__.backend.addEventListener(name, handler),
    });
    body.append(this.library.element, main);
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
    if (status.recording && !this.status.recording) this.recLevels = [];
    if (status.recording && status.position >= (status.recordStart ?? 0))
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

    const neededWidth = HEAD_WIDTH + this.timelineSeconds() * this.zoom;
    if ((lengthChanged || neededWidth > this.inner.offsetWidth + 1) && !this.dragging) this.renderArrange();
    else this.movePlayhead();
    if (status.recording || this.rows.querySelector(".rec-region")) this.updateRecording();
    this.pianoRoll?.setStatus(status);
  }

  tickMeters(elapsed) {
    const level = (id) => (id === 0 ? this.status.master : this.status.tracks?.[id]);
    for (const [id, meter] of this.meters) meter.push(level(id), elapsed);
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
    if (this.view === "mixer") for (const [id, strip] of this.strips) strip.meter.push(level(id), elapsed);
    this.status.master = undefined;
    if (this.status.tracks) this.status.tracks = {};
  }

  //============================================================================ keyboard
  // Every shortcut, for the "?" overlay. Keep in step with onKey.
  static SHORTCUTS = [
    ["Transport", [["Space", "Play / pause"], ["Enter", "Stop and go back to the start"], ["R", "Record on the armed tracks"],
                   ["← / →", "Move the playhead a beat (Shift: a bar)"], ["Home / End", "Go to the start / end of the song"],
                   ["L", "Loop on / off"], ["C", "Click (metronome) on / off"], ["F", "Follow the playhead on / off"],
                   ["K", "Add a marker at the playhead"], ["[ / ]", "Previous / next marker"], ["T", "Tap tempo"],
                   ["Click the clock", "Type a bar or time to go to"]]],
    ["Tracks", [["↑ / ↓", "Select the track above / below"], ["Ctrl+T", "New track"], ["M", "Mute the selected track"],
                ["Shift+S", "Solo the selected track"], ["Alt+click S", "Solo only that track"], ["Ctrl+Shift+D", "Duplicate the selected track"], ["A", "Arm the selected track"], ["I", "Monitor the mic on the selected track"]]],
    ["Clips", [[", / .", "Select the previous / next clip on the track"], ["S", "Split the selected clip at the playhead"],
               ["Alt+← / →", "Nudge the selected clip a beat (Shift: a bar)"], ["Ctrl+D", "Duplicate"],
               ["Ctrl+X / C / V", "Cut, copy, paste (pastes at the playhead on the selected track)"], ["Delete", "Delete the clip"],
               ["V / B / G / W", "Select, cut, gain and warp tools"], ["Shift+M", "Mute / unmute the selected clip"],
               ["Shift+L", "Loop the selected clip"], ["Alt+drag", "Move or trim without snapping"]]],
    ["View", [["Tab", "Tracks / mixer"], ["E", "Show / hide the effects panel"], ["Shift+B", "Show / hide the library"], ["Ctrl+F", "Search the library"], ["+ / −", "Zoom in / out (or Ctrl+scroll)"], ["Z / Shift+Z", "Zoom to fit the song / the selected clip"], ["Shift+↑ / ↓", "Taller / shorter tracks (or Alt+scroll)"],
              ["Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"], ["Ctrl+S", "Save (Shift: save as)"], ["?", "This list"], ["F11 / Ctrl+Cmd+F", "Full screen"], ["Esc", "Close, deselect, back to the select tool"]]],
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
          row.append(html("kbd", "", keys), html("span", "", action));
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
    if (x < left || x > left + this.arrange.clientWidth - HEAD_WIDTH - 40) this.scrollTo(x - 40);
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

  async duplicateTrack(id = this.selectedTrack) {
    if (!id) return toast("Select a track first.");
    const copy = await daw("duplicateTrack", { id });
    if (typeof copy === "number" && copy > 0) this.select(copy, null);
  }

  copyEffects(owner = this.selectedTrack) {
    if (owner == null) return toast("Select a track first.");
    const from = owner === 0 ? this.session.master : this.session.tracks.find((t) => t.id === owner);
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
    const visible = Math.max(100, this.arrange.clientWidth - HEAD_WIDTH);
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
      stripSilence: () => this.withClip(async (clip) => {
        const result = await daw("stripSilence", { id: clip.id, threshold: this.stripThreshold ?? -45 });
        if (result?.ok) {
          this.selectedClip = null;
          toast(`Kept ${result.clips} phrase${result.clips === 1 ? "" : "s"}. Ctrl+Z puts the clip back.`);
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
    };
  }

  run(command) {
    if (this.active) this.commands[command]?.();
  }

  onKey(event) {
    if (!this.active || isTyping(event)) return;
    const mod = event.ctrlKey || event.metaKey;
    const key = event.key.toLowerCase();
    const handled = () => event.preventDefault();
    const onSlider = event.target.closest?.("[role=slider]");
    const step = event.shiftKey ? this.bar : this.beat;

    if (mod && key === "z") { handled(); this.run(event.shiftKey ? "redo" : "undo"); }
    else if (mod && key === "y") { handled(); this.run("redo"); }
    else if (mod && key === "s") { handled(); this.run(event.shiftKey ? "saveAs" : "save"); }
    else if (mod && event.shiftKey && key === "d") { handled(); this.run("duplicateTrack"); }
    else if (mod && key === "d") { handled(); this.run("duplicate"); }
    else if (mod && event.shiftKey && key === "t") { handled(); this.addMidiTrack(); }
    else if (mod && key === "t") { handled(); this.run("addTrack"); }
    else if (mod && key === "x") { handled(); this.run("cut"); }
    else if (mod && key === "c") { handled(); this.run("copy"); }
    else if (mod && key === "v") { handled(); this.run("paste"); }
    else if (mod && key === "f") { handled(); this.run("searchLibrary"); }
    else if (mod) return;
    else if (event.altKey && (event.key === "ArrowLeft" || event.key === "ArrowRight")) { handled(); this.nudgeSelected(event.key === "ArrowLeft" ? -step : step); }
    else if (event.altKey) return;
    else if (onSlider && event.key.startsWith("Arrow")) return;
    else if (event.key === " ") { if (event.target.closest?.("button, [role=slider]")) return; handled(); this.run("play"); }
    else if (event.key === "Enter") { if (event.target.closest?.("button")) return; handled(); this.run("stop"); }
    else if (event.key === "ArrowLeft") { handled(); this.seek(Math.ceil(this.status.position / step - 1e-6) * step - step); }
    else if (event.key === "ArrowRight") { handled(); this.seek(Math.floor(this.status.position / step + 1e-6) * step + step); }
    else if (event.key === "ArrowUp") { handled(); event.shiftKey ? this.setRowHeight(this.rowHeight + 12) : this.selectTrackBy(-1); }
    else if (event.key === "ArrowDown") { handled(); event.shiftKey ? this.setRowHeight(this.rowHeight - 12) : this.selectTrackBy(1); }
    else if (event.key === "Home") { handled(); this.run("start"); }
    else if (event.key === "End") { handled(); this.run("end"); }
    else if (event.key === "?" || event.key === "/") { handled(); this.run("shortcuts"); }
    else if (event.key === "Tab") { handled(); this.run("toggleView"); }
    else if (event.key === "Delete" || event.key === "Backspace") { if (onSlider) return; handled(); this.run("delete"); }
    else if (event.key === "+" || event.key === "=") { handled(); this.run("zoomIn"); }
    else if (event.key === "-" || event.key === "_") { handled(); this.run("zoomOut"); }
    else if (event.key === ",") { handled(); this.selectClipBy(-1); }
    else if (event.key === ".") { handled(); this.selectClipBy(1); }
    else if (event.key === "Escape") {
      this.toggleExportPanel(false);
      this.toggleAutoMix(false);
      this.toggleShortcuts(false);
      this.setTool("select");
      this.select(this.selectedTrack, null);
    }
    else if (event.shiftKey && key === "s") { handled(); this.toggleTrack("solo"); }
    else if (event.shiftKey && key === "m") { handled(); this.run("muteClip"); }
    else if (event.shiftKey && key === "l") { handled(); this.run("loopClip"); }
    else if (event.shiftKey && key === "z") { handled(); this.run("zoomClip"); }
    else if (event.shiftKey && key === "b") { handled(); this.run("toggleLibrary"); }
    else if (event.shiftKey) return;
    else if (key === "r") { handled(); this.run("record"); }
    else if (key === "k") { handled(); this.run("addMarker"); }
    else if (key === "z") { handled(); this.run("zoomFit"); }
    else if (event.key === "[") { handled(); this.run("previousMarker"); }
    else if (event.key === "]") { handled(); this.run("nextMarker"); }
    else if (key === "t") { handled(); this.run("tapTempo"); }
    else if (key === "s") { handled(); this.run("split"); }
    else if (key === "l") { handled(); this.run("loop"); }
    else if (key === "c") { handled(); this.run("metronome"); }
    else if (key === "f") { handled(); this.run("follow"); }
    else if (key === "m") { handled(); this.toggleTrack("mute"); }
    else if (key === "a") { handled(); this.toggleTrack("arm"); }
    else if (key === "i") { handled(); this.toggleTrack("monitor"); }
    else if (key === "v") { handled(); this.setTool("select"); }
    else if (key === "b") { handled(); this.setTool("cut"); }
    else if (key === "g") { handled(); this.setTool("gain"); }
    else if (key === "w") { handled(); this.setTool("warp"); }
    else if (key === "e") { handled(); this.run("toggleDock"); }
  }
}

//==============================================================================
// Wires the Live/DAW switch in the header. Returns nothing; DAW mode is a
// standalone-app feature, so in a plugin host the switch stays hidden.
export function initDaw() {
  const root = document.querySelector(".daw");
  const modeSwitch = document.querySelector(".mode-switch");
  const subtitle = document.querySelector(".brand p");
  const liveSubtitle = subtitle.textContent;
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
    subtitle.textContent = isDaw ? "Multitrack recorder" : liveSubtitle;
    root.hidden = !isDaw;
    [...modeSwitch.children].forEach((b) => b.classList.toggle("is-selected", (b.dataset.mode === "daw") === isDaw));
    if (changed && isDaw) screen.render();
  };

  for (const b of modeSwitch.children)
    b.addEventListener("click", () => daw("mode", { daw: b.dataset.mode === "daw" }));

  document.addEventListener("daw-session", (event) => applyMode(event.detail));

  // The app's menu bar (standalone only) sends the same commands as the buttons and keys.
  window.__JUCE__.backend.addEventListener("appMenu", (action) => {
    if (action === "mode:live" || action === "mode:daw") daw("mode", { daw: action === "mode:daw" });
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
