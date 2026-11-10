// The DAW screen: tracks on a timeline, per-track effect inserts, a mixer and a
// transport. All state lives in C++ (src/DawBridge.cpp); this file draws the
// "dawSession" it receives and sends edits back through one native function,
// daw (command, args). Every edit is one undo step: drags only send on release.
import * as Juce from "./juce/index.js";
import { CATEGORIES, MODULES } from "./modules.js";
import { ARC_END, ARC_START, arcPath, clamp, formatDuration, formatValue, html, polar, svg } from "./widgets.js";

const nativeDaw = Juce.getNativeFunction("daw");

const HEAD_WIDTH = 210;           // track header column, px (matches daw.css)
const CLIP_HEIGHT = 60;           // clip height, px (matches daw.css)
const MIN_ZOOM = 4, MAX_ZOOM = 400; // pixels per second
const METER_FLOOR_DB = -60;
const FADER_MIN_DB = -60, FADER_MAX_DB = 6;
const TRACK_COLOURS = ["#8c7bff", "#3fd9c6", "#ffb547", "#ff6fb1", "#5aafff", "#9be15d", "#ff8a5c", "#c78bff"];

// Module key (C++ fx::getModuleKey) for each entry of the Live screen's catalog. The
// pitch insert holds three of them (autotune, pitch shift, harmonizer).
const MODULE_KEYS = {
  hpf: "highPass", gate: "gate", tune: "pitch", pitch: "pitch", harmony: "pitch", eq: "eq",
  comp: "compressor", deess: "deEsser", exciter: "exciter", sat: "saturation", radio: "radio",
  crush: "bitcrusher", robot: "robot", filter: "filter", wah: "autoWah", tremolo: "tremolo",
  vibrato: "vibrato", chorus: "chorus", flanger: "flanger", phaser: "phaser", doubler: "doubler",
  echo: "echo", reverb: "reverb", width: "width",
};

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
const dbText = (db) => (db <= FADER_MIN_DB ? "−∞" : `${db > 0.05 ? "+" : ""}${db.toFixed(1)}`);

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
class Level {
  constructor(vertical = false) {
    this.element = html("div", vertical ? "level-meter is-vertical" : "level-meter");
    this.fill = html("div", "level-fill");
    this.element.append(this.fill);
    this.shown = METER_FLOOR_DB;
  }

  push(db, elapsed) {
    this.shown = Math.max(db ?? METER_FLOOR_DB, this.shown - elapsed * 0.03);
    const amount = clamp((this.shown - METER_FLOOR_DB) / -METER_FLOOR_DB);
    this.fill.style.setProperty("--level", amount);
    this.element.classList.toggle("is-hot", this.shown > -1);
  }
}

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
      this.signature = signature;
      this.build();
    }

    for (const insert of owner.inserts) this.cards.get(insert.id)?.update(insert);
  }

  build() {
    this.element.replaceChildren();
    this.cards.clear();

    if (this.owner.inserts.length === 0)
      this.element.append(html("p", "inserts-empty", "No effects yet. The track plays dry; add one below."));

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

    this.element = html("section", "insert");
    this.element.style.setProperty("--cat", category.color);

    const head = html("header", "insert-head");
    const title = html("span", "insert-name", insert.type === "pitch" ? "Pitch" : main?.name ?? insert.type);
    if (insert.type !== "pitch") head.append(this.addSwitch(main.power, `Switch ${main.name} on or off`));
    head.append(title,
      button("mini", "↑", "Earlier in the chain", () => daw("moveInsert", { id: insert.id, index: index - 1 })),
      button("mini", "↓", "Later in the chain", () => daw("moveInsert", { id: insert.id, index: index + 1 })),
      button("mini is-danger", "×", "Remove this effect", () => daw("removeInsert", { id: insert.id })));
    head.children[head.children.length - 3].disabled = index === 0;
    head.children[head.children.length - 2].disabled = index === count - 1;

    this.body = html("div", "insert-body");
    this.element.append(head, this.body);

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

    this.update(insert);
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
      onChange: (v, first) => this.send(control.param, v, first),
    });
    this.controls.push({ param: control.param, widget: knob });
    return knob.element;
  }

  update(insert) {
    this.insert = insert;
    for (const { param, element } of this.switches)
      element.setAttribute("aria-pressed", String((insert.values[param] ?? 0) >= 0.5));
    for (const { param, widget } of this.controls) widget.update(insert.values[param]);
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
    this.zoom = 20;            // pixels per second
    this.snap = true;
    this.view = "arrange";     // or "mixer"
    this.selectedTrack = null; // track id, 0 = master
    this.selectedClip = null;
    this.peaks = new Map();    // file -> { seconds, peaks } | "loading"
    this.meters = new Map();   // track id -> [Level]
    this.dragging = false;
    this.pendingSession = null;

    this.buildTransport();
    this.buildArrange();
    this.buildMixer();
    this.buildInspector();
    this.buildBanners();

    window.__JUCE__.backend.addEventListener("dawSession", (session) => this.onSession(session));
    window.__JUCE__.backend.addEventListener("dawStatus", (status) => this.onStatus(status));
    window.__JUCE__.backend.addEventListener("dawExport", (progress) => this.onExport(progress));
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

  // Seconds snapped to the beat grid (unless snap is off or Alt is held).
  snapTime(seconds, event) {
    if (!this.snap || event?.altKey) return Math.max(0, seconds);
    return Math.max(0, Math.round(seconds / this.beat) * this.beat);
  }

  barsBeats(seconds) {
    const beats = Math.max(0, seconds) / this.beat;
    return `${Math.floor(beats / 4) + 1}.${Math.floor(beats % 4) + 1}`;
  }

  //============================================================================ transport
  buildTransport() {
    const bar = this.root.querySelector(".daw-transport");

    const files = html("div", "daw-group");
    files.append(
      button("tool", "New", "Start a new project", () => daw("new")),
      button("tool", "Open…", "Open a project", () => daw("open")),
      (this.saveButton = button("tool", "Save", "Save the project (Ctrl+S)", () => daw("save"))),
      button("tool", "Save as…", "Save a copy under a new name", () => daw("saveAs")),
      button("tool", "Import…", "Add an audio file at the playhead", () => daw("import", { track: this.selectedTrack ?? -1 })),
      (this.exportButton = button("tool is-accent", "Export…", "Render the mix or stems to WAV", () => this.toggleExportPanel())));

    this.projectName = html("div", "daw-project");

    const transport = html("div", "daw-group daw-transport-buttons");
    transport.append(
      button("transport-button", "", "Back to the start (Enter)", () => daw("transport", { action: "stop" })),
      (this.playButton = button("transport-button play-button", "", "Play / pause (Space)", () => this.togglePlay())),
      (this.recordButton = button("record-button", "", "Record on the armed tracks (R)", () => this.toggleRecord())));
    transport.children[0].innerHTML = `<svg viewBox="0 0 16 16" aria-hidden="true"><rect x="3" y="3" width="2" height="10" rx="0.5"/><path d="M13 3.5v9a.5.5 0 0 1-.8.4L6.3 8.4a.5.5 0 0 1 0-.8l5.9-4.5a.5.5 0 0 1 .8.4z"/></svg>`;
    this.playButton.innerHTML = `<svg class="icon-play" viewBox="0 0 16 16" aria-hidden="true"><path d="M4 2.8v10.4a.6.6 0 0 0 .9.5l8.2-5.2a.6.6 0 0 0 0-1L4.9 2.3a.6.6 0 0 0-.9.5z"/></svg><svg class="icon-pause" viewBox="0 0 16 16" aria-hidden="true"><rect x="3.5" y="2.5" width="3" height="11" rx="0.8"/><rect x="9.5" y="2.5" width="3" height="11" rx="0.8"/></svg>`;
    this.recordButton.innerHTML = `<span class="record-dot"></span><span class="record-label">Rec</span>`;
    this.recordButton.setAttribute("aria-label", "Record");
    this.playButton.setAttribute("aria-label", "Play");

    const clock = html("div", "daw-clock");
    clock.append((this.clockBars = html("span", "clock-bars", "1.1")), (this.clockTime = html("span", "clock-time", "0:00.0")));

    const timing = html("div", "daw-group");
    this.tempoInput = html("input", "tempo-input");
    Object.assign(this.tempoInput, { type: "number", min: 20, max: 400, step: 1, title: "Tempo (BPM)" });
    this.tempoInput.addEventListener("change", () => daw("tempo", { bpm: Number(this.tempoInput.value) || 120 }));
    const tempo = html("label", "tempo");
    tempo.append(this.tempoInput, html("span", "", "BPM"));

    this.countIn = html("select", "dropdown count-in");
    this.countIn.title = "Count-in before recording";
    ["No count-in", "1 bar count-in", "2 bar count-in", "4 bar count-in"].forEach((text, i) => this.countIn.append(new Option(text, [0, 1, 2, 4][i])));
    this.countIn.addEventListener("change", () => daw("countIn", { bars: Number(this.countIn.value) }));

    timing.append(tempo,
      (this.metronomeButton = button("toggle", "Click", "Metronome (C)", () => daw("metronome", { on: !this.session?.metronome }))),
      this.countIn,
      (this.loopButton = button("toggle", "Loop", "Loop the marked range (L). Drag on the ruler to mark it.", () => this.toggleLoop())),
      (this.snapButton = button("toggle", "Snap", "Snap edits to the beat (hold Alt to drag freely)", () => {
        this.snap = !this.snap;
        this.renderTransport();
      })));

    const right = html("div", "daw-group");
    right.append(
      button("tool icon", "−", "Zoom out (−)", () => this.setZoom(this.zoom / 1.5)),
      button("tool icon", "+", "Zoom in (+)", () => this.setZoom(this.zoom * 1.5)),
      (this.undoButton = button("tool", "Undo", "Undo (Ctrl+Z)", () => daw("undo"))),
      (this.redoButton = button("tool", "Redo", "Redo (Ctrl+Shift+Z)", () => daw("redo"))));

    this.viewTabs = html("div", "segmented view-tabs");
    for (const view of ["arrange", "mixer"]) {
      const tab = button("", view === "arrange" ? "Tracks" : "Mixer", view === "arrange" ? "Tracks and clips (Tab)" : "Mixer (Tab)", () => this.setView(view));
      tab.dataset.view = view;
      this.viewTabs.append(tab);
    }
    right.append(this.viewTabs);

    bar.append(files, this.projectName, transport, clock, timing, right);
  }

  renderTransport() {
    const s = this.session;
    if (!s) return;

    this.projectName.replaceChildren(html("strong", "", s.project.name), html("span", "dirty", s.project.dirty ? "•" : ""));
    this.projectName.title = s.project.open ? s.project.path : "Not saved yet. Save to keep your work in a project folder.";
    this.saveButton.classList.toggle("is-dirty", s.project.dirty || !s.project.open);
    if (document.activeElement !== this.tempoInput) this.tempoInput.value = Math.round(s.tempo * 100) / 100;
    this.countIn.value = String(s.countIn);
    this.metronomeButton.setAttribute("aria-pressed", String(s.metronome));
    this.loopButton.setAttribute("aria-pressed", String(s.loop.on));
    this.snapButton.setAttribute("aria-pressed", String(this.snap));
    this.undoButton.disabled = !s.canUndo;
    this.redoButton.disabled = !s.canRedo;
    [...this.viewTabs.children].forEach((tab) => tab.classList.toggle("is-selected", tab.dataset.view === this.view));
  }

  togglePlay() {
    daw("transport", { action: this.status.playing ? "pause" : "play" });
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
      daw("loop", { on: true, start, end: start + 16 * this.beat });
    }
  }

  //============================================================================ arrange
  buildArrange() {
    this.arrange = this.root.querySelector(".arrange");
    this.inner = html("div", "arrange-inner");

    const rulerRow = html("div", "ruler-row");
    const corner = html("div", "corner");
    corner.append(button("tool", "+ Track", "Add a track (Ctrl+T)", () => this.addTrack()));
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

    this.inner.append(rulerRow, this.rows, this.masterRow, this.loopShade, this.playhead);
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
      if (!event.ctrlKey && !event.metaKey) return;
      event.preventDefault();
      const box = this.arrange.getBoundingClientRect();
      const anchor = event.clientX - box.left - HEAD_WIDTH;
      const seconds = (this.arrange.scrollLeft + anchor) / this.zoom;
      this.setZoom(this.zoom * Math.exp(-event.deltaY * 0.002));
      this.arrange.scrollLeft = seconds * this.zoom - anchor;
    }, { passive: false });
  }

  setZoom(zoom) {
    this.zoom = clamp(zoom, MIN_ZOOM, MAX_ZOOM);
    this.renderArrange(true);
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
    if (typeof id === "number") this.select(id, null);
  }

  renderArrange(force = false) {
    const session = this.session;
    if (!session) return;

    const width = this.timelineSeconds() * this.zoom;
    const signature = JSON.stringify([
      session.tracks.map(({ inserts, ...track }) => [track, inserts.length]),
      session.master.volume, session.master.inserts.length, session.tempo, session.loop,
      this.zoom, width, this.selectedTrack, this.selectedClip, this.snap,
    ]);
    if (!force && signature === this.arrangeSignature) return this.movePlayhead();
    this.arrangeSignature = signature;

    this.inner.style.width = `${HEAD_WIDTH + width}px`;
    this.inner.style.setProperty("--bar", `${this.beat * 4 * this.zoom}px`);
    this.inner.style.setProperty("--beat", `${this.beat * this.zoom}px`);
    this.inner.classList.toggle("dense", this.beat * this.zoom < 8);
    this.renderRuler(width);
    this.showLoop(session.loop);

    this.meters.clear();
    const scroll = this.arrange.scrollLeft;
    this.rows.replaceChildren(...session.tracks.map((track, index) => this.createTrackRow(track, index)));
    this.renderMasterRow();
    this.emptyHint.hidden = session.tracks.some((track) => track.clips.length > 0);
    this.arrange.scrollLeft = scroll;
    this.movePlayhead();
  }

  renderRuler(width) {
    [...this.ruler.querySelectorAll(".bar-label")].forEach((label) => label.remove());
    const barSeconds = this.beat * 4;
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

    const head = html("div", "track-head");
    head.addEventListener("pointerdown", (event) => {
      if (!event.target.closest("button, input")) this.select(track.id, null);
    });

    const name = html("input", "track-name");
    Object.assign(name, { value: track.name, spellcheck: false, title: "Rename the track" });
    name.addEventListener("change", () => daw("setTrack", { id: track.id, name: name.value.trim() || track.name }));
    name.addEventListener("keydown", (event) => { if (event.key === "Enter") name.blur(); });

    const toggles = html("div", "track-toggles");
    const toggle = (key, text, title) => {
      const b = button(`tt tt-${key}`, text, title, () => daw("setTrack", { id: track.id, [key]: !track[key] }));
      b.setAttribute("aria-pressed", String(Boolean(track[key])));
      return b;
    };
    toggles.append(toggle("mute", "M", "Mute"), toggle("solo", "S", "Solo"),
                   toggle("arm", "●", "Arm for recording"), toggle("monitor", "In", "Hear the mic through this track's effects"));

    const meter = new Level();
    this.meters.set(track.id, meter);

    const fx = button("fx-count", track.inserts.length ? `${track.inserts.length} fx` : "+ fx", "Effects on this track", () => this.select(track.id, null));
    const remove = button("mini is-danger track-remove", "×", "Delete the track", async () => {
      await daw("removeTrack", { id: track.id });
      if (track.clips.length > 0) toast(`Deleted "${track.name}". Ctrl+Z brings it back.`);
    });

    const top = html("div", "track-head-top");
    top.append(html("span", "track-swatch"), name, remove);
    const bottom = html("div", "track-head-bottom");
    bottom.append(toggles, fx);
    head.append(top, bottom, meter.element);

    const lane = html("div", "lane");
    lane.addEventListener("pointerdown", (event) => {
      if (event.target !== lane) return;
      this.select(track.id, null);
      const box = lane.getBoundingClientRect();
      daw("transport", { action: "seek", time: Math.max(0, (event.clientX - box.left) / this.zoom) });
    });

    for (const clip of track.clips) lane.append(this.createClip(clip, track));

    row.append(head, lane);
    return row;
  }

  renderMasterRow() {
    const master = this.session.master;
    const head = html("div", "track-head");
    head.addEventListener("pointerdown", () => this.select(0, null));
    const meter = new Level();
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
    element.title = clip.missing ? `Missing file: ${clip.file}` : `${clip.name}\nDrag to move · Edges to trim · Corners to fade · S splits at the playhead`;

    const place = (start, length) => {
      element.style.left = `${start * this.zoom}px`;
      element.style.width = `${Math.max(2, length * this.zoom)}px`;
    };
    place(clip.start, clip.length);

    const canvas = html("canvas", "clip-wave");
    canvas.dataset.color = trackColour(track, this.session.tracks.indexOf(track));
    const label = html("span", "clip-name", clip.missing ? `⚠ ${clip.name}` : clip.name);
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

    this.drawWaveform(canvas, clip);

    element.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      event.stopPropagation();
      this.select(track.id, clip.id, false);
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
        } else if (mode === "trim-end") {
          const newEnd = clamp(this.snapTime(end + dt, e), clip.start + 0.01, maxEnd);
          edit = { command: "trimEnd", args: { id: clip.id, end: newEnd } };
          place(clip.start, newEnd - clip.start);
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

  async drawWaveform(canvas, clip) {
    let peaks = this.peaks.get(clip.file);

    if (!peaks && !clip.missing) {
      this.peaks.set(clip.file, "loading");
      const result = await nativeDaw("peaks", { file: clip.file });
      if (result?.peaks) this.peaks.set(clip.file, result);
      else {
        this.peaks.delete(clip.file); // still decoding: try again shortly
        setTimeout(() => canvas.isConnected && this.drawWaveform(canvas, clip), 400);
        return;
      }
      peaks = result;
      if (!canvas.isConnected) return;
    }

    if (!peaks || peaks === "loading") {
      if (peaks === "loading") setTimeout(() => canvas.isConnected && this.drawWaveform(canvas, clip), 200);
      return;
    }

    const ratio = window.devicePixelRatio || 1;
    const cssWidth = Math.min(clip.length * this.zoom, 16000 / ratio);
    const width = Math.max(1, Math.round(cssWidth * ratio));
    const height = Math.max(1, Math.round(CLIP_HEIGHT * ratio));
    canvas.width = width;
    canvas.height = height;

    const context = canvas.getContext("2d");
    context.fillStyle = canvas.dataset.color || "#8c7bff";
    const gain = Math.pow(10, (clip.gain || 0) / 20);
    const perPixel = clip.length / width / peaks.seconds;
    const first = clip.offset / peaks.seconds;
    const middle = height / 2;

    for (let x = 0; x < width; ++x) {
      const a = Math.floor(first + x * perPixel);
      const b = Math.max(a + 1, Math.floor(first + (x + 1) * perPixel));
      let peak = 0;
      for (let i = a; i < Math.min(b, peaks.peaks.length); ++i) peak = Math.max(peak, peaks.peaks[i]);
      const half = Math.max(0.5 * ratio, Math.min(1, peak * gain) * (middle - ratio));
      context.fillRect(x, middle - half, 1, half * 2);
    }
  }

  movePlayhead() {
    const x = HEAD_WIDTH + this.status.position * this.zoom;
    this.playhead.style.transform = `translateX(${x}px)`;

    // Follow the playhead while playing.
    if (this.status.playing && !this.dragging && this.view === "arrange") {
      const left = this.arrange.scrollLeft, visible = this.arrange.clientWidth;
      if (x > left + visible - 40 || x < left + HEAD_WIDTH) this.arrange.scrollLeft = x - HEAD_WIDTH - 40;
    }
  }

  layoutArrange() {
    if (this.session && this.inner.offsetWidth < this.arrange.clientWidth) this.renderArrange();
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
    const meter = new Level(true);

    const faderRow = html("div", "strip-fader");
    faderRow.append(fader.element, meter.element);
    const readout = html("div", "strip-readout");

    const toggles = {};
    const buttons = html("div", "strip-toggles");
    if (!isMaster)
      for (const [key, text, title] of [["mute", "M", "Mute"], ["solo", "S", "Solo"], ["arm", "●", "Arm"], ["monitor", "In", "Monitor"]]) {
        toggles[key] = button(`tt tt-${key}`, text, title, () => daw("setTrack", { id: track.id, [key]: toggles[key].getAttribute("aria-pressed") !== "true" }));
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
    if (openAdd) this.inspector.querySelector(".add-effect")?.focus();
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
      this.clipPanel = found ? this.createClipPanel(found.clip) : null;
      if (this.clipPanel) this.inspector.append(this.clipPanel.element);
      if (owner) this.inspector.append(this.createTrackPanel(owner));
    }

    if (found && this.clipPanel) this.clipPanel.update(found.clip);
    if (owner) this.rack?.update(owner);
  }

  createClipPanel(clip) {
    const element = html("section", "panel clip-panel");
    element.append(html("h3", "", "Clip"), html("p", "panel-caption", clip.name));
    const row = html("div", "insert-controls");
    const id = clip.id;
    const current = () => this.findClip(id)?.clip ?? clip;

    const gain = new ValueKnob({ label: "Gain", min: -24, max: 24, value: clip.gain, defaultValue: 0, bipolar: true, unit: "dB",
                                 onChange: (v, first) => daw("clipGain", { id, gain: v, continue: !first }) });
    const fadeFormat = (v) => `${v.toFixed(2)} s`;
    const fadeIn = new ValueKnob({ label: "Fade in", min: 0, max: Math.max(0.01, clip.length), skew: 0.5, value: clip.fadeIn, defaultValue: 0,
                                   format: fadeFormat, onChange: (v, first) => daw("clipFades", { id, fadeIn: v, fadeOut: current().fadeOut, continue: !first }) });
    const fadeOut = new ValueKnob({ label: "Fade out", min: 0, max: Math.max(0.01, clip.length), skew: 0.5, value: clip.fadeOut, defaultValue: 0,
                                    format: fadeFormat, onChange: (v, first) => daw("clipFades", { id, fadeIn: current().fadeIn, fadeOut: v, continue: !first }) });
    row.append(gain.element, fadeIn.element, fadeOut.element);

    const actions = html("div", "panel-actions");
    actions.append(
      button("tool", "Split", "Split at the playhead (S)", () => this.splitSelected()),
      button("tool", "Duplicate", "Copy after itself (Ctrl+D)", () => this.duplicateSelected()),
      button("tool is-danger", "Delete", "Delete the clip (Delete)", () => this.deleteSelected()));
    element.append(row, actions);

    return { element, update: (c) => { gain.update(c.gain); fadeIn.update(c.fadeIn); fadeOut.update(c.fadeOut); } };
  }

  createTrackPanel(owner) {
    const element = html("section", "panel track-panel");
    const isMaster = owner.id === 0;
    element.append(html("h3", "", isMaster ? "Master effects" : `${owner.name} effects`),
                   html("p", "panel-caption", isMaster ? "Applied to the whole mix, after the tracks." : "The take is recorded dry. These play on top, and you can change them any time."));

    if (!isMaster) {
      const colours = html("div", "colour-row");
      TRACK_COLOURS.forEach((colour) => {
        const swatch = button("colour-swatch", "", "Track colour", () => daw("setTrack", { id: owner.id, colour }));
        swatch.style.background = colour;
        colours.append(swatch);
      });
      element.append(colours);
    }

    const list = html("div", "insert-list");
    this.rack = new InsertRack(list, owner);
    element.append(list);

    const add = html("select", "dropdown add-effect");
    add.append(new Option("+ Add effect…", ""));
    for (const [categoryId, category] of Object.entries(CATEGORIES)) {
      const group = document.createElement("optgroup");
      group.label = category.name;
      for (const module of MODULES.filter((m) => m.category === categoryId)) group.append(new Option(module.name, module.id));
      add.append(group);
    }
    add.addEventListener("change", async () => {
      const module = MODULES.find((m) => m.id === add.value);
      add.value = "";
      if (!module) return;
      const id = await daw("addInsert", { track: owner.id, type: MODULE_KEYS[module.id] });
      // The pitch insert switches on autotune by default; switch to the part that was picked.
      if (id >= 0 && MODULE_KEYS[module.id] === "pitch" && module.id !== "tune") {
        await daw("setInsert", { id, param: "tuneOn", value: 0, continue: true });
        await daw("setInsert", { id, param: module.power, value: 1, continue: true });
      }
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
    element.append(footer);
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
    document.dispatchEvent(new CustomEvent("daw-session", { detail: session }));
    if (this.active) this.render();
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
    this.status = status;
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
  }

  tickMeters(elapsed) {
    const level = (id) => (id === 0 ? this.status.master : this.status.tracks?.[id]);
    for (const [id, meter] of this.meters) meter.push(level(id), elapsed);
    if (this.view === "mixer") for (const [id, strip] of this.strips) strip.meter.push(level(id), elapsed);
    this.status.master = undefined;
    if (this.status.tracks) this.status.tracks = {};
  }

  onKey(event) {
    if (!this.active || isTyping(event)) return;
    const mod = event.ctrlKey || event.metaKey;
    const key = event.key.toLowerCase();
    const handled = () => event.preventDefault();

    if (mod && key === "z") { handled(); daw(event.shiftKey ? "redo" : "undo"); }
    else if (mod && key === "y") { handled(); daw("redo"); }
    else if (mod && key === "s") { handled(); daw(event.shiftKey ? "saveAs" : "save"); }
    else if (mod && key === "d") { handled(); this.duplicateSelected(); }
    else if (mod && key === "t") { handled(); this.addTrack(); }
    else if (mod || event.altKey) return;
    else if (event.key === " ") { if (event.target.closest?.("button, [role=slider]")) return; handled(); this.togglePlay(); }
    else if (event.key === "Enter") { handled(); daw("transport", { action: "stop" }); }
    else if (key === "r") { handled(); this.toggleRecord(); }
    else if (key === "s") { handled(); this.splitSelected(); }
    else if (key === "l") { handled(); this.toggleLoop(); }
    else if (key === "c") { handled(); daw("metronome", { on: !this.session?.metronome }); }
    else if (event.key === "Tab") { handled(); this.setView(this.view === "arrange" ? "mixer" : "arrange"); }
    else if (event.key === "Delete" || event.key === "Backspace") { if (event.target.closest?.("[role=slider]")) return; handled(); this.deleteSelected(); }
    else if (event.key === "+" || event.key === "=") { handled(); this.setZoom(this.zoom * 1.5); }
    else if (event.key === "-") { handled(); this.setZoom(this.zoom / 1.5); }
    else if (event.key === "Escape") { this.toggleExportPanel(false); this.select(this.selectedTrack, null); }
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
  nativeDaw("session", {}).then((session) => session && screen.onSession(session));
}
