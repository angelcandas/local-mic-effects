// A clip's volume envelope: a line drawn over the clip, in dB, that the clip plays through
// (on top of its gain, before the track's fader). Points sit in the clip's own audio time
// (seconds of the file's played audio, the same as the clip's `offset`), so they move with
// the clip. The Envelope tool (H) edits them: click to add a point, drag a point, Alt+click
// a point to remove it. Alt while dragging turns the grid off.
import { clamp, svg } from "./widgets.js";

export const ENVELOPE_TOP = 12;      // dB at the top of the clip
export const ENVELOPE_BOTTOM = -60;  // dB at the bottom: silence
const HIT = 9;                       // px: how close the pointer must be to grab a point

const dbToFraction = (db) => (ENVELOPE_TOP - clamp(db, ENVELOPE_BOTTOM, ENVELOPE_TOP)) / (ENVELOPE_TOP - ENVELOPE_BOTTOM);
const fractionToDb = (f) => {
  const db = ENVELOPE_TOP - clamp(f, 0, 1) * (ENVELOPE_TOP - ENVELOPE_BOTTOM);
  return db < ENVELOPE_BOTTOM + 1 ? ENVELOPE_BOTTOM : Math.round(db * 10) / 10;
};

export function envelopeDbAt(points, time) {
  if (!points.length) return 0;
  if (time <= points[0][0]) return points[0][1];
  const last = points[points.length - 1];
  if (time >= last[0]) return last[1];
  let i = 1;
  while (points[i][0] < time) ++i;
  const [t0, d0] = points[i - 1], [t1, d1] = points[i];
  return t1 > t0 ? d0 + (d1 - d0) * ((time - t0) / (t1 - t0)) : d1;
}

// Draws the line (and, with the tool picked, the points) over a clip element.
export function drawClipEnvelope(element, clip, zoom, editing, height = 70) {
  element.querySelector(".clip-envelope")?.remove();
  const points = clip.envelope ?? [];
  if (clip.midi || (!points.length && !editing)) return;
  const width = Math.max(2, clip.length * zoom);
  const x = (t) => (t - clip.offset) * zoom;
  const y = (db) => dbToFraction(db) * height;

  const layer = svg("svg", { class: "clip-envelope", viewBox: `0 0 ${width} ${height}`, preserveAspectRatio: "none" });
  layer.style.width = `${width}px`;
  layer.style.height = `${height}px`;
  if (editing) layer.append(svg("line", { class: "env-zero", x1: 0, x2: width, y1: y(0), y2: y(0) }));
  if (points.length) {
    const d = [`M0 ${y(envelopeDbAt(points, clip.offset))}`];
    for (const [t, db] of points) if (x(t) > 0 && x(t) < width) d.push(`L${x(t)} ${y(db)}`);
    d.push(`L${width} ${y(envelopeDbAt(points, clip.offset + clip.length))}`);
    layer.append(svg("path", { class: "env-line", d: d.join(" ") }));
    if (editing)
      for (const [t, db] of points)
        if (x(t) >= -4 && x(t) <= width + 4) layer.append(svg("circle", { class: "env-point", cx: x(t), cy: y(db), r: 4 }));
  }
  element.append(layer);
}

// The Envelope tool's press on a clip. `snap(seconds, event)` snaps a timeline time to the
// grid; `commit(points)` stores the new envelope (one undo step); `redraw()` repaints.
export function pressClipEnvelope(event, { clip, element, zoom, snap, commit, redraw, label }) {
  event.preventDefault();
  const rect = element.getBoundingClientRect();
  const height = rect.height || 70;
  const toFileTime = (e) => {
    const timeline = clip.start + (e.clientX - rect.left) / zoom;
    return clip.offset + (snap(timeline, e) - clip.start);
  };
  const toDb = (e) => fractionToDb((e.clientY - rect.top) / height);
  const original = (clip.envelope ?? []).map((p) => [...p]);
  const pointer = [event.clientX - rect.left, event.clientY - rect.top];
  const near = original.findIndex(([t, db]) =>
    Math.hypot((t - clip.offset) * zoom - pointer[0], dbToFraction(db) * height - pointer[1]) <= HIT);

  if (event.altKey) { // remove the point under the pointer
    if (near >= 0) commit(original.filter((_, i) => i !== near));
    return;
  }

  let points = original;
  let index = near;
  if (index < 0) {
    const t = clamp(toFileTime(event), clip.offset, clip.offset + clip.length);
    const db = toDb(event);
    // The first point of an envelope brings 0 dB anchors at the clip's ends, so only that stretch changes.
    points = original.length ? original : [[clip.offset, 0], [clip.offset + clip.length, 0]];
    points = [...points, [t, db]].sort((a, b) => a[0] - b[0]);
    index = points.findIndex((p) => p[0] === t && p[1] === db);
  }
  clip.envelope = points;
  redraw();

  const target = event.currentTarget ?? element;
  target.setPointerCapture?.(event.pointerId);
  const say = (db) => { if (label) label.textContent = `${clip.name}  ${db <= ENVELOPE_BOTTOM ? "−∞" : db.toFixed(1)} dB`; };
  say(points[index][1]);

  const onMove = (e) => {
    const lo = index > 0 ? points[index - 1][0] + 0.001 : -Infinity;
    const hi = index < points.length - 1 ? points[index + 1][0] - 0.001 : Infinity;
    points[index] = [clamp(toFileTime(e), lo, hi), toDb(e)];
    clip.envelope = points;
    say(points[index][1]);
    redraw();
  };
  const onUp = () => {
    target.removeEventListener("pointermove", onMove);
    target.removeEventListener("pointerup", onUp);
    target.removeEventListener("pointercancel", onUp);
    if (label) label.textContent = clip.name;
    commit(points);
  };
  target.addEventListener("pointermove", onMove);
  target.addEventListener("pointerup", onUp);
  target.addEventListener("pointercancel", onUp);
}
