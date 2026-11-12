// The project's tempo over time in the DAW screen: the same tempo map as src/daw/TempoMap
// (points [beat, bpm, ramp]: a ramp glides into its point from the one before), and the
// tempo lane under the ruler where points are added, dragged and removed.
import { html } from "./widgets.js";

export class TempoMap {
  constructor(points, fallback = 120) {
    const list = (points?.length ? points : [[0, fallback, false]]).map(([b, t, r]) => [Number(b), Number(t), !!r]);
    list.sort((a, b) => a[0] - b[0]);
    list[0] = [0, list[0][1], false];
    this.points = list;
    this.starts = [0];
    for (let i = 1; i < list.length; ++i) this.starts.push(this.starts[i - 1] + this.within(i - 1, list[i][0]));
  }

  get steady() { return this.points.length === 1; }

  slope(i) {
    const next = this.points[i + 1];
    return next?.[2] ? (next[1] - this.points[i][1]) / (next[0] - this.points[i][0]) : 0;
  }

  // Seconds from point i to a beat in its span (the integral of 60 / tempo).
  within(i, beat) {
    const [b0, t0] = this.points[i], k = this.slope(i), db = beat - b0;
    if (Math.abs(k) < 1e-9 || db <= 0) return 60 * db / t0;
    return 60 / k * Math.log((t0 + k * db) / t0);
  }

  indexAt(beat) {
    let i = 0;
    while (i + 1 < this.points.length && this.points[i + 1][0] <= beat) ++i;
    return i;
  }

  toSeconds(beat) {
    const i = this.indexAt(beat);
    return this.starts[i] + this.within(i, beat);
  }

  toBeats(seconds) {
    let i = 0;
    while (i + 1 < this.starts.length && this.starts[i + 1] <= seconds) ++i;
    const [b0, t0] = this.points[i], k = this.slope(i), s = seconds - this.starts[i];
    if (Math.abs(k) < 1e-9 || s <= 0) return b0 + s * t0 / 60;
    return b0 + t0 * (Math.exp(k * s / 60) - 1) / k;
  }

  bpmAtBeat(beat) {
    const i = this.indexAt(beat);
    return this.points[i][1] + this.slope(i) * Math.max(0, beat - this.points[i][0]);
  }

  bpmAt(seconds) { return this.bpmAtBeat(this.toBeats(seconds)); }

  snap(seconds, beats) { return this.toSeconds(Math.round(this.toBeats(seconds) / beats) * beats); }
}

// Grid lines for lanes when the tempo changes: an SVG as a CSS background, a line per bar
// (brighter) and per beat (unless beats are too close together).
export function gridImage(map, beatsPerBar, zoom, width) {
  const lastBeat = Math.ceil(map.toBeats(width / zoom)) + 1;
  const parts = [];
  let previous = -Infinity;
  for (let beat = 0; beat <= lastBeat; ++beat) {
    const x = map.toSeconds(beat) * zoom;
    const bar = beat % beatsPerBar === 0;
    if (!bar && x - previous < 8) continue;
    previous = x;
    parts.push(`<rect x="${x.toFixed(1)}" width="1" height="1" fill="rgba(255,255,255,${bar ? 0.06 : 0.025})"/>`);
  }
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${Math.ceil(width)}" height="1" viewBox="0 0 ${Math.ceil(width)} 1" preserveAspectRatio="none">${parts.join("")}</svg>`;
  return `url("data:image/svg+xml,${encodeURIComponent(svg)}")`;
}

const HEIGHT = 46, PAD = 6;

// The tempo lane: a line through the tempo points. Click to add a point (at the grid, with
// the tempo under the pointer), drag one to move it (the first only up and down), double-
// click to remove it, Alt-click (or right-click) to switch between a step and a ramp.
// `screen` gives zoom, snapTime(seconds, event), beatsPerBar, gridBeats and session.
export function createTempoLane(screen, daw) {
  const element = html("div", "tempo-lane");
  element.title = "Tempo: click to add a change · drag to move · double-click to remove · Alt-click: ramp or step";
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.classList.add("tempo-curve");
  element.append(svg);
  let range = [60, 180], points = [], width = 0;

  const yOf = (bpm) => PAD + (1 - (bpm - range[0]) / (range[1] - range[0])) * (HEIGHT - 2 * PAD);
  const send = (list, more = {}) => daw("tempoMap", { points: list, ...more });

  const draw = (list) => {
    const map = new TempoMap(list);
    const zoom = screen.zoom;
    const node = (tag, attrs) => {
      const n = document.createElementNS("http://www.w3.org/2000/svg", tag);
      for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
      return n;
    };
    svg.setAttribute("width", width);
    svg.setAttribute("height", HEIGHT);
    svg.replaceChildren();
    // The curve: straight across steps, sampled along ramps.
    let d = "";
    map.points.forEach(([beat, bpm], i) => {
      const x = map.toSeconds(beat) * zoom;
      if (i === 0) d += `M0 ${yOf(bpm)}`;
      else if (map.points[i][2]) {
        const [b0] = map.points[i - 1];
        for (let s = 1; s <= 24; ++s) {
          const b = b0 + (beat - b0) * s / 24;
          d += ` L${map.toSeconds(b) * zoom} ${yOf(map.bpmAtBeat(b))}`;
        }
      } else d += ` L${x} ${yOf(map.points[i - 1][1])} L${x} ${yOf(bpm)}`;
    });
    d += ` L${width} ${yOf(map.points[map.points.length - 1][1])}`;
    svg.append(node("path", { d, class: "tempo-line" }));
    map.points.forEach(([beat, bpm, ramp], i) => {
      const x = map.toSeconds(beat) * zoom;
      const dot = node("circle", { cx: x, cy: yOf(bpm), r: 4, class: `tempo-point${ramp ? " is-ramp" : ""}` });
      dot.dataset.index = i;
      svg.append(dot);
      const label = node("text", { x: x + 6, y: Math.max(10, yOf(bpm) - 5), class: "tempo-label" });
      label.textContent = `${Math.round(bpm * 10) / 10}${ramp ? " ↗" : ""}`;
      svg.append(label);
    });
  };

  const pointAt = (event) => {
    const dot = event.target.closest?.(".tempo-point");
    return dot ? Number(dot.dataset.index) : -1;
  };
  const local = (event) => {
    const box = element.getBoundingClientRect();
    return { x: event.clientX - box.left, y: event.clientY - box.top };
  };

  element.addEventListener("contextmenu", (event) => {
    const i = pointAt(event);
    if (i <= 0) return;
    event.preventDefault();
    const list = points.map((p) => [...p]);
    list[i][2] = !list[i][2];
    send(list);
  });

  element.addEventListener("dblclick", (event) => {
    const i = pointAt(event);
    if (i <= 0) return;
    send(points.filter((_, n) => n !== i));
  });

  element.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    const map = new TempoMap(points);
    let i = pointAt(event);
    const list = points.map((p) => [...p]);

    if (i > 0 && event.altKey) {
      list[i][2] = !list[i][2];
      send(list);
      return;
    }
    if (event.detail >= 2) return; // the dblclick handler removes it

    if (i < 0) {
      // A new point at the grid line under the pointer, with the tempo the line shows there.
      const { x } = local(event);
      const beat = map.toBeats(screen.snapTime(x / screen.zoom, event));
      if (beat <= 1e-6) return;
      list.push([beat, Math.round(map.bpmAtBeat(beat)), false]);
      list.sort((a, b) => a[0] - b[0]);
      i = list.findIndex((p) => p[0] === beat);
    }

    const start = local(event);
    const [startBeat, startBpm] = list[i];
    let moved = false;
    screen.dragging = true;
    element.setPointerCapture?.(event.pointerId);

    const move = (e) => {
      const at = local(e);
      if (Math.abs(at.x - start.x) + Math.abs(at.y - start.y) > 2) moved = true;
      list[i][1] = Math.min(400, Math.max(20, Math.round((startBpm + (start.y - at.y) * (range[1] - range[0]) / (HEIGHT - 2 * PAD)) * 10) / 10));
      if (i > 0) {
        const before = list[i - 1][0] + 1 / 16, after = list[i + 1]?.[0] - 1 / 16 || Infinity;
        const seconds = map.toSeconds(startBeat) + (at.x - start.x) / screen.zoom;
        const beat = map.toBeats(screen.snapTime(seconds, e));
        list[i][0] = Math.min(after, Math.max(before, beat));
      }
      draw(list);
    };
    const up = () => {
      element.removeEventListener("pointermove", move);
      element.removeEventListener("pointerup", up);
      screen.dragging = false;
      if (moved || list.length !== points.length) send(list);
      else draw(points);
    };
    element.addEventListener("pointermove", move);
    element.addEventListener("pointerup", up);
  });

  return {
    element,
    update(session, laneWidth) {
      points = (session.tempoMap?.length ? session.tempoMap : [[0, session.tempo ?? 120, false]]).map((p) => [...p]);
      width = laneWidth;
      const tempos = points.map((p) => p[1]);
      const lo = Math.min(...tempos), hi = Math.max(...tempos);
      range = [Math.max(20, Math.floor((lo - 20) / 10) * 10), Math.min(400, Math.ceil((hi + 20) / 10) * 10)];
      element.style.width = `${laneWidth}px`;
      draw(points);
    },
  };
}
