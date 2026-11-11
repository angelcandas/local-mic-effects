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

let savedGrid = 0.25;

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

    this.view = html("div", "roll-view");
    this.canvas = html("canvas", "roll-canvas");
    this.view.append(this.canvas);
    this.element.append(this.view);

    this.zoom = null;     // px per second; fitted on first layout
    this.scrollX = 0;     // seconds into the clip's window
    this.scrollY = null;  // px from the top (pitch 127); centred on the notes on first layout

    this.canvas.addEventListener("pointerdown", (e) => this.onPointerDown(e));
    this.canvas.addEventListener("dblclick", (e) => this.onDoubleClick(e));
    this.canvas.addEventListener("pointermove", (e) => { if (!this.gesture) this.updateCursor(e); });
    this.view.addEventListener("wheel", (e) => this.onWheel(e), { passive: false });
    this.element.addEventListener("keydown", (e) => this.onKey(e));

    this.resizeObserver = new ResizeObserver(() => this.draw());
    this.resizeObserver.observe(this.view);
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

    this.hint = html("p", "panel-caption roll-hint", "Click to add notes · drag to move · drag the end to resize · double-click deletes");
    head.append(title, this.instrumentRow, row1, row2, velocity, this.hint);
    this.element.append(head);
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

  local(event) {
    const box = this.canvas.getBoundingClientRect();
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

    // Rows: black-key rows darker, C rows with a line.
    for (let p = bottom; p <= top; ++p) {
      const y = this.yOf(p);
      c.fillStyle = BLACK.has(p % 12) ? "#14151c" : "#1a1c25";
      c.fillRect(KEYS, y, width - KEYS, ROW);
      if (p % 12 === 0) { c.fillStyle = "rgba(255,255,255,0.10)"; c.fillRect(KEYS, y + ROW - 1, width - KEYS, 1); }
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

  onWheel(event) {
    event.preventDefault();
    if (event.ctrlKey || event.metaKey) {
      const { x } = this.local(event);
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

  onKey(event) {
    if (event.target !== this.element) return;
    const mod = event.ctrlKey || event.metaKey;
    const key = event.key;
    const some = this.selected.size > 0; // without a selection, the screen's own keys apply
    let handled = true;

    if (some && (key === "Delete" || key === "Backspace")) this.removeSelected();
    else if (some && key === "ArrowUp") this.transpose(event.shiftKey ? 12 : 1);
    else if (some && key === "ArrowDown") this.transpose(event.shiftKey ? -12 : -1);
    else if (some && key === "ArrowLeft") this.nudge(-1);
    else if (some && key === "ArrowRight") this.nudge(1);
    else if (mod && key.toLowerCase() === "a") { this.selected = new Set(this.notes.map((n) => n.id)); this.draw(); }
    else if (mod && key.toLowerCase() === "d") this.duplicateSelected();
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
