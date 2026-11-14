// The Tempo tool of the clip editor (ui/clipeditor.js): the clip's tempo definition, where
// the audio's own beats fall (its beat map, see Edit::BeatMap). Musical mode plays those
// beats on the project's grid, so a take that drifts comes out straight.
//
//   the waveform shows the audio's own grid (bars strong, beats light), in its own colour
//   drag the first bar line          move the downbeat
//   drag any other line              change the tempo (it scales around the downbeat)
//   Ctrl/Cmd-drag a line             pin just that beat there (a point; the others stay)
//   double-click a pinned point      remove it
//
// The beat map's points are [[seconds, beat], ...] in the clip's straight time: its file time
// after its free warp (for a clip that isn't musical, that's its rendered time).
import { clamp, html } from "./widgets.js";
import { toSource, toTarget } from "./rhythm.js";

const GRAB_PX = 6;

// Own beat at straight time x, and back (the first and last segments run on).
export function beatAt(map, x) {
  if (map.length < 2) return x * 2;
  let i = 1;
  while (i + 1 < map.length && x > map[i][0]) ++i;
  const [s0, b0] = map[i - 1], [s1, b1] = map[i];
  return b0 + (x - s0) * (b1 - b0) / (s1 - s0);
}
export function secondsAt(map, beat) {
  if (map.length < 2) return beat / 2;
  let i = 1;
  while (i + 1 < map.length && beat > map[i][1]) ++i;
  const [s0, b0] = map[i - 1], [s1, b1] = map[i];
  return s0 + (beat - b0) * (s1 - s0) / (b1 - b0);
}

export function createTempoTool(ctx) {
  const { daw, RULER, button, toast } = ctx;
  let shown = false;
  let dragged = null; // the beat map while a line is dragged

  //================================================================ the section's body
  const body = html("div", "tempo-tool");
  const info = html("p", "panel-caption tempo-tool-info");
  const define = async (mode, extra = {}) => {
    const result = await daw("defineTempo", { id: ctx.active().clip.id, mode, ...extra });
    if (result?.ok && (mode === "steady" || mode === "follow")) toast(`The clip's tempo: ${result.clipTempo.toFixed(2)} BPM${mode === "follow" && result.beatMap?.length > 2 ? `, following ${result.beatMap.length} points` : ""}.`);
  };
  const detectRow = html("div", "panel-actions");
  detectRow.append(
    button("tool", "Detect: steady", "Find one tempo and the first downbeat in the audio", () => define("steady")),
    button("tool", "Detect: follow", "Follow every beat as played, for a take that speeds up and slows down", () => define("follow")));
  const scaleRow = html("div", "panel-actions");
  scaleRow.append(
    button("tool", "÷2", "Half the tempo (each beat becomes two)", () => define("half")),
    button("tool", "×2", "Double the tempo", () => define("double")),
    button("tool", "From the project", "The take was played to the project's click: its beats are the project's beats where it plays now", () => define("project")),
    button("tool", "Clear", "Forget the definition (the clip's tempo number stays)", () => define("clear")));

  const bpm = html("input", "tempo-input");
  Object.assign(bpm, { type: "number", min: 30, max: 300, step: 0.01, placeholder: "BPM", title: "The clip's tempo (the first segment's, for a drifting one): typing scales the whole definition" });
  bpm.addEventListener("change", () => { if (Number(bpm.value) > 0) define("bpm", { bpm: Number(bpm.value) }); });
  const bars = html("input", "tempo-input");
  Object.assign(bars, { type: "number", min: 0.25, max: 999, step: 0.25, placeholder: "bars", title: "How many bars the clip is: sets the tempo from its length" });
  bars.addEventListener("change", () => { if (Number(bars.value) > 0) define("bars", { bars: Number(bars.value) }); });
  const fields = html("div", "panel-actions tempo-tool-fields");
  const label = (input, text) => { const l = html("label", "tempo"); l.append(input, html("span", "", text)); return l; };
  fields.append(label(bpm, "BPM"), label(bars, "bars"));

  const fromClip = button("tool", "Project tempo from this clip", "Set the project's tempo from this clip (a drifting clip gives a drifting tempo map); the clip stops being musical so it plays as now", async () => {
    const result = await daw("tempoFromClip", { id: ctx.active().clip.id });
    if (result?.ok) toast(`Project tempo ${result.tempo.toFixed(2)} BPM from the clip.`);
  });
  const caption = html("p", "panel-caption", "Drag the first bar line to move the downbeat, any other line to change the tempo. Ctrl/Cmd-drag pins one beat; double-click a pin removes it.");
  body.append(info, detectRow, scaleRow, fields, fromClip, caption);

  //================================================================ the clip's definition
  const definition = (lane = ctx.active()) => {
    const c = lane.clip;
    if (Array.isArray(c.beatMap) && c.beatMap.length >= 2) return c.beatMap.map((p) => [...p]);
    return [[0, 0], [1, (c.clipTempo || 120) / 60]];
  };
  const defined = (lane = ctx.active()) => Array.isArray(lane.clip.beatMap) && lane.clip.beatMap.length >= 2;
  // Straight time <-> rendered time of the lane's clip.
  const free = (lane) => (lane.clip.musical ? lane.clip.freeWarp ?? [] : null);
  const renderedOf = (lane, x) => {
    const f = free(lane);
    return f ? toTarget(lane.map(), f.length ? toSource(f, x) : x) : x;
  };
  const straightOf = (lane, t) => {
    const f = free(lane);
    if (!f) return t;
    const file = toSource(lane.map(), t);
    return f.length ? toTarget(f, file) : file;
  };
  const perBar = () => ctx.screen.beatsPerBar || 4;

  // The own-grid line under the pointer: its beat, or null.
  const lineAt = (lane, map, clientX) => {
    const c = lane.clip, w = ctx.width();
    let best = null, distance = GRAB_PX;
    const from = beatAt(map, straightOf(lane, c.offset)), to = beatAt(map, straightOf(lane, c.offset + c.length));
    for (let b = Math.ceil(from); b <= to; ++b) {
      const x = ctx.xOf(lane.toTimeline(renderedOf(lane, secondsAt(map, b))));
      const d = Math.abs(x - (clientX - ctx.wave.getBoundingClientRect().left));
      if (x >= 0 && x <= w && d < distance) { distance = d; best = b; }
    }
    return best;
  };

  //================================================================ pointer
  const pointerdown = (event) => {
    const lane = ctx.active();
    if (ctx.laneAt(event.clientY) !== lane && ctx.lanes.length > 1) return false;
    const map = definition(lane);
    const beat = lineAt(lane, map, event.clientX);
    if (beat == null) return false; // not on a line: locate / scroll as usual

    if (event.detail === 2) { // double-click a pinned point: remove it
      const i = map.findIndex(([, b]) => Math.abs(b - beat) < 1e-6);
      if (i >= 0 && map.length > 2 && defined(lane)) { map.splice(i, 1); daw("setClip", { id: lane.clip.id, beatMap: map }); }
      return true;
    }

    const pin = event.ctrlKey || event.metaKey;
    const origin = secondsAt(map, 0), at = secondsAt(map, beat);
    const move = (e) => {
      const x = straightOf(lane, lane.toRendered(ctx.timeAt(e.clientX)));
      let next;
      if (pin) {
        const others = map.filter(([, b]) => Math.abs(b - beat) > 1e-6);
        const before = others.filter(([, b]) => b < beat).pop(), after = others.find(([, b]) => b > beat);
        const low = before ? before[0] + 0.02 : -Infinity, high = after ? after[0] - 0.02 : Infinity;
        next = [...others, [clamp(x, low, high), beat]].sort((p, q) => p[1] - q[1]);
      } else if (Math.abs(beat) < 1e-6) {
        next = map.map(([s, b]) => [s + x - origin, b]); // the downbeat moves, the tempo stays
      } else {
        const scale = (x - origin) / (at - origin);
        if (!(scale > 0.25 && scale < 4)) return;
        next = map.map(([s, b]) => [origin + (s - origin) * scale, b]);
      }
      const tempo = 60 * (next[1][1] - next[0][1]) / (next[1][0] - next[0][0]);
      if (!(tempo >= 30 && tempo <= 300)) return;
      dragged = next;
      ctx.redraw();
    };
    const up = () => {
      ctx.wave.removeEventListener("pointermove", move);
      ctx.wave.removeEventListener("pointerup", up);
      ctx.wave.removeEventListener("pointercancel", up);
      if (dragged) daw("setClip", { id: lane.clip.id, beatMap: dragged });
      dragged = null;
      ctx.redraw();
    };
    ctx.wave.setPointerCapture?.(event.pointerId);
    ctx.wave.addEventListener("pointermove", move);
    ctx.wave.addEventListener("pointerup", up);
    ctx.wave.addEventListener("pointercancel", up);
    return true;
  };

  const hover = (event) => {
    const lane = ctx.active();
    ctx.wave.classList.toggle("is-on-line", lineAt(lane, definition(lane), event.clientX) != null);
  };
  const leave = () => ctx.wave.classList.remove("is-on-line");

  //================================================================ drawing
  const draw = (visible) => {
    shown = visible;
    ctx.layer.querySelectorAll(".own-beat").forEach((e) => e.remove());
    if (!visible) return;
    const lane = ctx.active(), c = lane.clip, w = ctx.width();
    const map = dragged ?? definition(lane);
    const pinned = defined(lane) || dragged ? map.map(([, b]) => b) : [];
    const from = beatAt(map, straightOf(lane, c.offset)), to = beatAt(map, straightOf(lane, c.offset + c.length));
    const px = (b) => ctx.xOf(lane.toTimeline(renderedOf(lane, secondsAt(map, b))));
    const every = px(Math.ceil(from) + 1) - px(Math.ceil(from)) < 5 ? perBar() : 1;
    const bar = perBar();
    for (let b = Math.ceil(from / every) * every; b <= to && b - from < 5000; b += every) {
      const x = px(b);
      if (x < 0 || x > w) continue;
      const isBar = ((b % bar) + bar) % bar === 0;
      const line = html("div", `own-beat${isBar ? " is-bar" : ""}${b === 0 ? " is-downbeat" : ""}${pinned.some((p) => Math.abs(p - b) < 1e-6) && pinned.length > 2 ? " is-pinned" : ""}`);
      Object.assign(line.style, { left: `${x}px`, top: `${RULER + lane.top}px`, height: `${lane.height}px` });
      if (isBar) line.dataset.bar = String(Math.floor(b / bar) + 1);
      ctx.layer.append(line);
    }
  };

  const update = () => {
    const lane = ctx.active(), c = lane.clip;
    const map = definition(lane);
    const steady = map.length <= 2;
    info.textContent = defined(lane)
      ? `The clip is ${c.clipTempo.toFixed(2)} BPM${steady ? ", steady" : `, drifting (${map.length} points)`}.${c.musical ? " Musical: its beats play on the project's grid." : " Turn on Musical to play it to the project's tempo."}`
      : c.musical ? `${c.clipTempo.toFixed(2)} BPM from the file's start. Detect to find its downbeat.` : "Not defined yet: Detect, type its BPM or bars, or From the project.";
    if (document.activeElement !== bpm) bpm.value = defined(lane) || c.musical ? (Math.round(c.clipTempo * 100) / 100).toString() : "";
    if (document.activeElement !== bars) {
      const length = beatAt(map, straightOf(lane, c.offset + c.length)) - beatAt(map, straightOf(lane, c.offset));
      bars.value = defined(lane) ? String(Math.round(length / perBar() * 100) / 100) : "";
    }
    fromClip.disabled = !defined(lane) && !c.musical;
  };

  const hint = () => "The lines are the clip's own beats. Drag the first bar line to move the downbeat, another line to change the tempo; Ctrl/Cmd-drag pins one beat.";

  return { body, pointerdown, hover, leave, draw, update, hint, get shown() { return shown; }, dispose: () => {} };
}
