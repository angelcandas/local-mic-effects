// The piano roll: edits the notes of one MIDI clip, in the effects dock under the tracks.
//
//   click an empty spot      add a note (snapped to the grid; the last length you used)
//   drag a note              move it in time and pitch (Alt: no snapping)
//   drag a note's right edge change its length
//   drag on empty space      select notes in a box; Shift adds to the selection
//   double-click a note      delete it
//   keys on the left         play the note (on the armed/monitored MIDI tracks)
//   wheel / Shift+wheel      scroll pitch / time;  Ctrl+wheel zooms time
//   Delete, ↑↓ (Shift: octave), ←→ (one grid step), Ctrl+A, Ctrl+D, Q (quantize)
//   Ctrl+C / Ctrl+X / Ctrl+V copy, cut and paste notes (pasted at the playhead)
//   the song key's rows are tinted, its root a little more (Song key panel)
//   under each chord of the chord track, its notes' rows are tinted (gold), the root more
//
// Under the roll, a controller lane (picked in the head: mod wheel, expression, pitch bend,
// pressure, any CC) shows the clip's CTRL points as a stepped line. Drag on empty space to
// draw (it replaces the points under the stroke), drag a point to move it, double-click or
// Delete to remove it. One "editControllers" command (one undo step) per gesture.
//
// Notes live in C++ (the clip's NOTE children); every gesture is one "editNotes" command
// (one undo step), and the roll redraws from the session that comes back. Note times are
// seconds from the start of the clip's content (see src/daw/Edit.h).
import { clamp, html } from "./widgets.js";

const ROW = 12;          // px per semitone
const KEYS = 52;         // keyboard width, px
const NAMES = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
const BLACK = new Set([1, 3, 6, 8, 10]);
const GRIDS = [[1, "1/4"], [0.5, "1/8"], [0.25, "1/16"], [1 / 3, "1/8 T"], [0.125, "1/32"]]; // in beats
const EDGE = 6;          // px: grabbing a note this close to its end resizes it

export const noteName = (pitch) => `${NAMES[pitch % 12]}${Math.floor(pitch / 12) - 1}`;

const LANE_HEIGHT = 110;   // px
const LANES = [
  { id: "off", name: "Controller lane: off" },
  { id: "cc:1", name: "Mod wheel (CC 1)", kind: "cc", number: 1 },
  { id: "cc:11", name: "Expression (CC 11)", kind: "cc", number: 11 },
  { id: "cc:7", name: "Volume (CC 7)", kind: "cc", number: 7 },
  { id: "cc:10", name: "Pan (CC 10)", kind: "cc", number: 10 },
  { id: "cc:64", name: "Sustain pedal (CC 64)", kind: "cc", number: 64 },
  { id: "cc:74", name: "Brightness (CC 74)", kind: "cc", number: 74 },
  { id: "bend", name: "Pitch bend", kind: "bend", number: 0 },
  { id: "pressure", name: "Pressure (aftertouch)", kind: "pressure", number: 0 },
];
const laneId = (point) => (point.kind === "cc" ? `cc:${point.number}` : point.kind);
const laneRange = (kind) => (kind === "bend" ? [-8192, 8191] : [0, 127]);

let savedGrid = 0.25;
let savedLane = "off";
let noteClipboard = null; // copied notes, times from the first one: [{ pitch, time, length, velocity }]
const SCALES = { major: new Set([0, 2, 4, 5, 7, 9, 11]), minor: new Set([0, 2, 3, 5, 7, 8, 10]) };

export class PianoRoll {
  // options: { clip, track, colour, session, daw (command, args) -> result, toast (text) }
  constructor({ clip, track, colour, session, daw, toast }) {
    this.daw = daw;
    this.toast = toast;
    this.colour = colour || "#8c7bff";
    this.clip = clip;
    this.track = track;
    this.session = session;
    this.notes = clip.notes.map((n) => ({ ...n }));
    this.hasLanes = Array.isArray(clip.controllers); // not for a pattern's part
    this.controllers = (clip.controllers ?? []).map((p) => ({ ...p }));
    this.lane = this.hasLanes ? savedLane : "off";
    this.laneSelected = new Set();
    this.selected = new Set();
    this.grid = savedGrid;
    this.lastLength = null;   // seconds; null: one grid step
    this.velocity = 100;
    this.held = new Set();    // from the MIDI input
    this.position = -1;       // timeline seconds of the playhead
    this.gesture = null;

    this.element = html("section", "panel piano-panel");
    this.element.tabIndex = 0;
    this.buildHead();

    this.stage = html("div", "roll-stage");
    this.view = html("div", "roll-view");
    this.canvas = html("canvas", "roll-canvas");
    this.view.append(this.canvas);
    this.laneView = html("div", "roll-view roll-lane");
    this.laneCanvas = html("canvas", "roll-canvas");
    this.laneView.append(this.laneCanvas);
    this.laneView.hidden = true;
    this.stage.append(this.view, this.laneView);
    this.element.append(this.stage);

    this.zoom = null;     // px per second; fitted on first layout
    this.scrollX = 0;     // seconds into the clip's window
    this.scrollY = null;  // px from the top (pitch 127); centred on the notes on first layout

    this.canvas.addEventListener("pointerdown", (e) => this.onPointerDown(e));
    this.canvas.addEventListener("dblclick", (e) => this.onDoubleClick(e));
    this.canvas.addEventListener("pointermove", (e) => { if (!this.gesture) this.updateCursor(e); });
    this.view.addEventListener("wheel", (e) => this.onWheel(e), { passive: false });
    this.laneView.addEventListener("wheel", (e) => this.onWheel(e, true), { passive: false });
    this.laneCanvas.addEventListener("pointerdown", (e) => this.onLaneDown(e));
    this.laneCanvas.addEventListener("dblclick", (e) => this.onLaneDouble(e));
    this.laneCanvas.addEventListener("pointermove", (e) => { if (!this.gesture) this.laneCanvas.style.cursor = this.lanePointAt(this.local(e, true)) ? "grab" : "crosshair"; });
    this.element.addEventListener("keydown", (e) => this.onKey(e));

    this.resizeObserver = new ResizeObserver(() => this.draw());
    this.resizeObserver.observe(this.view);
    this.resizeObserver.observe(this.laneView);
    this.showLane();
  }

  get beat() { return 60 / (this.session.tempo || 120); }
  get gridSeconds() { return this.grid * this.beat; }

  //============================================================================ head
  buildHead() {
    const head = html("div", "roll-head");
    const title = html("div", "roll-title");
    this.nameLabel = html("span", "roll-clip-name", this.clip.name);
    title.append(html("h3", "", "Notes"), this.nameLabel);

    const gridSelect = html("select", "dropdown");
    gridSelect.title = "Grid: where notes snap (hold Alt while dragging to place freely)";
    for (const [value, text] of GRIDS) gridSelect.append(new Option(text, String(value)));
    gridSelect.value = String(this.grid);
    gridSelect.addEventListener("change", () => { this.grid = savedGrid = Number(gridSelect.value); this.draw(); });

    const tool = (text, title, onClick) => {
      const b = html("button", "tool", text);
      b.type = "button";
      b.title = title;
      b.addEventListener("click", onClick);
      return b;
    };

    this.velocityInput = html("input", "roll-velocity");
    Object.assign(this.velocityInput, { type: "range", min: 1, max: 127, value: this.velocity, title: "Velocity (the selected notes, and new ones)" });
    this.velocityLabel = html("span", "roll-velocity-value", String(this.velocity));
    this.velocityInput.addEventListener("input", () => { this.velocityLabel.textContent = this.velocityInput.value; });
    this.velocityInput.addEventListener("change", () => {
      this.velocity = Number(this.velocityInput.value);
      const change = this.selectedNotes().map((n) => ({ id: n.id, velocity: this.velocity }));
      if (change.length) this.send({ change });
    });
    const velocity = html("label", "roll-velocity-row");
    velocity.append(html("span", "", "Vel"), this.velocityInput, this.velocityLabel);

    this.instrumentSelect = html("select", "dropdown roll-instrument");
    this.instrumentSelect.title = "The track's instrument";
    this.instrumentSelect.addEventListener("change", () => {
      const id = this.instrumentSelect.value;
      if (id) this.daw("addInstrument", { track: this.track.id, instrument: id });
    });
    this.instrumentButton = tool("Open", "Open the instrument's own window", () => this.daw("showInstrument", { track: this.track.id }));
    this.instrumentRow = html("div", "roll-row");
    this.instrumentRow.append(this.instrumentSelect, this.instrumentButton);
    this.loadInstruments();

    const row1 = html("div", "roll-row");
    row1.append(gridSelect, tool("Quantize", "Snap the selected notes (or all) to the grid (Q)", () => this.quantize()));
    const row2 = html("div", "roll-row");
    row2.append(tool("▲", "Up a semitone (↑; Shift+↑ an octave)", () => this.transpose(1)),
                tool("▼", "Down a semitone (↓; Shift+↓ an octave)", () => this.transpose(-1)),
                tool("Delete", "Delete the selected notes (Delete)", () => this.removeSelected()));

    if (this.hasLanes) {
      this.laneSelect = html("select", "dropdown roll-lane-select");
      this.laneSelect.title = "A controller lane under the notes: draw mod wheel, expression, pitch bend...";
      this.laneSelect.addEventListener("change", () => {
        this.laneSelected.clear();
        if (this.laneSelect.value === "other") { this.laneNumber.hidden = false; this.laneNumber.focus(); return; }
        this.lane = savedLane = this.laneSelect.value;
        this.showLane();
      });
      this.laneNumber = html("input", "dropdown roll-lane-number");
      Object.assign(this.laneNumber, { type: "number", min: 0, max: 127, placeholder: "CC", title: "The controller number (0 to 127)" });
      this.laneNumber.hidden = true;
      this.laneNumber.addEventListener("change", () => {
        const number = Math.round(Number(this.laneNumber.value));
        if (!(number >= 0 && number <= 127)) return;
        this.lane = savedLane = `cc:${number}`;
        this.fillLaneSelect();
        this.showLane();
      });
      this.laneRow = html("div", "roll-row");
      this.laneRow.append(this.laneSelect, this.laneNumber);
      this.fillLaneSelect();
      head.append(title, this.instrumentRow, row1, row2, velocity, this.laneRow);
    } else head.append(title, this.instrumentRow, row1, row2, velocity);

    this.hint = html("p", "panel-caption roll-hint", "Click to add notes · drag to move · drag the end to resize · double-click deletes");
    head.append(this.hint);
    this.element.prepend(head);
  }

  // The standard lanes, plus a lane for every other CC the clip has points on.
  fillLaneSelect() {
    if (!this.laneSelect) return;
    const known = new Set(LANES.map((l) => l.id));
    const extra = [...new Set(this.controllers.map(laneId))].filter((id) => !known.has(id)).sort();
    if (!known.has(this.lane) && !extra.includes(this.lane) && this.lane.startsWith("cc:")) extra.push(this.lane);
    this.laneSelect.replaceChildren();
    for (const lane of LANES) this.laneSelect.append(new Option(lane.name, lane.id));
    for (const id of extra) this.laneSelect.append(new Option(`CC ${id.slice(3)}`, id));
    this.laneSelect.append(new Option("Other CC…", "other"));
    this.laneSelect.value = this.lane;
  }

  get laneSpec() {
    if (this.lane === "off") return null;
    return LANES.find((l) => l.id === this.lane) ?? { id: this.lane, kind: "cc", number: Number(this.lane.slice(3)) };
  }

  showLane() {
    if (this.laneSelect) this.laneSelect.value = this.lane;
    if (this.laneNumber) this.laneNumber.hidden = true;
    this.laneView.hidden = this.lane === "off";
    this.laneView.style.flex = `0 0 ${LANE_HEIGHT}px`;
    this.draw();
  }

  async loadInstruments() {
    const result = await this.daw("instruments");
    if (!result?.ok) return;
    this.instruments = result.instruments;
    this.renderInstrument();
  }

  renderInstrument() {
    const current = this.track.instrument ?? {};
    const select = this.instrumentSelect;
    const list = this.instruments ?? [];
    select.replaceChildren();
    if (!list.some((i) => i.id === current.id)) select.append(new Option(current.name || "Instrument", current.id || ""));
    let group = null, groupName = "";
    for (const instrument of list) {
      const name = instrument.kind === "builtin" ? "Built-in" : instrument.kind.toUpperCase();
      if (name !== groupName) {
        group = html("optgroup");
        group.label = groupName = name;
        select.append(group);
      }
      group.append(new Option(instrument.vendor && instrument.kind !== "builtin" ? `${instrument.name} (${instrument.vendor})` : instrument.name, instrument.id));
    }
    select.value = current.id || "";
    this.instrumentButton.hidden = !current.hasWindow;
    this.instrumentRow.classList.toggle("has-error", Boolean(current.error));
    select.title = current.error ? `⚠ ${current.error}` : "The track's instrument";
  }

  //============================================================================ updates
  update(clip, track, session) {
    this.clip = clip;
    this.track = track;
    this.session = session;
    this.nameLabel.textContent = clip.name;
    if (!this.gesture) {
      if (this.hasLanes) {
        this.controllers = (clip.controllers ?? []).map((p) => ({ ...p }));
        const ids = new Set(this.controllers.map((p) => p.id));
        for (const id of [...this.laneSelected]) if (!ids.has(id)) this.laneSelected.delete(id);
        this.fillLaneSelect();
      }
      this.notes = clip.notes.map((n) => ({ ...n }));
      const ids = new Set(this.notes.map((n) => n.id));
      for (const id of [...this.selected]) if (!ids.has(id)) this.selected.delete(id);
    }
    this.renderInstrument();
    this.draw();
  }

  setStatus(status) {
    const held = new Set(status.midiNotes ?? []);
    const playheadMoved = status.playing && Math.abs(status.position - this.position) > 1e-3;
    const heldChanged = held.size !== this.held.size || [...held].some((n) => !this.held.has(n));
    this.position = status.position;
    this.held = held;
    if (playheadMoved || heldChanged) this.draw();
  }

  async send(edits) {
    const result = await this.daw("editNotes", { clip: this.clip.id, ...edits });
    if (result?.ok && Array.isArray(result.added) && result.added.length) {
      this.selected = new Set(result.added);
      this.draw();
    }
    return result;
  }

  selectedNotes() { return this.notes.filter((n) => this.selected.has(n.id)); }

  //============================================================================ geometry
  // Content time <-> x; pitch <-> y.
  xOf(time) { return KEYS + (time - this.clip.offset - this.scrollX) * this.zoom; }
  timeOf(x) { return (x - KEYS) / this.zoom + this.scrollX + this.clip.offset; }
  yOf(pitch) { return (127 - pitch) * ROW - this.scrollY; }
  pitchOf(y) { return clamp(127 - Math.floor((y + this.scrollY) / ROW), 0, 127); }

  // Snaps a content time to the grid, which is laid on the song's beats (not the clip's start).
  snap(time, mode = "round", free = false) {
    if (free) return Math.max(0, time);
    const g = this.gridSeconds;
    const songTime = this.clip.start + time - this.clip.offset;
    const snapped = Math[mode](songTime / g + 1e-6) * g;
    return Math.max(0, snapped - this.clip.start + this.clip.offset);
  }

  fit() {
    const width = Math.max(100, this.view.clientWidth - KEYS);
    this.zoom = clamp(width / Math.max(0.25, this.clip.length), 20, 2000);
    this.scrollX = 0;
    const pitches = this.notes.map((n) => n.pitch);
    const centre = pitches.length ? (Math.min(...pitches) + Math.max(...pitches)) / 2 : 60;
    this.scrollY = clamp((127 - centre) * ROW - this.view.clientHeight / 2, 0, 128 * ROW - this.view.clientHeight);
  }

  hitTest(x, y) {
    for (let i = this.notes.length - 1; i >= 0; --i) {
      const n = this.notes[i];
      const x0 = this.xOf(n.time), x1 = Math.max(x0 + 3, this.xOf(n.time + n.length)), y0 = this.yOf(n.pitch);
      if (x >= x0 && x <= x1 && y >= y0 && y < y0 + ROW)
        return { note: n, edge: x >= x1 - Math.min(EDGE, (x1 - x0) / 3) };
    }
    return null;
  }

  local(event, lane = false) {
    const box = (lane ? this.laneCanvas : this.canvas).getBoundingClientRect();
    return { x: event.clientX - box.left, y: event.clientY - box.top };
  }

  //============================================================================ drawing
  draw() {
    const width = this.view.clientWidth, height = this.view.clientHeight;
    if (width <= 0 || height <= 0) return;
    if (this.zoom == null || this.scrollY == null) this.fit();
    this.scrollY = clamp(this.scrollY, 0, Math.max(0, 128 * ROW - height));

    const ratio = window.devicePixelRatio || 1;
    const canvas = this.canvas;
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
      canvas.width = Math.round(width * ratio);
      canvas.height = Math.round(height * ratio);
      canvas.style.width = `${width}px`;
      canvas.style.height = `${height}px`;
    }
    const c = canvas.getContext("2d");
    c.setTransform(ratio, 0, 0, ratio, 0, 0);
    c.clearRect(0, 0, width, height);

    const clip = this.clip;
    const top = this.pitchOf(0), bottom = this.pitchOf(height);

    // Rows: black-key rows darker, C rows with a line, the song key's notes tinted.
    const songKey = this.session.key?.key ?? -1;
    const scale = this.session.key?.minor ? SCALES.minor : SCALES.major;
    for (let p = bottom; p <= top; ++p) {
      const y = this.yOf(p);
      c.fillStyle = BLACK.has(p % 12) ? "#14151c" : "#1a1c25";
      c.fillRect(KEYS, y, width - KEYS, ROW);
      const degree = songKey >= 0 ? (p - songKey + 120) % 12 : -1;
      if (degree >= 0 && scale.has(degree)) {
        c.fillStyle = degree === 0 ? "rgba(140,123,255,0.16)" : "rgba(140,123,255,0.07)";
        c.fillRect(KEYS, y, width - KEYS, ROW);
      }
      if (p % 12 === 0) { c.fillStyle = "rgba(255,255,255,0.10)"; c.fillRect(KEYS, y + ROW - 1, width - KEYS, 1); }
    }

    // The chord track: under each chord, its notes' rows tinted (the root more); the
    // names go on top once the notes are drawn.
    const chords = this.session.chords ?? [];
    const xAtSong = (songTime) => this.xOf(songTime - clip.start + clip.offset);
    const chordSpans = chords.map((chord, i) => ({
      chord,
      x0: Math.max(KEYS, xAtSong(chord.time)),
      x1: Math.min(width, i + 1 < chords.length ? xAtSong(chords[i + 1].time) : width),
    })).filter(({ x0, x1 }) => x1 > x0);
    for (const { chord, x0, x1 } of chordSpans) {
      if (!chord.chord) continue;
      for (let p = bottom; p <= top; ++p) {
        if (!((chord.mask >> (p % 12)) & 1)) continue;
        c.fillStyle = p % 12 === chord.root ? "rgba(224,180,90,0.26)" : "rgba(224,180,90,0.14)";
        c.fillRect(x0, this.yOf(p), x1 - x0, ROW);
      }
      c.fillStyle = "rgba(224,180,90,0.55)";
      c.fillRect(x0, 0, 1, height);
    }

    // Outside the clip's window: shaded.
    const windowEnd = this.xOf(clip.offset + clip.length);
    if (windowEnd < width) { c.fillStyle = "rgba(0,0,0,0.35)"; c.fillRect(Math.max(KEYS, windowEnd), 0, width, height); }

    // Grid lines, on the song's beats and bars.
    const g = this.gridSeconds, beat = this.beat, bar = beat * (this.session.beatsPerBar || 4);
    const songAt = (x) => clip.start + this.timeOf(x) - clip.offset;
    const first = Math.floor(songAt(KEYS) / g), last = Math.ceil(songAt(width) / g);
    if (last - first < 2000) {
      for (let i = first; i <= last; ++i) {
        const songTime = i * g;
        const x = Math.round(this.xOf(songTime - clip.start + clip.offset)) + 0.5;
        if (x < KEYS) continue;
        const onBar = Math.abs(songTime / bar - Math.round(songTime / bar)) < 1e-6;
        const onBeat = Math.abs(songTime / beat - Math.round(songTime / beat)) < 1e-6;
        c.fillStyle = onBar ? "rgba(255,255,255,0.22)" : onBeat ? "rgba(255,255,255,0.10)" : "rgba(255,255,255,0.04)";
        c.fillRect(x - 0.5, 0, 1, height);
      }
    }

    // Notes.
    for (const n of this.notes) {
      const x0 = this.xOf(n.time), x1 = this.xOf(n.time + n.length), y = this.yOf(n.pitch);
      if (x1 < KEYS || x0 > width || y + ROW < 0 || y > height) continue;
      const inside = n.time >= clip.offset && n.time < clip.offset + clip.length;
      const selected = this.selected.has(n.id);
      c.globalAlpha = (inside ? 0.45 + 0.55 * (n.velocity / 127) : 0.25);
      c.fillStyle = selected ? "#ffffff" : this.colour;
      const w = Math.max(3, x1 - x0);
      c.fillRect(Math.max(KEYS, x0), y + 1, w - Math.max(0, KEYS - x0), ROW - 2);
      c.globalAlpha = 1;
      c.fillStyle = "rgba(0,0,0,0.45)";
      c.fillRect(Math.max(KEYS, x0 + w - 2), y + 1, 2, ROW - 2); // the grab edge
      if (w > 26 && ROW >= 11) {
        c.fillStyle = selected ? "#111" : "rgba(0,0,0,0.7)";
        c.font = "9px system-ui, sans-serif";
        c.fillText(noteName(n.pitch), Math.max(KEYS, x0) + 3, y + ROW - 3);
      }
    }

    // The chord names, over the notes.
    c.font = "700 10px ui-monospace, monospace";
    for (const { chord, x0, x1 } of chordSpans) {
      if (x1 - x0 < 20) continue;
      const w = Math.min(x1 - x0 - 4, c.measureText(chord.name).width + 8);
      c.fillStyle = "rgba(11,13,18,0.85)";
      c.fillRect(x0 + 2, 2, w, 14);
      c.fillStyle = chord.chord ? "#f3d79a" : "#8b90a0";
      c.fillText(chord.name, x0 + 6, 13, w - 8);
    }

    // Box selection.
    if (this.gesture?.type === "box" && this.gesture.moved) {
      const { x0, y0, x1, y1 } = this.gesture;
      c.strokeStyle = "rgba(255,255,255,0.7)";
      c.setLineDash([4, 3]);
      c.strokeRect(Math.min(x0, x1) + 0.5, Math.min(y0, y1) + 0.5, Math.abs(x1 - x0), Math.abs(y1 - y0));
      c.setLineDash([]);
    }

    // Playhead.
    if (this.position >= clip.start && this.position <= clip.start + clip.length) {
      const x = Math.round(this.xOf(this.position - clip.start + clip.offset)) + 0.5;
      if (x >= KEYS) { c.fillStyle = "#ffffff"; c.fillRect(x - 0.5, 0, 1, height); }
    }

    // Keyboard.
    for (let p = bottom; p <= top; ++p) {
      const y = this.yOf(p);
      const black = BLACK.has(p % 12);
      c.fillStyle = this.held.has(p) || this.gesture?.previewPitch === p ? this.colour : black ? "#2a2c36" : "#d9dbe3";
      c.fillRect(0, y, black ? KEYS * 0.62 : KEYS, ROW);
      if (black) { c.fillStyle = "#d9dbe3"; c.fillRect(KEYS * 0.62, y, KEYS * 0.38, ROW); }
      c.fillStyle = "rgba(0,0,0,0.25)";
      c.fillRect(0, y + ROW - 1, KEYS, 1);
      if (p % 12 === 0) {
        c.fillStyle = "#30323c";
        c.font = "9px system-ui, sans-serif";
        c.fillText(noteName(p), KEYS - 22, y + ROW - 3);
      }
    }
    c.fillStyle = "rgba(255,255,255,0.15)";
    c.fillRect(KEYS - 1, 0, 1, height);
    this.drawLane();
  }

  //============================================================================ controller lane
  lanePoints() {
    const spec = this.laneSpec;
    if (!spec) return [];
    return this.controllers.filter((p) => p.kind === spec.kind && (spec.kind !== "cc" || p.number === spec.number))
      .sort((a, b) => a.time - b.time);
  }

  laneY(value, kind, height) {
    const [lo, hi] = laneRange(kind);
    return 6 + (1 - (value - lo) / (hi - lo)) * (height - 12);
  }

  laneValue(y, kind, height) {
    const [lo, hi] = laneRange(kind);
    return Math.round(clamp(lo + (1 - (y - 6) / (height - 12)) * (hi - lo), lo, hi));
  }

  drawLane() {
    const spec = this.laneSpec;
    if (!spec || this.laneView.hidden) return;
    const width = this.laneView.clientWidth, height = this.laneView.clientHeight;
    if (width <= 0 || height <= 0) return;

    const ratio = window.devicePixelRatio || 1;
    const canvas = this.laneCanvas;
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
      canvas.width = Math.round(width * ratio);
      canvas.height = Math.round(height * ratio);
      canvas.style.width = `${width}px`;
      canvas.style.height = `${height}px`;
    }
    const c = canvas.getContext("2d");
    c.setTransform(ratio, 0, 0, ratio, 0, 0);
    c.clearRect(0, 0, width, height);
    const clip = this.clip;
    const [lo, hi] = laneRange(spec.kind);
    const rest = spec.kind === "bend" ? 0 : lo;
    const yRest = this.laneY(rest, spec.kind, height);

    c.fillStyle = "#1a1c25";
    c.fillRect(KEYS, 0, width - KEYS, height);
    const windowEnd = this.xOf(clip.offset + clip.length);
    if (windowEnd < width) { c.fillStyle = "rgba(0,0,0,0.35)"; c.fillRect(Math.max(KEYS, windowEnd), 0, width, height); }

    // Beat and bar lines, as in the roll.
    const beat = this.beat, bar = beat * (this.session.beatsPerBar || 4);
    const songAt = (x) => clip.start + this.timeOf(x) - clip.offset;
    const first = Math.floor(songAt(KEYS) / beat), last = Math.ceil(songAt(width) / beat);
    if (last - first < 2000) {
      for (let i = first; i <= last; ++i) {
        const x = Math.round(this.xOf(i * beat - clip.start + clip.offset)) + 0.5;
        if (x < KEYS) continue;
        const onBar = Math.abs(i * beat / bar - Math.round(i * beat / bar)) < 1e-6;
        c.fillStyle = onBar ? "rgba(255,255,255,0.22)" : "rgba(255,255,255,0.08)";
        c.fillRect(x - 0.5, 0, 1, height);
      }
    }

    // The resting value's line.
    c.fillStyle = "rgba(255,255,255,0.16)";
    c.fillRect(KEYS, Math.round(yRest), width - KEYS, 1);

    // The points as a stepped line with a faint fill; what a stroke is drawing replaces its range.
    let points = this.lanePoints();
    const stroke = this.gesture?.type === "lane-draw" ? this.gesture : null;
    if (stroke?.points.length) {
      const t0 = stroke.points[0].time, t1 = stroke.points[stroke.points.length - 1].time;
      points = points.filter((p) => p.time < Math.min(t0, t1) - 1e-6 || p.time > Math.max(t0, t1) + 1e-6).concat(stroke.points)
        .sort((a, b) => a.time - b.time);
    }
    c.save();
    c.beginPath();
    c.rect(KEYS, 0, width - KEYS, height);
    c.clip();
    if (points.length) {
      c.beginPath();
      let y = this.laneY(points[0].value, spec.kind, height);
      c.moveTo(KEYS, yRest);
      c.lineTo(Math.max(KEYS, this.xOf(points[0].time)), yRest);
      for (const p of points) {
        const x = this.xOf(p.time), py = this.laneY(p.value, spec.kind, height);
        c.lineTo(x, y);
        c.lineTo(x, py);
        y = py;
      }
      c.lineTo(Math.max(this.xOf(clip.offset + clip.length), this.xOf(points[points.length - 1].time)), y);
      c.strokeStyle = this.colour;
      c.lineWidth = 1.5;
      c.stroke();
      c.lineTo(Math.max(this.xOf(clip.offset + clip.length), this.xOf(points[points.length - 1].time)), yRest);
      c.closePath();
      c.globalAlpha = 0.16;
      c.fillStyle = this.colour;
      c.fill();
      c.globalAlpha = 1;
      for (const p of points) {
        const inside = p.time >= clip.offset && p.time < clip.offset + clip.length;
        c.beginPath();
        c.arc(this.xOf(p.time), this.laneY(p.value, spec.kind, height), 3, 0, Math.PI * 2);
        c.fillStyle = this.laneSelected.has(p.id) ? "#ffffff" : this.colour;
        c.globalAlpha = inside ? 1 : 0.4;
        c.fill();
        c.globalAlpha = 1;
      }
    }
    c.restore();

    if (this.position >= clip.start && this.position <= clip.start + clip.length) {
      const x = Math.round(this.xOf(this.position - clip.start + clip.offset)) + 0.5;
      if (x >= KEYS) { c.fillStyle = "#ffffff"; c.fillRect(x - 0.5, 0, 1, height); }
    }

    // The left gutter: the lane's name and its scale.
    c.fillStyle = "#20222c";
    c.fillRect(0, 0, KEYS, height);
    c.fillStyle = "#9aa0b4";
    c.font = "9px system-ui, sans-serif";
    c.fillText(spec.kind === "cc" ? `CC ${spec.number}` : spec.kind === "bend" ? "Bend" : "Press.", 4, 12);
    c.fillText(String(hi), 4, 6 + 18);
    c.fillText(String(rest), 4, Math.min(height - 4, Math.round(yRest) + 3));
    if (spec.kind === "bend") c.fillText(String(lo), 4, height - 4);
    c.fillStyle = "rgba(255,255,255,0.15)";
    c.fillRect(KEYS - 1, 0, 1, height);
  }

  lanePointAt({ x, y }) {
    const spec = this.laneSpec;
    if (!spec || x <= KEYS) return null;
    const height = this.laneView.clientHeight;
    let best = null, bestDistance = 8;
    for (const p of this.lanePoints()) {
      const d = Math.hypot(this.xOf(p.time) - x, this.laneY(p.value, spec.kind, height) - y);
      if (d <= bestDistance) { best = p; bestDistance = d; }
    }
    return best;
  }

  async sendController(edits) {
    const result = await this.daw("editControllers", { clip: this.clip.id, ...edits });
    if (result?.ok && Array.isArray(result.added) && result.added.length === 1) {
      this.laneSelected = new Set(result.added);
      this.draw();
    }
    return result;
  }

  onLaneDown(event) {
    if (event.button !== 0 || !this.laneSpec) return;
    this.element.focus({ preventScroll: true });
    event.preventDefault();
    const spec = this.laneSpec;
    const where = this.local(event, true);
    const height = this.laneView.clientHeight;
    if (where.x <= KEYS) return;
    this.laneCanvas.setPointerCapture(event.pointerId);
    const clip = this.clip;
    const clampTime = (t) => clamp(t, clip.offset, clip.offset + clip.length - 0.001);
    const point = this.lanePointAt(where);

    if (point) {
      // Move a point.
      this.laneSelected = new Set([point.id]);
      const original = { time: point.time, value: point.value };
      this.gesture = { type: "lane-move" };
      this.draw();
      this.trackLane(event, (dx, dy) => {
        point.time = clampTime(original.time + dx / this.zoom);
        point.value = this.laneValue(this.laneY(original.value, spec.kind, height) + dy, spec.kind, height);
        this.draw();
      }, (moved) => {
        if (moved) this.sendController({ change: [{ id: point.id, time: point.time, value: point.value }] });
      });
      return;
    }

    // Draw: points along the stroke (one per few pixels), replacing what was under it.
    const sample = (e) => {
      const at = this.local(e, true);
      return { id: -1, kind: spec.kind, number: spec.number, time: clampTime(this.timeOf(at.x)), value: this.laneValue(at.y, spec.kind, height) };
    };
    const first = sample(event);
    const g = { type: "lane-draw", points: [first] };
    this.gesture = g;
    this.laneSelected.clear();
    this.draw();
    this.trackLane(event, (dx, dy, e) => {
      const next = sample(e);
      const last = g.points[g.points.length - 1];
      if (Math.abs(next.time - last.time) * this.zoom < 5) { if (next.time >= last.time) last.value = next.value; }
      else g.points.push(next);
      this.draw();
    }, () => {
      const times = g.points.map((p) => p.time);
      const t0 = Math.min(...times), t1 = Math.max(...times);
      const remove = g.points.length > 1 ? this.lanePoints().filter((p) => p.time >= t0 - 1e-6 && p.time <= t1 + 1e-6).map((p) => p.id) : [];
      const add = g.points.map(({ kind, number, time, value }) => ({ kind, number, time, value }));
      this.gesture = null;
      this.sendController({ remove, add });
    });
  }

  trackLane(event, move, end) {
    const startX = event.clientX, startY = event.clientY;
    let moved = false;
    const onMove = (e) => {
      const dx = e.clientX - startX, dy = e.clientY - startY;
      if (!moved && Math.abs(dx) + Math.abs(dy) < 3) return;
      moved = true;
      move(dx, dy, e);
    };
    const onUp = () => {
      this.laneCanvas.removeEventListener("pointermove", onMove);
      this.laneCanvas.removeEventListener("pointerup", onUp);
      this.laneCanvas.removeEventListener("pointercancel", onUp);
      end(moved);
      this.gesture = null;
      this.draw();
    };
    this.laneCanvas.addEventListener("pointermove", onMove);
    this.laneCanvas.addEventListener("pointerup", onUp);
    this.laneCanvas.addEventListener("pointercancel", onUp);
  }

  onLaneDouble(event) {
    const point = this.lanePointAt(this.local(event, true));
    if (!point) return;
    this.controllers = this.controllers.filter((p) => p.id !== point.id);
    this.laneSelected.delete(point.id);
    this.draw();
    this.sendController({ remove: [point.id] });
  }

  updateCursor(event) {
    const { x, y } = this.local(event);
    const hit = x > KEYS ? this.hitTest(x, y) : null;
    this.canvas.style.cursor = x <= KEYS ? "pointer" : hit ? (hit.edge ? "ew-resize" : "grab") : "crosshair";
  }

  //============================================================================ gestures
  preview(pitch, on) {
    this.daw("liveNote", { note: pitch, velocity: on ? this.velocity : 0 });
  }

  onPointerDown(event) {
    if (event.button !== 0) return;
    this.element.focus({ preventScroll: true });
    event.preventDefault();
    const { x, y } = this.local(event);
    this.canvas.setPointerCapture(event.pointerId);

    // The keyboard: play the note while held.
    if (x <= KEYS) {
      const pitch = this.pitchOf(y);
      this.gesture = { type: "key", previewPitch: pitch };
      this.preview(pitch, true);
      this.draw();
      this.track_(event, () => {}, () => { this.preview(pitch, false); });
      return;
    }

    const hit = this.hitTest(x, y);

    if (hit) {
      const note = hit.note;
      if (event.shiftKey) {
        if (this.selected.has(note.id)) this.selected.delete(note.id); else this.selected.add(note.id);
      } else if (!this.selected.has(note.id)) {
        this.selected = new Set([note.id]);
      }
      if (this.selected.has(note.id)) {
        this.velocity = note.velocity;
        this.velocityInput.value = String(note.velocity);
        this.velocityLabel.textContent = String(note.velocity);
      }
      const originals = new Map(this.selectedNotes().map((n) => [n.id, { ...n }]));
      const mode = hit.edge ? "resize" : "move";
      this.gesture = { type: mode, previewPitch: note.pitch };
      this.preview(note.pitch, true);
      let lastPitch = note.pitch;
      this.draw();

      this.track_(event, (dx, dy, e) => {
        const free = e.altKey;
        if (mode === "move") {
          const anchor = originals.get(note.id) ?? note;
          const newTime = this.snap(anchor.time + dx / this.zoom, "round", free);
          const dt = newTime - anchor.time;
          const dp = -Math.round(dy / ROW);
          for (const n of this.notes) {
            const o = originals.get(n.id);
            if (!o) continue;
            n.time = Math.max(0, o.time + dt);
            n.pitch = clamp(o.pitch + dp, 0, 127);
          }
          const pitch = clamp(anchor.pitch + dp, 0, 127);
          if (pitch !== lastPitch) { this.preview(lastPitch, false); this.preview(pitch, true); lastPitch = pitch; this.gesture.previewPitch = pitch; }
        } else {
          for (const n of this.notes) {
            const o = originals.get(n.id);
            if (!o) continue;
            const end = this.snap(o.time + o.length + dx / this.zoom, "round", free);
            n.length = Math.max(free ? 0.01 : this.gridSeconds / 4, end - o.time);
          }
        }
        this.draw();
      }, (moved) => {
        this.preview(lastPitch, false);
        if (!moved) return;
        const change = this.selectedNotes().map((n) => ({ id: n.id, pitch: n.pitch, time: n.time, length: n.length }));
        if (mode === "resize" && change.length) this.lastLength = change.find((n) => n.id === note.id)?.length ?? this.lastLength;
        this.send({ change });
      });
      return;
    }

    // Empty space: a box selection, or (a click) a new note.
    if (!event.shiftKey) this.selected.clear();
    const before = new Set(this.selected);
    this.gesture = { type: "box", x0: x, y0: y, x1: x, y1: y, moved: false };
    this.track_(event, (dx, dy) => {
      const g = this.gesture;
      g.x1 = x + dx;
      g.y1 = y + dy;
      g.moved = true;
      const t0 = this.timeOf(Math.min(g.x0, g.x1)), t1 = this.timeOf(Math.max(g.x0, g.x1));
      const p0 = this.pitchOf(Math.max(g.y0, g.y1)), p1 = this.pitchOf(Math.min(g.y0, g.y1));
      this.selected = new Set(before);
      for (const n of this.notes)
        if (n.time < t1 && n.time + n.length > t0 && n.pitch >= p0 && n.pitch <= p1) this.selected.add(n.id);
      this.draw();
    }, (moved) => {
      if (moved) return;
      const pitch = this.pitchOf(y);
      const time = this.snap(this.timeOf(x), "floor", event.altKey);
      const length = this.lastLength ?? this.gridSeconds;
      const note = { id: -1, pitch, time, length, velocity: this.velocity };
      this.notes.push(note);
      this.preview(pitch, true);
      setTimeout(() => this.preview(pitch, false), 200);
      this.send({ add: [{ pitch, time, length, velocity: this.velocity }] });
    });
    this.draw();
  }

  // Follows a drag until release: move(dx, dy, event), end(moved).
  track_(event, move, end) {
    const startX = event.clientX, startY = event.clientY;
    let moved = false;
    const onMove = (e) => {
      const dx = e.clientX - startX, dy = e.clientY - startY;
      if (!moved && Math.abs(dx) + Math.abs(dy) < 3) return;
      moved = true;
      move(dx, dy, e);
    };
    const onUp = () => {
      this.canvas.removeEventListener("pointermove", onMove);
      this.canvas.removeEventListener("pointerup", onUp);
      this.canvas.removeEventListener("pointercancel", onUp);
      end(moved);
      this.gesture = null;
      this.draw();
    };
    this.canvas.addEventListener("pointermove", onMove);
    this.canvas.addEventListener("pointerup", onUp);
    this.canvas.addEventListener("pointercancel", onUp);
  }

  onDoubleClick(event) {
    const { x, y } = this.local(event);
    const hit = x > KEYS ? this.hitTest(x, y) : null;
    if (!hit) return;
    this.notes = this.notes.filter((n) => n.id !== hit.note.id);
    this.selected.delete(hit.note.id);
    this.draw();
    this.send({ remove: [hit.note.id] });
  }

  onWheel(event, lane = false) {
    event.preventDefault();
    if (lane && !event.ctrlKey && !event.metaKey && !event.shiftKey && Math.abs(event.deltaX) <= Math.abs(event.deltaY)) return;
    if (event.ctrlKey || event.metaKey) {
      const { x } = this.local(event, lane);
      const at = this.timeOf(Math.max(KEYS, x));
      this.zoom = clamp(this.zoom * Math.exp(-event.deltaY * 0.0015), 10, 4000);
      this.scrollX = at - this.clip.offset - (Math.max(KEYS, x) - KEYS) / this.zoom;
    } else if (event.shiftKey || Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
      this.scrollX += (event.deltaX || event.deltaY) / this.zoom;
    } else {
      this.scrollY += event.deltaY;
    }
    this.scrollX = clamp(this.scrollX, -1, Math.max(0, this.clip.length));
    this.draw();
  }

  //============================================================================ edits
  removeSelected() {
    const remove = [...this.selected];
    if (!remove.length) return this.toast("Select some notes first.");
    this.notes = this.notes.filter((n) => !this.selected.has(n.id));
    this.selected.clear();
    this.draw();
    this.send({ remove });
  }

  transpose(semitones) {
    const notes = this.selectedNotes();
    if (!notes.length) return this.toast("Select some notes first.");
    this.send({ change: notes.map((n) => ({ id: n.id, pitch: clamp(n.pitch + semitones, 0, 127) })) });
  }

  nudge(steps) {
    const notes = this.selectedNotes();
    if (!notes.length) return;
    this.send({ change: notes.map((n) => ({ id: n.id, time: Math.max(0, n.time + steps * this.gridSeconds) })) });
  }

  quantize() {
    const notes = this.selected.size ? this.selectedNotes() : this.notes;
    if (!notes.length) return this.toast("There are no notes to quantize.");
    this.send({ change: notes.map((n) => {
      const time = this.snap(n.time);
      const end = this.snap(n.time + n.length);
      return { id: n.id, time, length: Math.max(this.gridSeconds, end - time) };
    }) });
    this.toast(`Quantized ${notes.length} note${notes.length === 1 ? "" : "s"} to ${GRIDS.find(([v]) => v === this.grid)?.[1] ?? "the grid"}.`);
  }

  duplicateSelected() {
    const notes = this.selectedNotes();
    if (!notes.length) return;
    const start = Math.min(...notes.map((n) => n.time));
    const end = Math.max(...notes.map((n) => n.time + n.length));
    const shift = Math.max(this.gridSeconds, this.snap(end, "ceil") - this.snap(start, "floor"));
    this.send({ add: notes.map((n) => ({ pitch: n.pitch, time: n.time + shift, length: n.length, velocity: n.velocity })) });
  }

  copySelected(cut = false) {
    const notes = this.selectedNotes();
    if (!notes.length) return;
    const first = Math.min(...notes.map((n) => n.time));
    noteClipboard = notes.map((n) => ({ pitch: n.pitch, time: n.time - first, length: n.length, velocity: n.velocity }));
    if (cut) this.removeSelected();
    this.toast(`${cut ? "Cut" : "Copied"} ${notes.length} note${notes.length === 1 ? "" : "s"}.`);
  }

  // At the playhead when it's inside the clip, else right after the selection (or the start).
  paste() {
    if (!noteClipboard?.length) return this.toast("Copy some notes first (Ctrl+C).");
    const clip = this.clip;
    const playhead = this.position - clip.start + clip.offset;
    let at;
    if (this.position >= clip.start && playhead < clip.offset + clip.length) at = this.snap(playhead);
    else {
      const selected = this.selectedNotes();
      at = selected.length ? this.snap(Math.max(...selected.map((n) => n.time + n.length)), "ceil") : clip.offset;
    }
    this.send({ add: noteClipboard.map((n) => ({ ...n, time: at + n.time })) });
  }

  onKey(event) {
    if (event.target !== this.element) return;
    const mod = event.ctrlKey || event.metaKey;
    const key = event.key;
    const some = this.selected.size > 0; // without a selection, the screen's own keys apply
    let handled = true;

    if (!some && this.laneSelected.size && (key === "Delete" || key === "Backspace")) {
      const remove = [...this.laneSelected];
      this.controllers = this.controllers.filter((p) => !this.laneSelected.has(p.id));
      this.laneSelected.clear();
      this.draw();
      this.sendController({ remove });
    }
    else if (some && (key === "Delete" || key === "Backspace")) this.removeSelected();
    else if (some && key === "ArrowUp") this.transpose(event.shiftKey ? 12 : 1);
    else if (some && key === "ArrowDown") this.transpose(event.shiftKey ? -12 : -1);
    else if (some && key === "ArrowLeft") this.nudge(-1);
    else if (some && key === "ArrowRight") this.nudge(1);
    else if (mod && key.toLowerCase() === "a") { this.selected = new Set(this.notes.map((n) => n.id)); this.draw(); }
    else if (mod && key.toLowerCase() === "d") this.duplicateSelected();
    else if (mod && some && key.toLowerCase() === "c") this.copySelected();
    else if (mod && some && key.toLowerCase() === "x") this.copySelected(true);
    else if (mod && noteClipboard && key.toLowerCase() === "v") this.paste();
    else if (!mod && key.toLowerCase() === "q") this.quantize();
    else if (key === "Escape") { this.selected.clear(); this.draw(); }
    else handled = false;

    if (handled) {
      event.preventDefault();
      event.stopPropagation();
    }
  }

  dispose() {
    this.resizeObserver.disconnect();
  }
}
