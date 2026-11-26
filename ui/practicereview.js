// Take review (Practice): after you've recorded a take, see what you sang over the song's notes.
//
// The engine (Practice::startReview) listens to the recorded take again and sends its pitch every 20 ms,
// in the song's time, with the score of every note and phrase. The page lays your pitch over the notes
// on a lane you scroll through the whole song: the line is coloured by where it sat (flat, in tune,
// sharp), each phrase shows its score and what was off, and "Where to focus" lists the weakest phrases
// to hear again or practise. Events: "practice" { state: "review", review, error }.
import { clamp, html } from "./widgets.js";

const IN_TUNE = 25;  // cents: green
const CLOSE = 60;    // cents: amber
const LANE_PHRASES = 22; // px: the strip of phrase scores along the top of the lane

const mmss = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, "0")}`;

// What was off in a phrase, in words.
export function phraseIssues(p) {
  const what = [];
  if (!p) return what;
  if (p.coverage < 0.6) what.push("parts not sung");
  if (Math.abs(p.lean) >= 15) what.push(`${p.lean > 0 ? "sharp" : "flat"} ${Math.round(Math.abs(p.lean))}¢`);
  if (Math.abs(p.timing) >= 0.08) what.push(`${p.timing > 0 ? "late" : "early"} ${Math.round(Math.abs(p.timing) * 1000)} ms`);
  if (p.steady < 0.6 && p.coverage >= 0.6) what.push("unsteady");
  return what;
}

export class TakeReview {
  // `host`: { practice, section, noteName, foldToOctave, scoreClass, now(), goTo(seconds), loopPhrase(i), isPlaying() }
  constructor(host) {
    this.host = host;
    this.takes = [];
    this.data = null;       // the review being shown
    this.takeId = -1;
    this.open = false;
    this.view = { from: 0, pps: 70 };
    this.selected = -1;
    this.drag = null;
    this.build();

    const frame = () => {
      if (this.open && !document.hidden) this.draw();
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  build() {
    this.root = html("div", "practice-review");
    this.root.hidden = true;

    const head = html("div", "practice-review-head");
    this.title = html("strong", "", "Review a take");
    this.takeSelect = Object.assign(document.createElement("select"), { className: "dropdown", title: "Which recording to look at" });
    this.takeSelect.addEventListener("change", () => this.load(Number(this.takeSelect.value)));
    this.scoreChip = html("div", "practice-review-score");
    this.summary = html("span", "practice-review-summary");
    const zoom = html("label", "practice-review-zoom", "Zoom ");
    this.zoomInput = Object.assign(document.createElement("input"), { type: "range", min: 20, max: 240, step: 1, value: 70 });
    this.zoomInput.addEventListener("input", () => this.zoomTo(Number(this.zoomInput.value)));
    zoom.append(this.zoomInput);
    const done = Object.assign(html("button", "tool is-accent", "Back to practice"), { type: "button" });
    done.addEventListener("click", () => this.show(false));
    head.append(this.title, this.takeSelect, this.scoreChip, this.summary, zoom, done);

    this.legend = html("div", "practice-review-legend");
    for (const [cls, text] of [["is-flat", "Flat"], ["is-good", "In tune"], ["is-sharp", "Sharp"], ["is-off", "Not sung"]])
      this.legend.append(html("span", `practice-review-key ${cls}`, text));
    this.legend.append(html("small", "", "Drag to scroll · Ctrl+wheel to zoom · click a phrase to hear it"));

    this.stage = html("div", "practice-review-stage");
    this.canvas = document.createElement("canvas");
    this.note = html("div", "practice-review-note");
    this.note.hidden = true;
    this.stage.append(this.canvas, this.note);

    this.overview = html("div", "practice-review-overview");
    this.overviewCanvas = document.createElement("canvas");
    this.overview.append(this.overviewCanvas);

    this.focus = html("div", "practice-review-focus");
    this.empty = html("div", "practice-review-empty");

    this.root.append(head, this.legend, this.stage, this.overview, this.focus, this.empty);
    this.host.section.append(this.root);

    new ResizeObserver(() => this.draw()).observe(this.stage);

    // Scrolling: drag, wheel (Ctrl or ⌘ zooms), click a phrase.
    this.stage.addEventListener("pointerdown", (event) => {
      this.stage.setPointerCapture(event.pointerId);
      this.drag = { x: event.clientX, from: this.view.from, moved: false };
    });
    this.stage.addEventListener("pointermove", (event) => {
      if (!this.drag) return;
      const dx = event.clientX - this.drag.x;
      if (Math.abs(dx) > 3) this.drag.moved = true;
      this.view.from = this.clampFrom(this.drag.from - dx / this.view.pps);
    });
    this.stage.addEventListener("pointerup", (event) => {
      const moved = this.drag?.moved;
      this.drag = null;
      if (!moved) this.onClick(event);
    });
    this.stage.addEventListener("wheel", (event) => {
      event.preventDefault();
      if (event.ctrlKey || event.metaKey) {
        const box = this.stage.getBoundingClientRect();
        this.zoomTo(this.view.pps * Math.exp(-event.deltaY * 0.0025), event.clientX - box.left);
      } else {
        const delta = Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY;
        this.view.from = this.clampFrom(this.view.from + delta / this.view.pps);
      }
    }, { passive: false });

    const moveOverview = (event) => {
      if (!this.data) return;
      const box = this.overview.getBoundingClientRect();
      const t = this.span().from + ((event.clientX - box.left) / box.width) * this.span().length;
      this.view.from = this.clampFrom(t - this.visibleSeconds() / 2);
    };
    this.overview.addEventListener("pointerdown", (event) => { this.overview.setPointerCapture(event.pointerId); this.overviewDrag = true; moveOverview(event); });
    this.overview.addEventListener("pointermove", (event) => { if (this.overviewDrag) moveOverview(event); });
    this.overview.addEventListener("pointerup", () => { this.overviewDrag = false; });
  }

  // The stretch of the song the review covers (the whole song, so you can see what wasn't sung too).
  span() {
    const last = Math.max(1, this.data?.end ?? 1, ...(this.data?.phrases ?? []).map((p) => p.end));
    return { from: 0, length: last + 1 };
  }
  visibleSeconds() { return Math.max(1, this.stage.clientWidth) / this.view.pps; }
  clampFrom(from) {
    const { length } = this.span();
    return clamp(from, -1, Math.max(0, length - this.visibleSeconds() + 1));
  }
  zoomTo(pps, anchorPx = this.stage.clientWidth / 2) {
    const next = clamp(pps, 20, 240);
    const at = this.view.from + anchorPx / this.view.pps;
    this.view.pps = next;
    this.view.from = this.clampFrom(at - anchorPx / next);
    this.zoomInput.value = String(Math.round(next));
  }

  //==============================================================================
  async show(on, takeId = -1) {
    this.open = on;
    this.root.hidden = !on;
    this.host.section.classList.toggle("is-reviewing", on);
    if (!on) {
      this.host.practice("stopReview");
      return;
    }
    await this.refresh(takeId);
  }

  // The takes over this song: the newest is looked at first.
  async refresh(wanted = -1) {
    let takes = [];
    try { takes = (await this.host.practice("takes")) ?? []; } catch { /* no engine */ }
    this.takes = takes;
    this.takeSelect.replaceChildren();
    takes.forEach((take, i) => this.takeSelect.append(Object.assign(document.createElement("option"),
      { value: String(take.id), textContent: `Take ${i + 1} · ${mmss(take.start)}–${mmss(take.start + take.seconds)}${take.muted ? " (muted)" : ""}` })));
    this.takeSelect.hidden = takes.length < 2;
    if (!takes.length) {
      this.data = null;
      this.empty.hidden = false;
      this.empty.replaceChildren(html("p", "big", "No takes yet"),
        html("p", "", "Press Record under the lane, sing along to the song, then stop. Your take shows up here with your pitch laid over the notes, so you can see where to work on your tuning."));
      this.scoreChip.textContent = "";
      this.summary.textContent = "";
      this.focus.replaceChildren();
      return;
    }
    const id = takes.some((t) => t.id === wanted) ? wanted : takes.some((t) => t.id === this.takeId) ? this.takeId : takes[takes.length - 1].id;
    this.takeSelect.value = String(id);
    await this.load(id);
  }

  async load(id) {
    this.takeId = id;
    this.data = null;
    this.empty.hidden = false;
    this.empty.replaceChildren(html("p", "big", "Listening to your take…"), html("p", "", "Following your pitch through the whole recording."));
    this.focus.replaceChildren();
    this.scoreChip.textContent = "";
    this.summary.textContent = "";
    this.host.practice("review", { id });
  }

  // The engine's answer.
  onReview(event) {
    if (!this.open) return;
    const r = event.review;
    if (!r) {
      this.data = null;
      this.empty.hidden = false;
      this.empty.replaceChildren(html("p", "big", "Can't review this take"), html("p", "", event.error || "Try again in a moment."));
      return;
    }
    if (r.take !== this.takeId) return; // (an older answer)

    const notes = [];
    for (let i = 0; i + 3 < r.notes.length; i += 4) notes.push({ start: r.notes[i], end: r.notes[i + 1], pitch: r.notes[i + 2], phrase: r.notes[i + 3] });
    const phrases = [];
    for (let i = 0; i + 1 < r.phrases.length; i += 2) phrases.push({ start: r.phrases[i], end: r.phrases[i + 1] });
    const frames = [];
    for (let i = 0; i + 1 < r.frames.length; i += 2) frames.push({ time: r.frames[i], midi: r.frames[i + 1] });
    this.data = {
      ...r, notes, phrases, frames,
      noteScores: new Map(r.noteScores.map((n) => [n.i, n])),
      phraseScores: new Map(r.phraseScores.map((p) => [p.i, p])),
    };
    this.empty.hidden = true;
    this.selected = -1;

    // Start where the take starts.
    this.view.from = this.clampFrom(Math.max(0, r.start - 1));

    const scored = [...this.data.phraseScores.values()];
    this.scoreChip.textContent = scored.length ? String(Math.round(r.score)) : "–";
    this.scoreChip.className = `practice-review-score ${scored.length ? this.host.scoreClass(r.score) : ""}`;
    const parts = [];
    if (!scored.length) parts.push("No phrase of the song was in this take.");
    else {
      parts.push(`${scored.length} phrase${scored.length === 1 ? "" : "s"} sung`);
      if (Math.abs(r.lean) >= 12) parts.push(`leaning ${Math.round(Math.abs(r.lean))}¢ ${r.lean > 0 ? "sharp" : "flat"} overall`);
      else parts.push("sitting in the middle of the notes overall");
    }
    this.summary.textContent = parts.join(" · ");
    this.buildFocus();
  }

  buildFocus() {
    this.focus.replaceChildren();
    const weak = [...this.data.phraseScores.entries()].filter(([, p]) => p.scored && p.score < 80)
      .sort((a, b) => a[1].score - b[1].score).slice(0, 8);
    this.focus.append(html("div", "practice-review-focus-title", weak.length ? "Where to focus" : "Nothing to fix"));
    if (!weak.length) {
      this.focus.append(html("p", "practice-report-note", "Every phrase you sang scored 80 or more. Try it in another key, or at a faster tempo."));
      return;
    }
    const list = html("div", "practice-review-focus-list");
    for (const [i, p] of weak) {
      const phrase = this.data.phrases[i];
      const card = html("div", "practice-review-card");
      const issues = phraseIssues(p);
      const top = html("div", "practice-review-card-top");
      top.append(html("span", `practice-weak-score ${this.host.scoreClass(p.score)}`, String(Math.round(p.score))),
                 html("span", "practice-weak-name", `Phrase ${i + 1}`), html("small", "", mmss(phrase.start)));
      const what = html("div", "practice-review-card-what", issues.length ? issues.join(" · ") : "Uneven overall");
      const buttons = html("div", "practice-review-card-buttons");
      const show = Object.assign(html("button", "tool", "Show"), { type: "button" });
      show.addEventListener("click", () => this.reveal(i));
      const hear = Object.assign(html("button", "tool", "Hear"), { type: "button", title: "Play the song from just before this phrase (your take plays with it)" });
      hear.addEventListener("click", () => { this.reveal(i); this.host.goTo(Math.max(0, phrase.start - 1.5)); });
      const loop = Object.assign(html("button", "tool is-accent", "Practise"), { type: "button", title: "Loop this phrase in Practice" });
      loop.addEventListener("click", () => { this.show(false); this.host.loopPhrase(i); });
      buttons.append(show, hear, loop);
      card.append(top, what, buttons);
      list.append(card);
    }
    this.focus.append(list);
  }

  // Scrolls so the phrase is in the middle, and marks it.
  reveal(i) {
    const phrase = this.data?.phrases[i];
    if (!phrase) return;
    this.selected = i;
    this.view.from = this.clampFrom((phrase.start + phrase.end) / 2 - this.visibleSeconds() / 2);
  }

  onClick(event) {
    if (!this.data) return;
    const box = this.stage.getBoundingClientRect();
    const t = this.view.from + (event.clientX - box.left) / this.view.pps;
    const i = this.data.phrases.findIndex((p) => t >= p.start - 0.2 && t <= p.end + 0.2);
    if (i < 0) return;
    this.selected = i;
    this.host.goTo(Math.max(0, this.data.phrases[i].start - 1.5));
  }

  //==============================================================================
  // The colour of a sung pitch `midi` against the note aimed for: where it sat.
  static colourFor(cents, colours) {
    if (Math.abs(cents) <= IN_TUNE) return colours.good;
    if (Math.abs(cents) <= CLOSE) return colours.close;
    return cents < 0 ? colours.flat : colours.sharp;
  }

  draw() {
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.stage.clientWidth * ratio));
    const height = Math.max(1, Math.round(this.stage.clientHeight * ratio));
    if (this.canvas.width !== width) this.canvas.width = width;
    if (this.canvas.height !== height) this.canvas.height = height;
    const g = this.canvas.getContext("2d");
    g.clearRect(0, 0, width, height);
    const d = this.data;
    if (!d) { this.note.hidden = true; return; }

    const css = getComputedStyle(this.host.section);
    const color = (name, fallback) => css.getPropertyValue(name).trim() || fallback;
    const colours = { good: color("--good", "#3fd9c6"), close: color("--close", "#ffc94d"), off: color("--off", "#ff7a90"),
                      flat: color("--flat", "#5aa9ff"), sharp: color("--sharp", "#ff9a5a") };
    const target = color("--target", "#8c7bff"), line = color("--line", "#242a37"), faint = color("--faint", "#566077");

    // Follow the song while it plays.
    const now = this.host.now();
    const playing = this.host.isPlaying();
    if (playing && !this.drag) {
      const x = (now - this.view.from) * this.view.pps;
      if (x > this.stage.clientWidth * 0.85 || x < 0) this.view.from = this.clampFrom(now - this.visibleSeconds() * 0.25);
    }

    const pps = this.view.pps * ratio;
    const from = this.view.from, to = from + width / pps;
    const x = (t) => (t - from) * pps;
    const lane = LANE_PHRASES * ratio;
    const span = Math.max(14, d.highest - d.lowest + 6);
    const top = (d.lowest + d.highest) / 2 + span / 2;
    const rowHeight = (height - lane) / span;
    const y = (midi) => lane + (top - midi) * rowHeight;

    // Semitone rows with the C names.
    g.font = `${10 * ratio}px ui-monospace, Menlo, monospace`;
    g.textBaseline = "middle";
    for (let m = Math.ceil(top - span); m <= Math.floor(top); ++m) {
      const pc = ((m % 12) + 12) % 12;
      if ([1, 3, 6, 8, 10].includes(pc)) { g.fillStyle = "rgba(255,255,255,0.025)"; g.fillRect(0, y(m) - rowHeight / 2, width, rowHeight); }
      if (pc === 0) {
        g.fillStyle = line; g.fillRect(0, y(m) + rowHeight / 2, width, ratio);
        g.fillStyle = faint; g.fillText(this.host.noteName(m), 6 * ratio, y(m));
      }
    }

    // Phrases: a band behind the notes and a score chip along the top.
    g.textBaseline = "middle";
    d.phrases.forEach((p, i) => {
      if (p.end < from || p.start > to) return;
      const score = d.phraseScores.get(i);
      const left = x(p.start), w = Math.max(2 * ratio, (p.end - p.start) * pps);
      g.fillStyle = i === this.selected ? "rgba(140,123,255,0.16)" : "rgba(140,123,255,0.05)";
      g.fillRect(left, lane, w, height - lane);
      g.fillStyle = score?.scored ? this.fill(score.score, colours) : "rgba(255,255,255,0.07)";
      g.globalAlpha = score?.scored ? 0.9 : 1;
      g.fillRect(left, 0, Math.max(1, w - ratio), lane - 3 * ratio);
      g.globalAlpha = 1;
      if (score?.scored && w > 34 * ratio) {
        g.fillStyle = "#0b0d12";
        g.font = `600 ${10.5 * ratio}px ui-sans-serif, system-ui, sans-serif`;
        const issues = phraseIssues(score);
        const label = `${Math.round(score.score)}${w > 120 * ratio && issues.length ? ` · ${issues[0]}` : ""}`;
        g.fillText(label, left + 5 * ratio, (lane - 3 * ratio) / 2, w - 8 * ratio);
      }
    });

    // The notes to sing, coloured by how each was sung.
    const barHeight = Math.max(4 * ratio, rowHeight * 0.72);
    d.notes.forEach((note, i) => {
      if (note.end < from || note.start > to) return;
      const score = d.noteScores.get(i);
      g.globalAlpha = score ? 0.85 : 0.4;
      g.fillStyle = score ? (score.sung ? this.fill(score.score, colours) : faint) : target;
      const left = x(note.start), w = Math.max(2 * ratio, (note.end - note.start) * pps - ratio);
      this.round(g, left, y(note.pitch) - barHeight / 2, w, barHeight, Math.min(barHeight / 2, 5 * ratio));
      g.fill();
      if (score?.sung && score.onset && Math.abs(score.timing) >= 0.1 && w > 10 * ratio) {
        // Came in late or early: a small mark on the note's start.
        g.globalAlpha = 1;
        g.fillStyle = "#fff";
        g.font = `${9 * ratio}px ui-monospace, Menlo, monospace`;
        g.fillText(`${score.timing > 0 ? "+" : "−"}${Math.round(Math.abs(score.timing) * 1000)}`, left + 2 * ratio, y(note.pitch) - barHeight / 2 - 6 * ratio);
      }
    });
    g.globalAlpha = 1;

    // Your pitch, folded to the octave of the note it aimed for and coloured by where it sat.
    g.lineWidth = 3 * ratio;
    g.lineCap = "round";
    const lo = this.firstFrame(from - 0.1);
    let previous = null, noteIndex = 0;
    for (let k = lo; k < d.frames.length; ++k) {
      const f = d.frames[k];
      if (f.time > to + 0.1) break;
      if (f.midi <= 0) { previous = null; continue; }
      while (noteIndex < d.notes.length - 1 && d.notes[noteIndex].end < f.time - 0.4) ++noteIndex;
      const aim = this.aimAt(f.time, noteIndex);
      const midi = aim ? this.host.foldToOctave(f.midi, aim) : f.midi;
      const cents = aim ? (midi - aim) * 100 : 999;
      const point = { x: x(f.time), y: y(midi), color: aim ? TakeReview.colourFor(cents, colours) : "rgba(233,236,244,0.55)" };
      if (previous && point.x - previous.x < 10 * ratio && Math.abs(point.y - previous.y) < 3 * rowHeight) {
        g.strokeStyle = point.color;
        g.beginPath();
        g.moveTo(previous.x, previous.y);
        g.lineTo(point.x, point.y);
        g.stroke();
      }
      previous = point;
    }

    // The part of the song this take covers, and the playhead.
    g.fillStyle = "rgba(255,255,255,0.05)";
    g.fillRect(0, 0, Math.max(0, x(d.start)), height);
    g.fillRect(Math.min(width, x(d.end)), 0, width, height);
    if (now >= from && now <= to) {
      g.fillStyle = "rgba(255,255,255,0.55)";
      g.fillRect(Math.round(x(now)), 0, ratio * 1.5, height);
    }

    this.drawOverview(colours, now);
  }

  // The first frame at or after `time` (frames are in time order).
  firstFrame(time) {
    const frames = this.data.frames;
    let a = 0, b = frames.length;
    while (a < b) {
      const m = (a + b) >> 1;
      if (frames[m].time < time) a = m + 1; else b = m;
    }
    return a;
  }

  // The note aimed for at `time`: the one under it, or the nearest within half a second.
  aimAt(time, from) {
    const notes = this.data.notes;
    let best = 0, distance = 0.5;
    for (let i = Math.max(0, from - 1); i < notes.length; ++i) {
      const n = notes[i];
      if (n.start - 0.5 > time) break;
      const gap = time < n.start ? n.start - time : time > n.end ? time - n.end : 0;
      if (gap <= distance) { best = n.pitch; distance = gap; if (gap === 0) break; }
    }
    return best;
  }

  fill(score, colours) { return score >= 80 ? colours.good : score >= 55 ? colours.close : colours.off; }

  // The whole song at a glance: every phrase coloured by its score, and the part on screen.
  drawOverview(colours, now) {
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.overview.clientWidth * ratio));
    const height = Math.max(1, Math.round(this.overview.clientHeight * ratio));
    if (this.overviewCanvas.width !== width) this.overviewCanvas.width = width;
    if (this.overviewCanvas.height !== height) this.overviewCanvas.height = height;
    const g = this.overviewCanvas.getContext("2d");
    g.clearRect(0, 0, width, height);
    const d = this.data;
    const { length } = this.span();
    const x = (t) => (t / length) * width;

    g.fillStyle = "rgba(255,255,255,0.04)";
    g.fillRect(x(d.start), 0, Math.max(1, x(d.end) - x(d.start)), height);
    d.phrases.forEach((p, i) => {
      const score = d.phraseScores.get(i);
      g.fillStyle = score?.scored ? this.fill(score.score, colours) : "rgba(255,255,255,0.14)";
      g.globalAlpha = score?.scored ? 0.9 : 1;
      g.fillRect(x(p.start), height * 0.3, Math.max(2 * ratio, x(p.end) - x(p.start)), height * 0.4);
    });
    g.globalAlpha = 1;
    g.strokeStyle = "rgba(255,255,255,0.8)";
    g.lineWidth = 1.5 * ratio;
    g.strokeRect(x(this.view.from) + ratio, ratio, Math.max(4 * ratio, x(this.visibleSeconds()) - 2 * ratio), height - 2 * ratio);
    g.fillStyle = "#fff";
    g.fillRect(x(now), 0, ratio, height);
  }

  round(g, x, y, w, h, r) {
    g.beginPath();
    g.moveTo(x + r, y);
    g.arcTo(x + w, y, x + w, y + h, r);
    g.arcTo(x + w, y + h, x, y + h, r);
    g.arcTo(x, y + h, x, y, r);
    g.arcTo(x, y, x + w, y, r);
    g.closePath();
  }
}
