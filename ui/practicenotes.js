// The melody editor (Practice): fix the notes you're meant to sing.
//
// The melody is found in the song's voice, so a chorus with backing singers, a harmony or a
// wrong octave gives wrong notes. This is a piano-roll over the whole song to put them right:
//
//   drag a note             move it (time and pitch); drag its ends to resize
//   drag on empty space     select with a box;  Shift+click adds or removes
//   double-click empty      a new note
//   Delete                  delete the selection;  P selects the whole phrase around it
//   S split at the playhead (or in the middle);  M merges the selected notes
//   ↑ ↓                     a semitone (Shift: an octave);  ← →  10 ms (Shift: 100 ms)
//   Ctrl+Z / Ctrl+Shift+Z   undo / redo;  Space  play and stop
//
// Changes are sent to the engine ("setMelody") a moment after the last one and kept with the
// song; "Back to detected" brings the notes found in the voice back. Scores start over.
import { clamp, html } from "./widgets.js";

const HEADER = 18;       // px: the strip with the seconds, click to move the playhead
const EDGE = 6;          // px: grab a note's end to resize it
const MIN_LENGTH = 0.04; // s
const PHRASE_GAP = 0.45; // s: the gap between phrases (as the engine finds them)

export class NoteEditor {
  // `host`: { practice, transport, section, noteName, now(), isPlaying(), goTo(seconds), getSong(), isNeutral() }
  constructor(host) {
    this.host = host;
    this.open = false;
    this.notes = [];          // [{ start, end, pitch }]
    this.selected = new Set();
    this.undo = [];
    this.redo = [];
    this.view = { from: 0, pps: 80 };
    this.range = { low: 48, high: 84 };
    this.drag = null;
    this.saveTimer = 0;
    this.edited = false;
    this.build();

    const frame = () => {
      if (this.open && !document.hidden) this.draw();
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
    document.addEventListener("keydown", (event) => this.onKey(event), true);
  }

  build() {
    this.root = html("div", "practice-notes");
    this.root.hidden = true;

    const head = html("div", "practice-notes-head");
    this.title = html("strong", "", "Edit the notes");
    const tool = (label, title, run) => {
      const b = Object.assign(html("button", "tool", label), { type: "button", title });
      b.addEventListener("click", run);
      return b;
    };
    this.undoButton = tool("Undo", "Ctrl+Z", () => this.undoStep());
    this.redoButton = tool("Redo", "Ctrl+Shift+Z", () => this.redoStep());
    this.splitButton = tool("Split", "Cut the selected note at the playhead (S)", () => this.split());
    this.mergeButton = tool("Merge", "Join the selected notes into one (M)", () => this.merge());
    this.upButton = tool("Octave ↑", "Shift+↑", () => this.transpose(12));
    this.downButton = tool("Octave ↓", "Shift+↓", () => this.transpose(-12));
    this.phraseButton = tool("Select phrase", "Everything sung in one breath around the selection (P)", () => this.selectPhrase());
    this.deleteButton = tool("Delete", "Delete (Delete key)", () => this.remove());
    this.hideButton = tool("Not the lead: delete phrase", "The phrase is the backing singers: take its notes out", () => { this.selectPhrase(); this.remove(); });
    this.playButton = tool("Play", "Play and stop (Space)", () => this.togglePlay());
    this.resetButton = Object.assign(html("button", "text-button", "Back to detected"), { type: "button", title: "Throw the edits away and use the notes found in the voice" });
    this.resetButton.addEventListener("click", () => this.reset());
    const done = Object.assign(html("button", "tool is-accent", "Done"), { type: "button" });
    done.addEventListener("click", () => this.show(false));
    this.status = html("span", "practice-notes-status");
    head.append(this.title, this.undoButton, this.redoButton, this.splitButton, this.mergeButton, this.upButton, this.downButton,
                this.phraseButton, this.deleteButton, this.hideButton, this.playButton, this.resetButton, this.status, done);

    this.stage = html("div", "practice-notes-stage");
    this.canvas = document.createElement("canvas");
    this.stage.append(this.canvas);
    this.overview = html("div", "practice-review-overview");
    this.overviewCanvas = document.createElement("canvas");
    this.overview.append(this.overviewCanvas);
    this.hint = html("p", "practice-notes-hint",
                     "Drag notes to move them, drag their ends to resize, double-click empty space for a new note. Delete takes the wrong ones out (a backing singer's phrase: Select phrase, Delete).");
    this.root.append(head, this.stage, this.overview, this.hint);
    this.host.section.append(this.root);

    new ResizeObserver(() => this.draw()).observe(this.stage);
    this.stage.addEventListener("pointerdown", (event) => this.onDown(event));
    this.stage.addEventListener("pointermove", (event) => this.onMove(event));
    this.stage.addEventListener("pointerup", (event) => this.onUp(event));
    this.stage.addEventListener("pointercancel", (event) => this.onUp(event));
    this.stage.addEventListener("dblclick", (event) => this.onDouble(event));
    this.stage.addEventListener("wheel", (event) => {
      event.preventDefault();
      if (event.ctrlKey || event.metaKey) this.zoomTo(this.view.pps * Math.exp(-event.deltaY * 0.0025), event.clientX - this.stage.getBoundingClientRect().left);
      else if (event.shiftKey) this.range = this.shiftRange(event.deltaY > 0 ? -2 : 2);
      else this.view.from = this.clampFrom(this.view.from + (Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY) / this.view.pps);
    }, { passive: false });

    const moveOverview = (event) => {
      const box = this.overview.getBoundingClientRect();
      this.view.from = this.clampFrom(((event.clientX - box.left) / box.width) * this.length() - this.visibleSeconds() / 2);
    };
    this.overview.addEventListener("pointerdown", (event) => { this.overview.setPointerCapture(event.pointerId); this.overviewDrag = true; moveOverview(event); });
    this.overview.addEventListener("pointermove", (event) => { if (this.overviewDrag) moveOverview(event); });
    this.overview.addEventListener("pointerup", () => { this.overviewDrag = false; });
  }

  //==============================================================================
  show(on) {
    if (on === this.open) return;
    if (on) {
      const song = this.host.getSong();
      if (!song) return this.host.say("Open a song first.", true);
      if (!this.host.isNeutral()) return this.host.say("Put the key and speed back to the original (and leave any loop) to edit the notes.", true);
      this.notes = song.notes.map((n) => ({ start: n.start, end: n.end, pitch: n.pitch }));
      this.edited = Boolean(song.edited);
      this.selected.clear();
      this.undo = [];
      this.redo = [];
      this.range = this.computeRange();
      this.view.from = this.clampFrom(Math.max(0, (this.notes[0]?.start ?? 0) - 1));
    } else {
      this.flush();
    }
    this.open = on;
    this.root.hidden = !on;
    this.host.section.classList.toggle("is-editing-notes", on);
    this.refresh();
  }

  length() { return Math.max(10, (this.notes[this.notes.length - 1]?.end ?? 10) + 2); }
  visibleSeconds() { return Math.max(1, this.stage.clientWidth) / this.view.pps; }
  clampFrom(from) { return clamp(from, -0.5, Math.max(0, this.length() - this.visibleSeconds() + 1)); }
  zoomTo(pps, anchorPx = this.stage.clientWidth / 2) {
    const at = this.view.from + anchorPx / this.view.pps;
    this.view.pps = clamp(pps, 20, 400);
    this.view.from = this.clampFrom(at - anchorPx / this.view.pps);
  }
  computeRange() {
    const pitches = this.notes.map((n) => n.pitch);
    const low = (pitches.length ? Math.min(...pitches) : 55) - 5, high = (pitches.length ? Math.max(...pitches) : 79) + 5;
    return { low: Math.floor(Math.min(low, high - 24)), high: Math.ceil(high) };
  }
  shiftRange(by) { return { low: this.range.low + by, high: this.range.high + by }; }

  //==============================================================================
  // Geometry.
  rowHeight() { return (this.stage.clientHeight - HEADER) / (this.range.high - this.range.low + 1); }
  xOf(t) { return (t - this.view.from) * this.view.pps; }
  yOf(pitch) { return HEADER + (this.range.high - pitch) * this.rowHeight(); }
  timeAt(x) { return this.view.from + x / this.view.pps; }
  pitchAt(y) { return this.range.high - (y - HEADER) / this.rowHeight(); }

  hit(x, y) {
    // The note under a point, and which part of it: "start", "end" or "body".
    const rh = this.rowHeight();
    for (let i = this.notes.length - 1; i >= 0; --i) {
      const n = this.notes[i];
      const left = this.xOf(n.start), right = this.xOf(n.end), mid = this.yOf(n.pitch) + rh / 2;
      if (x < left - 2 || x > right + 2 || Math.abs(y - mid) > Math.max(rh / 2, 7)) continue;
      const edge = Math.min(EDGE, (right - left) / 3);
      return { note: n, part: x <= left + edge ? "start" : x >= right - edge ? "end" : "body" };
    }
    return null;
  }

  local(event) {
    const box = this.stage.getBoundingClientRect();
    return { x: event.clientX - box.left, y: event.clientY - box.top };
  }

  //==============================================================================
  // Editing: every change keeps what it replaced for undo and goes to the engine soon after.
  snapshot() { return this.notes.map((n) => ({ ...n })); }

  begin() {
    this.undo.push({ notes: this.snapshot(), selected: this.notes.map((n, i) => (this.selected.has(n) ? i : -1)).filter((i) => i >= 0) });
    if (this.undo.length > 200) this.undo.shift();
    this.redo = [];
  }

  changed() {
    this.notes.sort((a, b) => a.start - b.start);
    this.refresh();
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => this.flush(), 700);
    this.dirty = true;
  }

  async flush() {
    clearTimeout(this.saveTimer);
    if (!this.dirty) return;
    this.dirty = false;
    if (!this.notes.length) return;
    const flat = this.notes.flatMap((n) => [Math.round(n.start * 1000) / 1000, Math.round(n.end * 1000) / 1000, Math.round(n.pitch * 100) / 100]);
    const result = await this.host.practice("setMelody", { notes: flat });
    if (result?.ok === false) this.host.say(result.error, true);
    this.edited = true;
    this.refresh();
  }

  restore(entry) {
    this.notes = entry.notes.map((n) => ({ ...n }));
    this.selected = new Set(entry.selected.map((i) => this.notes[i]).filter(Boolean));
  }

  undoStep() {
    const entry = this.undo.pop();
    if (!entry) return;
    this.redo.push({ notes: this.snapshot(), selected: this.notes.map((n, i) => (this.selected.has(n) ? i : -1)).filter((i) => i >= 0) });
    this.restore(entry);
    this.changed();
  }

  redoStep() {
    const entry = this.redo.pop();
    if (!entry) return;
    this.undo.push({ notes: this.snapshot(), selected: this.notes.map((n, i) => (this.selected.has(n) ? i : -1)).filter((i) => i >= 0) });
    this.restore(entry);
    this.changed();
  }

  remove() {
    if (!this.selected.size) return;
    if (this.selected.size >= this.notes.length) return this.host.say("The song needs at least one note to sing.", true);
    this.begin();
    this.notes = this.notes.filter((n) => !this.selected.has(n));
    this.selected.clear();
    this.changed();
  }

  transpose(semitones) {
    if (!this.selected.size) return;
    this.begin();
    for (const n of this.selected) n.pitch = clamp(n.pitch + semitones, 20, 110);
    this.changed();
  }

  nudge(seconds) {
    if (!this.selected.size) return;
    this.begin();
    for (const n of this.selected) { n.start = Math.max(0, n.start + seconds); n.end = Math.max(n.start + MIN_LENGTH, n.end + seconds); }
    this.changed();
  }

  // The selected note cut in two: at the playhead when it's inside, else in the middle.
  split() {
    const targets = [...this.selected];
    if (!targets.length) return;
    this.begin();
    const playhead = this.host.now();
    const added = [];
    for (const n of targets) {
      const at = playhead > n.start + MIN_LENGTH && playhead < n.end - MIN_LENGTH ? playhead : (n.start + n.end) / 2;
      if (n.end - n.start < MIN_LENGTH * 3) continue;
      const second = { start: at + 0.01, end: n.end, pitch: n.pitch };
      n.end = at;
      this.notes.push(second);
      added.push(second);
    }
    this.selected = new Set([...targets, ...added]);
    this.changed();
  }

  // The selected notes as one: from the first start to the last end, at the pitch of the longest.
  merge() {
    const picked = [...this.selected].sort((a, b) => a.start - b.start);
    if (picked.length < 2) return;
    this.begin();
    const longest = picked.reduce((a, b) => (b.end - b.start > a.end - a.start ? b : a));
    const merged = { start: picked[0].start, end: picked[picked.length - 1].end, pitch: longest.pitch };
    this.notes = this.notes.filter((n) => !this.selected.has(n));
    this.notes.push(merged);
    this.selected = new Set([merged]);
    this.changed();
  }

  // Everything sung in one breath around the selection.
  selectPhrase() {
    if (!this.selected.size) return;
    const sorted = [...this.notes].sort((a, b) => a.start - b.start);
    const groups = new Set(this.selected);
    for (const seed of [...this.selected]) {
      let i = sorted.indexOf(seed);
      for (let j = i; j > 0 && sorted[j].start - sorted[j - 1].end < PHRASE_GAP; --j) groups.add(sorted[j - 1]);
      for (let j = i; j + 1 < sorted.length && sorted[j + 1].start - sorted[j].end < PHRASE_GAP; ++j) groups.add(sorted[j + 1]);
    }
    this.selected = groups;
    this.refresh();
  }

  async reset() {
    if (!this.edited && !this.dirty) return;
    clearTimeout(this.saveTimer);
    this.dirty = false;
    await this.host.practice("resetMelody");
    const song = this.host.getSong();
    this.notes = (song?.notes ?? []).map((n) => ({ start: n.start, end: n.end, pitch: n.pitch }));
    this.selected.clear();
    this.undo = [];
    this.redo = [];
    this.edited = false;
    this.range = this.computeRange();
    this.refresh();
  }

  togglePlay() {
    if (this.host.isPlaying()) this.host.transport("pause");
    else this.host.transport("play");
  }

  //==============================================================================
  onDown(event) {
    if (event.button !== 0) return;
    this.stage.setPointerCapture(event.pointerId);
    const { x, y } = this.local(event);
    if (y < HEADER) {
      this.host.goTo(Math.max(0, this.timeAt(x)));
      this.drag = { kind: "seek" };
      return;
    }
    const found = this.hit(x, y);
    if (!found) {
      if (!event.shiftKey) this.selected.clear();
      this.drag = { kind: "box", x0: x, y0: y, x1: x, y1: y, base: new Set(this.selected) };
      this.refresh();
      return;
    }
    const { note, part } = found;
    if (event.shiftKey) {
      if (this.selected.has(note)) this.selected.delete(note); else this.selected.add(note);
    } else if (!this.selected.has(note)) {
      this.selected = new Set([note]);
    }
    this.refresh();
    if (!this.selected.has(note)) return;
    this.drag = {
      kind: part, x0: x, y0: y, began: false,
      originals: new Map([...this.selected].map((n) => [n, { ...n }])),
    };
  }

  onMove(event) {
    const d = this.drag;
    const { x, y } = this.local(event);
    if (!d) {
      const found = y >= HEADER ? this.hit(x, y) : null;
      this.stage.style.cursor = !found ? "default" : found.part === "body" ? "grab" : "ew-resize";
      return;
    }
    if (d.kind === "seek") return this.host.goTo(Math.max(0, this.timeAt(x)));
    if (d.kind === "box") {
      d.x1 = x; d.y1 = y;
      const [l, r] = [Math.min(d.x0, d.x1), Math.max(d.x0, d.x1)], [t, b] = [Math.min(d.y0, d.y1), Math.max(d.y0, d.y1)];
      this.selected = new Set(d.base);
      for (const n of this.notes) {
        const ny = this.yOf(n.pitch) + this.rowHeight() / 2;
        if (this.xOf(n.end) >= l && this.xOf(n.start) <= r && ny >= t && ny <= b) this.selected.add(n);
      }
      return;
    }
    if (!d.began && Math.abs(x - d.x0) < 3 && Math.abs(y - d.y0) < 3) return;
    if (!d.began) {
      d.began = true;
      this.begin(); // (nothing has moved yet: the undo step holds the notes as they were)
    }
    const dt = (x - d.x0) / this.view.pps;
    const dp = Math.round((d.y0 - y) / this.rowHeight());
    for (const [n, o] of d.originals) {
      if (d.kind === "body") {
        n.start = Math.max(0, o.start + dt);
        n.end = n.start + (o.end - o.start);
        n.pitch = clamp(o.pitch + dp, 20, 110);
      } else if (d.kind === "start") {
        n.start = clamp(o.start + dt, 0, o.end - MIN_LENGTH);
      } else {
        n.end = Math.max(o.start + MIN_LENGTH, o.end + dt);
      }
    }
  }

  onUp() {
    const d = this.drag;
    this.drag = null;
    if (d?.began) this.changed();
    else this.refresh();
  }

  onDouble(event) {
    const { x, y } = this.local(event);
    if (y < HEADER || this.hit(x, y)) return;
    this.begin();
    const start = Math.max(0, Math.round(this.timeAt(x) * 20) / 20);
    const note = { start, end: start + 0.5, pitch: Math.round(this.pitchAt(y) - 0.5) };
    this.notes.push(note);
    this.selected = new Set([note]);
    this.changed();
  }

  onKey(event) {
    if (!this.open || /^(INPUT|SELECT|TEXTAREA)$/.test(event.target?.tagName ?? "")) return;
    const key = event.key;
    const mod = event.ctrlKey || event.metaKey;
    let handled = true;
    if (mod && key.toLowerCase() === "z") event.shiftKey ? this.redoStep() : this.undoStep();
    else if (mod && key.toLowerCase() === "y") this.redoStep();
    else if (mod && key.toLowerCase() === "a") { this.selected = new Set(this.notes); this.refresh(); }
    else if (mod) handled = false;
    else if (key === "Delete" || key === "Backspace") this.remove();
    else if (key === "ArrowUp") this.transpose(event.shiftKey ? 12 : 1);
    else if (key === "ArrowDown") this.transpose(event.shiftKey ? -12 : -1);
    else if (key === "ArrowLeft") this.nudge(event.shiftKey ? -0.1 : -0.01);
    else if (key === "ArrowRight") this.nudge(event.shiftKey ? 0.1 : 0.01);
    else if (key.toLowerCase() === "s") this.split();
    else if (key.toLowerCase() === "m") this.merge();
    else if (key.toLowerCase() === "p") this.selectPhrase();
    else if (key === " ") this.togglePlay();
    else if (key === "Escape") this.show(false);
    else handled = false;
    if (handled) { event.preventDefault(); event.stopPropagation(); }
  }

  refresh() {
    const n = this.selected.size;
    this.undoButton.disabled = !this.undo.length;
    this.redoButton.disabled = !this.redo.length;
    for (const b of [this.deleteButton, this.splitButton, this.upButton, this.downButton, this.phraseButton, this.hideButton]) b.disabled = n === 0;
    this.mergeButton.disabled = n < 2;
    this.resetButton.disabled = !this.edited && !this.dirty;
    this.playButton.textContent = this.host.isPlaying() ? "Stop" : "Play";
    this.status.textContent = `${this.notes.length} notes${n ? ` · ${n} selected` : ""}${this.edited ? " · edited" : ""}`;
  }

  //==============================================================================
  draw() {
    const ratio = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(this.stage.clientWidth * ratio)), h = Math.max(1, Math.round(this.stage.clientHeight * ratio));
    if (this.canvas.width !== w) this.canvas.width = w;
    if (this.canvas.height !== h) this.canvas.height = h;
    const g = this.canvas.getContext("2d");
    g.setTransform(ratio, 0, 0, ratio, 0, 0);
    const width = this.stage.clientWidth, height = this.stage.clientHeight;
    g.clearRect(0, 0, width, height);

    const css = getComputedStyle(this.host.section);
    const color = (name, fallback) => css.getPropertyValue(name).trim() || fallback;
    const line = color("--line", "#242a37"), faint = color("--faint", "#566077"), target = color("--target", "#8c7bff"), good = color("--good", "#3fd9c6");
    const rh = this.rowHeight();

    // Follow the song while it plays.
    const now = this.host.now();
    if (this.host.isPlaying() && !this.drag) {
      const px = this.xOf(now);
      if (px > width * 0.85 || px < 0) this.view.from = this.clampFrom(now - this.visibleSeconds() * 0.25);
    }

    // Rows, with the note names at the C's.
    g.font = "10px ui-monospace, Menlo, monospace";
    g.textBaseline = "middle";
    for (let m = this.range.low; m <= this.range.high; ++m) {
      const pc = ((m % 12) + 12) % 12, y = this.yOf(m);
      if ([1, 3, 6, 8, 10].includes(pc)) { g.fillStyle = "rgba(255,255,255,0.03)"; g.fillRect(0, y, width, rh); }
      if (pc === 0) { g.fillStyle = line; g.fillRect(0, y + rh, width, 1); g.fillStyle = faint; g.fillText(this.host.noteName(m), 6, y + rh / 2); }
    }

    // The seconds ruler.
    g.fillStyle = "rgba(255,255,255,0.04)";
    g.fillRect(0, 0, width, HEADER);
    const step = this.view.pps >= 160 ? 1 : this.view.pps >= 60 ? 5 : 10;
    g.fillStyle = faint;
    for (let t = Math.ceil(this.view.from / step) * step; this.xOf(t) < width; t += step) {
      const x = this.xOf(t);
      g.fillRect(x, HEADER - 5, 1, 5);
      g.fillText(`${Math.floor(t / 60)}:${String(Math.floor(t % 60)).padStart(2, "0")}`, x + 3, HEADER / 2);
    }

    // The notes.
    for (const n of this.notes) {
      const left = this.xOf(n.start), right = this.xOf(n.end);
      if (right < 0 || left > width) continue;
      const selected = this.selected.has(n);
      g.fillStyle = selected ? good : target;
      g.globalAlpha = selected ? 0.95 : 0.7;
      const y = this.yOf(n.pitch) + rh * 0.14;
      g.beginPath();
      g.roundRect(left, y, Math.max(3, right - left - 1), Math.max(4, rh * 0.72), 4);
      g.fill();
      if (selected) { g.globalAlpha = 1; g.strokeStyle = "#fff"; g.lineWidth = 1.2; g.stroke(); }
      g.globalAlpha = 1;
      if (right - left > 34 && rh > 9) {
        g.fillStyle = selected ? "#0b0d12" : "rgba(255,255,255,0.8)";
        g.fillText(this.host.noteName(n.pitch), left + 5, y + Math.max(4, rh * 0.72) / 2);
      }
    }

    // The box being drawn, and the playhead.
    const d = this.drag;
    if (d?.kind === "box") {
      g.fillStyle = "rgba(140,123,255,0.15)";
      g.strokeStyle = target;
      g.fillRect(Math.min(d.x0, d.x1), Math.min(d.y0, d.y1), Math.abs(d.x1 - d.x0), Math.abs(d.y1 - d.y0));
      g.strokeRect(Math.min(d.x0, d.x1), Math.min(d.y0, d.y1), Math.abs(d.x1 - d.x0), Math.abs(d.y1 - d.y0));
    }
    const px = this.xOf(now);
    if (px >= 0 && px <= width) { g.fillStyle = "rgba(255,255,255,0.6)"; g.fillRect(Math.round(px), 0, 1.5, height); }
    this.drawOverview(now);
    this.playButton.textContent = this.host.isPlaying() ? "Stop" : "Play";
  }

  drawOverview(now) {
    const ratio = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(this.overview.clientWidth * ratio)), h = Math.max(1, Math.round(this.overview.clientHeight * ratio));
    if (this.overviewCanvas.width !== w) this.overviewCanvas.width = w;
    if (this.overviewCanvas.height !== h) this.overviewCanvas.height = h;
    const g = this.overviewCanvas.getContext("2d");
    g.clearRect(0, 0, w, h);
    const total = this.length();
    const span = Math.max(12, this.range.high - this.range.low);
    g.fillStyle = "rgba(140,123,255,0.9)";
    for (const n of this.notes) g.fillRect((n.start / total) * w, h - ((n.pitch - this.range.low) / span) * h - 2, Math.max(1.5, ((n.end - n.start) / total) * w), 3 * ratio);
    g.strokeStyle = "rgba(255,255,255,0.8)";
    g.lineWidth = 1.5 * ratio;
    g.strokeRect((this.view.from / total) * w + ratio, ratio, Math.max(4, (this.visibleSeconds() / total) * w), h - 2 * ratio);
    g.fillStyle = "#fff";
    g.fillRect((now / total) * w, 0, ratio, h);
  }
}
