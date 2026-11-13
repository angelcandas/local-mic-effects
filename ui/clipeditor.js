// The clip editor (Wave view): an audio clip's time, laid out the way a sample editor is.
//  - a toolbar: Wave / Notes, Musical mode and the clip's tempo, the algorithm, Compare
//    (pitch, time), Solo, Loop, Fit;
//  - an overview of the whole rendered copy with the clip's window lit and the view box;
//  - a ruler in the project's bars and beats (click: locate, drag: loop range), the grid,
//    the playhead;
//  - the waveform, as a list of lanes: the selected clip, and every other audio clip
//    selected with it, stacked on one timeline (Edit: Active clip | All clips);
//  - on the left, one section per job: Pitch, Warp, Hitpoints, Quantize. The open section
//    is the active tool. With no tool open (or Pitch) the pointer only locates, scrolls and
//    zooms, so nothing in the audio changes by accident.
// The Notes view is ui/pitchedit.js; the Warp tool's gestures are ui/warptool.js.
import { clamp, html } from "./widgets.js";
import { createNoteEditor } from "./pitchedit.js";
import { createWarpTool } from "./warptool.js";
import { algorithmControls, keyCaption, pitchKnobs, prefs, savedQuantizeArgs, semitoneText } from "./rhythm.js";

export const RULER = 20; // px: the ruler lane at the top of the waveform (bars, tab handles)

// The sections on the left: [key, title, shortcut, is a tool (the pointer edits)].
const SECTIONS = [
  ["pitch", "Pitch", "", false],
  ["warp", "Warp", "1", true],
  ["hits", "Hitpoints", "2", false],
  ["quantize", "Quantize", "4", false],
];

const HINTS = {
  none: "Click to move the playhead, drag to scroll, Ctrl/Cmd + scroll to zoom. Open Warp (1) to change the timing.",
  pitch: "Click to move the playhead, drag to scroll, Ctrl/Cmd + scroll to zoom. Open Warp (1) to change the timing.",
  hits: "The faint lines are note starts found in the audio: the Warp tool snaps to them.",
  quantize: "Quantize warps the clip so its note starts land on the grid.",
};

// The waveform's lanes: one per clip, each with its own peaks, picture remap and hitpoints.
function createLane(clip, track) {
  return {
    clip, track,
    peaks: null, asking: null, retry: 0, pending: [], // waveform peaks, the stretches still rendering
    remap: null, drag: null, // new rendered time -> time in the picture we have (settled edits, the drag)
    hits: [], hitsKey: "",
    top: 0, height: 0, // px within the waveform, under the ruler
    toTimeline(t) { return this.clip.start + t - this.clip.offset; },
    toRendered(time) { return time - this.clip.start + this.clip.offset; },
    // file seconds -> rendered seconds, as the page can follow it: the whole map for a
    // musical clip (with the bend), else the warp markers.
    map() { return this.clip.timeMap ?? this.clip.warp ?? []; },
    setDrag(fn) { this.drag = fn; },
    settle(fn) { const earlier = this.remap; this.remap = earlier ? (t) => earlier(fn(t)) : fn; },
  };
}

// `ui` carries what the DAW screen shares: daw, peaks (no error toasts), ValueKnob, button,
// toast. `others`: the other audio clips selected with it ([{ track, clip }]), as more lanes.
// The sections and the toolbar are the selected (active) clip's; click a lane's name to
// make that clip the active one.
export function createClipEditor(screen, track, clip, ui, others = []) {
  const { daw, peaks, ValueKnob, button, toast } = ui;
  const id = clip.id;
  let current = clip, owner = track;
  const trackIndex = (t) => screen.session?.tracks?.findIndex((x) => x.id === t.id) ?? 0;
  const lanes = [{ clip, track }, ...others]
    .sort((x, y) => trackIndex(x.track) - trackIndex(y.track) || x.clip.start - y.clip.start)
    .map(({ clip: c, track: t }) => createLane(c, t));
  const primary = lanes.find((lane) => lane.clip.id === id);
  const active = () => primary;
  let editAll = prefs.get("editAll", false);
  let disposed = false;

  const element = html("section", "panel clip-editor");
  element.tabIndex = 0; // keys while it has focus (tools, Esc)

  //================================================================ left: one section per job
  const side = html("div", "clip-editor-side");
  const head = html("div", "rhythm-head");
  head.append(html("h3", "", "Pitch & time"), html("span", "panel-caption", "this clip"));
  head.title = "Non-destructive: the take stays as recorded, and every change undoes";

  // Pitch.
  const knobs = html("div", "insert-controls rhythm-knobs");
  const pitch = pitchKnobs(ValueKnob, "Pitch", clip.transpose, (v, first) => daw("setClip", { id, transpose: v, continue: !first }));
  knobs.append(...pitch.elements);
  const keyInfo = html("p", "panel-caption rhythm-key-info");
  const algorithm = algorithmControls(daw, () => [current]);
  const [algorithmRow, algorithmInfo] = algorithm.elements;

  // Hitpoints (read-only for now: where notes start, the Warp tool's snap targets).
  const hitsCount = html("p", "panel-caption");
  const sensitivity = new ValueKnob({ label: "Detect", min: 0, max: 100, step: 1, value: prefs.get("sensitivity", 50), defaultValue: 50,
                                      format: (v) => `${Math.round(v)}%`,
                                      onChange: (v) => { prefs.set("sensitivity", Math.round(v)); clearTimeout(hitsTimer); hitsTimer = setTimeout(() => lanes.forEach(fetchHits), 200); } });
  sensitivity.element.title = "How soft a note start still counts (also what Quantize uses)";
  let hitsTimer = 0;
  const hitsKnobs = html("div", "insert-controls rhythm-knobs");
  hitsKnobs.append(sensitivity.element);

  // Quantize (its settings are in the track's Pitch & time card).
  const report = (result) => {
    if (!result?.ok) return;
    toast(result.onsets ? `Quantize: ${result.moved} of ${result.onsets} notes moved onto the grid.` : "Quantize: no clear note starts found.");
  };
  const quantizeRow = html("div", "panel-actions");
  const resetTime = button("tool", "Reset", "Put the clip's timing back as recorded: every warp tab goes (its pitch stays)", () => daw("clearWarp", { clips: [id] }));
  quantizeRow.append(
    button("tool", "Quantize", "Warp this clip so its notes start on the grid (Q)",
      async () => report(await daw("quantize", { clips: [id], ...savedQuantizeArgs(screen.session, owner.id) }))),
    resetTime);

  // The Warp tool (its own section body, gestures and drawing).
  const toolLayer = html("div", "clip-editor-tools");
  const ctx = {
    screen, daw, toast, lanes, active, RULER,
    span: () => span(), width: () => width(), xOf: (time) => xOf(time), timeAt: (clientX) => timeAt(clientX),
    perPx: () => { const [a, b] = span(); return (b - a) / width(); },
    laneAt: (clientY) => laneAt(clientY), layer: toolLayer, wave: null,
    editAll: () => editAll && lanes.length > 1,
    paintSoon: () => paintSoon(), redraw: () => drawOverlay(), setStatus: (text) => setStatus(text),
  };
  const tools = { warp: createWarpTool(ctx) };

  const bodies = {
    pitch: [knobs, keyInfo, algorithmInfo],
    warp: [tools.warp.body],
    hits: [hitsKnobs, hitsCount, html("p", "panel-caption", "Note starts found in the audio. The Warp tool snaps to them; Tabs at hitpoints (Warp) pins them all.")],
    quantize: [quantizeRow, html("p", "panel-caption", "Grid, strength and swing: the track's Pitch & time card. Reset puts the timing back as recorded.")],
  };
  let open = prefs.get("tool", "pitch");
  if (!SECTIONS.some(([key]) => key === open)) open = null;
  const sectionEls = new Map();
  const toolList = html("div", "clip-tools");
  for (const [key, title, shortcut] of SECTIONS) {
    const box = html("div", "clip-tool");
    const toggle = html("button", "clip-tool-head");
    toggle.type = "button";
    toggle.append(html("span", "", title));
    if (shortcut) toggle.append(html("kbd", "key-chip", shortcut));
    toggle.title = key === "pitch" ? "The clip's pitch and how it's rendered" : `${title} tool${shortcut ? ` (${shortcut})` : ""}: open it to work on the waveform; close it (or Esc) to just look`;
    toggle.addEventListener("click", () => setOpen(open === key ? null : key));
    const body = html("div", "clip-tool-body");
    body.append(...bodies[key]);
    box.append(toggle, body);
    toolList.append(box);
    sectionEls.set(key, { box, toggle, body });
  }
  side.append(head, toolList);

  const tool = () => (open && SECTIONS.find(([key]) => key === open)?.[3] ? tools[open] : null);
  const setOpen = (key) => {
    tool()?.leave?.();
    open = key;
    prefs.set("tool", key);
    for (const [k, { box, toggle, body }] of sectionEls) {
      box.classList.toggle("is-open", k === key);
      toggle.setAttribute("aria-expanded", String(k === key));
      body.hidden = k !== key;
    }
    wave.classList.toggle("is-tool", Boolean(tool()));
    wave.dataset.tool = key ?? "none";
    hint.textContent = tool()?.hint?.() ?? HINTS[key ?? "none"];
    drawOverlay();
  };

  //================================================================ right: toolbar, overview, waveform
  const main = html("div", "clip-editor-main");
  const bar = html("div", "clip-editor-bar");

  const musical = button("tool", "♩ Musical", "Musical mode: tie the clip to bars and beats, so it stretches to the project tempo and follows tempo changes, keeping its pitch. Switching on finds the clip's tempo from its beat",
    () => daw("setClip", { id, musical: !current.musical }));
  const clipTempo = html("input", "tempo-input rhythm-clip-tempo");
  Object.assign(clipTempo, { type: "number", min: 30, max: 300, step: 0.01, title: "The clip's own tempo (BPM): set it if it was detected wrong" });
  clipTempo.addEventListener("change", () => { if (Number(clipTempo.value) > 0) daw("setClip", { id, clipTempo: Number(clipTempo.value) }); });
  const clipTempoLabel = html("label", "tempo");
  clipTempoLabel.append(clipTempo, html("span", "", "BPM"));

  // Compare: hear the clip without its pitch changes, or without its timing changes.
  const compare = html("div", "segmented clip-editor-compare");
  const comparePitch = button("segment", "Pitch", "Hear the clip without its pitch changes (transpose and note edits); its timing stays. Click again to hear them",
    () => daw("setWarpBypass", { id, bypass: !current.warpBypass }));
  const compareTime = button("segment", "Time", "Hear the clip without its warp tabs (as recorded, fitted to the same start and length); its pitch stays. Click again to hear them",
    () => daw("setWarpBypass", { id, time: !current.timeBypass }));
  const compareLabel = html("span", "clip-editor-compare-label", "Compare");
  compareLabel.title = "Hear the clip without some of its changes, to judge them";
  compare.append(compareLabel, comparePitch, compareTime);

  const solo = button("tool", "Solo", "Hear only this clip's track (click again to hear everything)", () => daw("setTrack", { id: owner.id, solo: !owner.solo }));
  const loopOf = () => {
    const loop = screen.session?.loop;
    return Boolean(loop?.on && Math.abs(loop.start - current.start) < 1e-3 && Math.abs(loop.end - (current.start + current.length)) < 1e-3);
  };
  const loop = button("tool", "Loop ▸", "Loop the clip and play it (click again to stop looping)", () => {
    if (loopOf()) return daw("loop", { on: false, start: current.start, end: current.start + current.length });
    daw("loop", { on: true, start: current.start, end: current.start + current.length });
    if (!screen.status?.playing) {
      daw("transport", { action: "seek", time: current.start });
      daw("transport", { action: "play" });
    }
  });
  const fit = button("tool", "Fit", "Show the whole clip", () => { if (mode === "notes") notesView?.fit(); else { view = null; draw(); } });
  const status = html("span", "clip-editor-status");

  // Two views of the clip: its waveform (timing) and its sung notes (pitch, note by note).
  let mode = prefs.get("view", "wave");
  const views = html("div", "segmented clip-editor-views");
  views.setAttribute("role", "tablist");
  const viewButton = (key, text, title) => {
    const b = button("segment", text, title, () => setMode(key));
    b.setAttribute("role", "tab");
    views.append(b);
    return b;
  };
  const waveTab = viewButton("wave", "Wave", "The clip's waveform: its timing, on the project's bars and beats");
  const notesTab = viewButton("notes", "Notes", "The clip's sung notes: move, straighten and shape each one (a single voice or instrument line)");
  // With several clips: does the Warp tool change the active clip, or all of them at once?
  const editSwitch = html("div", "segmented clip-editor-edit");
  const editLabel = html("span", "clip-editor-compare-label", "Edit");
  const editActive = button("segment", "Active clip", "The Warp tool changes only the active clip (the one whose name is lit)", () => setEditAll(false));
  const editEvery = button("segment", "All clips", "The Warp tool changes every clip at once: a tab goes in each clip at the same moment, and a drag moves them all together", () => setEditAll(true));
  editSwitch.append(editLabel, editActive, editEvery);
  editSwitch.hidden = lanes.length < 2;
  const setEditAll = (on) => {
    editAll = on;
    prefs.set("editAll", on);
    editActive.classList.toggle("is-selected", !on);
    editEvery.classList.toggle("is-selected", on);
    tools.warp.update();
    drawOverlay();
  };
  bar.append(views, editSwitch, musical, clipTempoLabel, algorithmRow, compare, solo, loop, status, fit);

  const overview = html("canvas", "clip-editor-overview");
  overview.title = "The whole recording: the clip's part is lit, the box is what's shown. Drag the box to scroll, its edges to zoom";
  const wave = html("div", "clip-editor-wave");
  ctx.wave = wave;
  const canvas = html("canvas", "clip-editor-canvas");
  const overlay = html("div", "clip-editor-overlay");
  const playhead = html("div", "clip-editor-playhead");
  const loopBand = html("div", "clip-editor-loop");
  const laneLabels = html("div", "clip-editor-lane-labels");
  overlay.append(loopBand, toolLayer, laneLabels, playhead);
  wave.append(canvas, overlay);
  const hint = html("p", "panel-caption clip-editor-hint");
  main.append(bar, overview, wave, hint);
  element.append(side, main);

  let notesView = null;
  const setMode = (key) => {
    mode = key === "notes" ? "notes" : "wave";
    prefs.set("view", mode);
    waveTab.classList.toggle("is-selected", mode === "wave");
    notesTab.classList.toggle("is-selected", mode === "notes");
    waveTab.setAttribute("aria-selected", String(mode === "wave"));
    notesTab.setAttribute("aria-selected", String(mode === "notes"));
    const notes = mode === "notes";
    if (notes && !notesView) {
      notesView = createNoteEditor({ screen, daw, toast, getClip: () => current, getTrack: () => owner, setStatus: (text) => setStatus(text) });
      main.append(notesView.element);
    }
    for (const item of [musical, clipTempoLabel, compareTime, overview, wave, hint]) item.hidden = notes;
    editSwitch.hidden = notes || lanes.length < 2;
    if (notesView) notesView.element.hidden = !notes;
    element.classList.toggle("is-notes", notes);
    if (notes) notesView.shown(); else requestAnimationFrame(draw);
  };

  //================================================================ the view (timeline seconds)
  let view = null; // null: every lane's clip whole
  const full = () => {
    let a = Infinity, b = -Infinity;
    for (const lane of lanes) { a = Math.min(a, lane.clip.start); b = Math.max(b, lane.clip.start + Math.max(0.01, lane.clip.length)); }
    return [a, b];
  };
  const span = () => {
    const [a, b] = full();
    if (!view) return [a, b];
    const length = clamp(view[1] - view[0], 0.05, b - a);
    const start = clamp(view[0], a, b - length);
    return [start, start + length];
  };
  const width = () => Math.max(1, wave.clientWidth);
  const timeAt = (clientX) => { const [a, b] = span(); return a + (clientX - wave.getBoundingClientRect().left) / width() * (b - a); };
  const xOf = (time) => { const [a, b] = span(); return (time - a) / (b - a) * width(); };
  const laneAt = (clientY) => {
    const y = clientY - wave.getBoundingClientRect().top - RULER;
    return lanes.find((lane) => y >= lane.top && y < lane.top + lane.height) ?? (y < 0 ? lanes[0] : lanes[lanes.length - 1]);
  };
  const layoutLanes = () => {
    const h = Math.max(1, wave.clientHeight - RULER);
    lanes.forEach((lane, i) => { lane.top = Math.round(i * h / lanes.length); lane.height = Math.round((i + 1) * h / lanes.length) - lane.top; });
  };

  //================================================================ peaks
  // Waveform peaks for the part of each lane in view, asked for again while the copy is still
  // rendering. Until the new peaks come, the old picture stays, stretched to where the edit
  // puts it (the lane's `remap`), and only the stretches still rendering are shaded.
  let frame = 0;
  const visibleRendered = (lane) => {
    const [a, b] = span();
    const c = lane.clip;
    const from = clamp(lane.toRendered(a), c.offset, c.offset + c.length), to = clamp(lane.toRendered(b), c.offset, c.offset + c.length);
    return to - from > 1e-4 ? [from, to] : null;
  };
  const fetchPeaks = async (lane) => {
    const range = visibleRendered(lane);
    if (!range) return;
    const [a, b] = span();
    const [from, to] = range;
    const c = lane.clip;
    const audioKey = c.audioKey || c.file;
    const key = `${audioKey}|${from}|${to}|${width()}|${a}|${b}`;
    if (lane.peaks?.key === key || lane.asking === key) return;
    lane.asking = key;
    const result = await peaks({ file: audioKey, clip: c.id, start: from, end: to, seconds: (b - a) / (width() * (window.devicePixelRatio || 1)) });
    if (lane.asking === key) lane.asking = null;
    if (disposed) return;
    if (result?.max) {
      lane.peaks = { key, ...result };
      lane.remap = null;
      lane.pending = [];
      setStatus();
      paint();
    } else {
      // Still decoding or rendering the new pitch/timing: see how far it has got, try again shortly.
      if (!lane.peaks) setStatus("Rendering…");
      const state = await daw("renderState", { id: c.id });
      if (disposed) return;
      lane.pending = state?.ok && !state.ready ? state.ranges ?? [] : [];
      paint();
      clearTimeout(lane.retry);
      lane.retry = setTimeout(() => { if (!disposed && element.isConnected) fetchPeaks(lane); }, state?.ready ? 150 : 400);
    }
  };
  const setStatus = (text = "") => {
    status.textContent = text;
    status.classList.toggle("is-busy", Boolean(text));
  };

  // The whole rendered copy, small, for the overview.
  let overviewPeaks = null, overviewAsking = "";
  const fetchOverview = async () => {
    const c = active().clip;
    const total = c.fileLength > 0 ? c.fileLength : c.offset + c.length;
    const w = Math.max(1, overview.clientWidth);
    const key = `${c.audioKey || c.file}|${total}|${w}`;
    if (overviewPeaks?.key === key || overviewAsking === key) return;
    overviewAsking = key;
    const result = await peaks({ file: c.audioKey || c.file, clip: c.id, start: 0, end: total, seconds: total / w });
    if (overviewAsking === key) overviewAsking = "";
    if (disposed || !result?.max) return;
    overviewPeaks = { key, total, ...result };
    paintOverview();
  };

  //================================================================ painting
  const colour = () => getComputedStyle(element).getPropertyValue("--wave").trim() || "#8c7bff";

  const paintOverview = () => {
    const ratio = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(overview.clientWidth * ratio)), h = Math.max(1, Math.round(overview.clientHeight * ratio));
    if (overview.width !== w) overview.width = w;
    if (overview.height !== h) overview.height = h;
    const g = overview.getContext("2d");
    g.clearRect(0, 0, w, h);
    const c = active().clip;
    const total = overviewPeaks?.total ?? (c.fileLength > 0 ? c.fileLength : c.offset + c.length);
    const xOfRendered = (t) => t / total * w;
    // The clip's window lit, the rest dim.
    g.fillStyle = "rgba(255,255,255,0.07)";
    g.fillRect(xOfRendered(c.offset), 0, Math.max(1, xOfRendered(c.offset + c.length) - xOfRendered(c.offset)), h);
    if (overviewPeaks) {
      g.fillStyle = colour();
      const mid = h / 2;
      for (let x = 0; x < w; ++x) {
        const i0 = Math.floor(x / w * overviewPeaks.max.length), i1 = Math.max(i0 + 1, Math.floor((x + 1) / w * overviewPeaks.max.length));
        let lo = Infinity, hi = -Infinity;
        for (let i = i0; i < Math.min(i1, overviewPeaks.max.length); ++i) { lo = Math.min(lo, overviewPeaks.min[i]); hi = Math.max(hi, overviewPeaks.max[i]); }
        if (lo > hi) continue;
        const t = x / w * total;
        g.globalAlpha = t >= c.offset && t <= c.offset + c.length ? 0.85 : 0.3;
        g.fillRect(x, mid - clamp(hi, -1, 1) * mid, 1, Math.max(ratio, (hi - lo) * mid));
      }
      g.globalAlpha = 1;
    }
    // The view box.
    const lane = active();
    const [a, b] = span();
    const x0 = xOfRendered(lane.toRendered(a)), x1 = xOfRendered(lane.toRendered(b));
    g.strokeStyle = "rgba(255,255,255,0.8)";
    g.lineWidth = ratio;
    g.strokeRect(x0 + ratio / 2, ratio / 2, Math.max(2 * ratio, x1 - x0 - ratio), h - ratio);
  };

  const paint = () => {
    const ratio = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(width() * ratio)), h = Math.max(1, Math.round(wave.clientHeight * ratio));
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    layoutLanes();
    const g = canvas.getContext("2d");
    g.clearRect(0, 0, w, h);
    const [a, b] = span();
    const perPixel = (b - a) / w;
    const ruler = RULER * ratio;

    // The ruler lane, then bars and beats from the project's tempo (the lanes share the timeline).
    g.fillStyle = "rgba(255,255,255,0.04)";
    g.fillRect(0, 0, w, ruler);
    g.fillStyle = "rgba(255,255,255,0.12)";
    g.fillRect(0, ruler - ratio, w, ratio);
    const map = screen.tempoMap, perBar = screen.beatsPerBar || 4;
    if (map) {
      const firstBeat = Math.floor(map.toBeats(Math.max(0, a))), lastBeat = Math.ceil(map.toBeats(Math.max(0, b)));
      const beatPx = w / Math.max(1e-6, lastBeat - firstBeat + 1) / ratio;
      const every = beatPx < 6 ? perBar * Math.max(1, Math.ceil(24 / (beatPx * perBar))) : 1;
      g.font = `${10 * ratio}px system-ui, sans-serif`;
      for (let beat = Math.ceil(firstBeat / every) * every; beat <= lastBeat; beat += every) {
        const x = Math.round((map.toSeconds(beat) - a) / perPixel);
        if (x < 0 || x > w) continue;
        const isBar = beat % perBar === 0;
        g.fillStyle = isBar ? "rgba(255,255,255,0.16)" : "rgba(255,255,255,0.06)";
        g.fillRect(x, ruler, ratio, h - ruler);
        g.fillStyle = isBar ? "rgba(255,255,255,0.5)" : "rgba(255,255,255,0.25)";
        g.fillRect(x, isBar ? ruler * 0.35 : ruler * 0.65, ratio, isBar ? ruler * 0.65 : ruler * 0.35);
        if (isBar || beatPx > 34) {
          g.fillStyle = isBar ? "rgba(255,255,255,0.7)" : "rgba(255,255,255,0.4)";
          g.fillText(isBar ? `${beat / perBar + 1}` : `${Math.floor(beat / perBar) + 1}.${(beat % perBar) + 1}`, x + 3 * ratio, 10 * ratio);
        }
      }
    }

    const fill = colour();
    for (const lane of lanes) {
      g.globalAlpha = lanes.length > 1 && lane !== active() ? 0.55 : 1;
      const c = lane.clip;
      const top = ruler + lane.top * ratio, laneH = lane.height * ratio;
      // Outside the clip's window: dim.
      const x0 = Math.max(0, (c.start - a) / perPixel), x1 = Math.min(w, (c.start + c.length - a) / perPixel);
      g.fillStyle = "rgba(0,0,0,0.25)";
      if (x0 > 0) g.fillRect(0, top, x0, laneH);
      if (x1 < w) g.fillRect(x1, top, w - x1, laneH);
      if (lanes.length > 1 && lane.top > 0) { g.fillStyle = "rgba(255,255,255,0.1)"; g.fillRect(0, top, w, ratio); }

      const result = lane.peaks;
      if (!result) continue;
      g.fillStyle = lane.track.colour || fill;
      const back = lane.remap ? (lane.drag ? (t) => lane.remap(lane.drag(t)) : lane.remap) : lane.drag ?? ((t) => t);
      const gain = Math.pow(10, (c.gain || 0) / 20);
      const middle = top + laneH / 2, reach = laneH / 2 - ratio;
      if (!lane.track.colour) g.fillStyle = fill;
      for (let x = Math.floor(x0); x < Math.ceil(x1); ++x) {
        // Where this column came from in the picture we have (the same place unless an edit moved it).
        const from = back(lane.toRendered(a + x * perPixel)), until = back(lane.toRendered(a + (x + 1) * perPixel));
        const t = Math.min(from, until) - result.start;
        const i0 = Math.floor(t / result.seconds), i1 = Math.max(i0 + 1, Math.floor((t + Math.abs(until - from)) / result.seconds));
        let lo = Infinity, hi = -Infinity;
        for (let i = Math.max(0, i0); i < Math.min(i1, result.max.length); ++i) {
          lo = Math.min(lo, result.min[i]);
          hi = Math.max(hi, result.max[i]);
        }
        if (lo > hi) continue;
        const yTop = middle - clamp(hi * gain, -1, 1) * reach, yBottom = middle - clamp(lo * gain, -1, 1) * reach;
        g.fillRect(x, Math.min(yTop, middle - 0.5 * ratio), 1, Math.max(ratio, yBottom - yTop));
      }

      // The stretches still rendering: shaded, so the rest can be heard and judged already.
      if (lane.pending.length) {
        g.fillStyle = "rgba(255,255,255,0.14)";
        for (const [from, to] of lane.pending) {
          const p0 = Math.max(x0, (lane.toTimeline(from) - a) / perPixel), p1 = Math.min(x1, (lane.toTimeline(to) - a) / perPixel);
          if (p1 > p0) g.fillRect(p0, top, p1 - p0, laneH);
        }
      }
    }
    g.globalAlpha = 1;
    paintOverview();
  };
  const paintSoon = () => { if (!frame) frame = requestAnimationFrame(() => { frame = 0; paint(); }); };

  //================================================================ hitpoints (read-only)
  const fetchHits = async (lane) => {
    const c = lane.clip;
    const level = prefs.get("sensitivity", 50) / 100;
    const key = `${c.audioKey}|${c.offset}|${c.length}|${JSON.stringify(c.warp ?? [])}|${level}`;
    if (key === lane.hitsKey) return;
    lane.hitsKey = key;
    const result = await daw("hitpoints", { id: c.id, sensitivity: level });
    if (disposed || lane.hitsKey !== key) return;
    if (!result?.ok || !result.hits.length) {
      // Perhaps the take is still loading: ask again in a moment (a few times).
      lane.hitsTries = (lane.hitsTries ?? 0) + 1;
      if (lane.hitsTries < 6) setTimeout(() => { if (!disposed && lane.hitsKey === key) { lane.hitsKey = ""; fetchHits(lane); } }, 1000 * lane.hitsTries);
    } else lane.hitsTries = 0;
    lane.hits = result?.hits ?? [];
    if (lane === active()) hitsCount.textContent = `${lane.hits.length} note starts in the clip.`;
    tools.warp.update();
    drawOverlay();
  };

  //================================================================ overlay: hitpoints, tabs, playhead, loop
  const hitLayer = html("div", "clip-editor-hits");
  overlay.prepend(hitLayer);
  const drawOverlay = () => {
    hitLayer.replaceChildren();
    const w = width();
    const strong = open === "hits" || open === "warp";
    for (const lane of lanes) {
      for (const [time, strength] of lane.hits) {
        const x = xOf(lane.toTimeline(time));
        if (x < 0 || x > w) continue;
        const line = html("div", "hitpoint");
        Object.assign(line.style, { left: `${x}px`, top: `${RULER + lane.top}px`, height: `${lane.height}px`, opacity: String((strong ? 0.35 : 0.18) + (strong ? 0.6 : 0.3) * strength) });
        hitLayer.append(line);
      }
    }
    tools.warp.draw(open === "warp" && !active().clip.timeBypass);
    drawLaneLabels();
    drawLoop();
    movePlayhead();
  };
  // With several lanes: each one's track name; click it to make that clip the active one.
  const drawLaneLabels = () => {
    laneLabels.replaceChildren();
    if (lanes.length < 2) return;
    for (const lane of lanes) {
      const label = html("button", `clip-editor-lane-label${lane === active() ? " is-active" : ""}`, `${lane.track.name} · ${lane.clip.name || "clip"}`);
      label.type = "button";
      label.title = lane === active() ? "The active clip: the sections on the left are its" : "Make this the active clip";
      label.style.top = `${RULER + lane.top + 2}px`;
      label.addEventListener("pointerdown", (event) => event.stopPropagation());
      label.addEventListener("click", () => { if (lane !== active()) screen.select(lane.track.id, lane.clip.id); });
      laneLabels.append(label);
    }
  };

  const drawLoop = () => {
    const loopRange = screen.session?.loop;
    const shown = loopRange && loopRange.end > loopRange.start;
    loopBand.hidden = !shown;
    if (!shown) return;
    const x0 = xOf(loopRange.start), x1 = xOf(loopRange.end);
    Object.assign(loopBand.style, { left: `${x0}px`, width: `${Math.max(1, x1 - x0)}px` });
    loopBand.classList.toggle("is-on", Boolean(loopRange.on));
  };
  const movePlayhead = () => {
    const position = screen.status?.position ?? 0;
    const x = xOf(position);
    playhead.hidden = x < 0 || x > width();
    playhead.style.transform = `translateX(${x}px)`;
  };

  const draw = () => {
    if (!wave.isConnected || mode !== "wave") return;
    paint();
    drawOverlay();
    lanes.forEach(fetchPeaks);
    fetchOverview();
  };

  const resize = new ResizeObserver(() => { for (const lane of lanes) lane.peaks = lane.peaks && { ...lane.peaks, key: "" }; overviewPeaks = overviewPeaks && { ...overviewPeaks, key: "" }; draw(); });
  resize.observe(wave);
  resize.observe(overview);

  //================================================================ pointer: scroll, zoom, locate, loop
  const setView = (next) => { view = next; view = span(); draw(); };

  // Ctrl/Cmd + scroll zooms around the pointer; scroll moves along.
  wave.addEventListener("wheel", (event) => {
    event.preventDefault();
    const [a, b] = span();
    if (event.ctrlKey || event.metaKey) {
      const at = timeAt(event.clientX), factor = Math.pow(1.0015, event.deltaY);
      setView([at - (at - a) * factor, at + (b - at) * factor]);
    } else {
      const move = (event.deltaX || event.deltaY) / width() * (b - a);
      setView([a + move, b + move]);
    }
  }, { passive: false });

  // A press with no tool taking it: in the ruler, click locates and drag marks a loop range;
  // in the waveform, click locates and drag scrolls.
  const plainPress = (event) => {
    const box = wave.getBoundingClientRect();
    const inRuler = event.clientY - box.top < RULER;
    const [a0, b0] = span();
    const startTime = timeAt(event.clientX);
    let moved = false;
    const band = html("div", "clip-editor-range");
    const move = (e) => {
      if (!moved && Math.abs(e.clientX - event.clientX) < 3) return;
      moved = true;
      if (inRuler) {
        const from = screen.snapTime(startTime, e), to = screen.snapTime(timeAt(e.clientX), e);
        if (!band.isConnected) overlay.append(band);
        Object.assign(band.style, { left: `${xOf(Math.min(from, to))}px`, width: `${Math.abs(xOf(to) - xOf(from))}px` });
      } else {
        const shift = (e.clientX - event.clientX) / width() * (b0 - a0);
        setView([a0 - shift, b0 - shift]);
      }
    };
    const up = (e) => {
      wave.removeEventListener("pointermove", move);
      wave.removeEventListener("pointerup", up);
      wave.removeEventListener("pointercancel", up);
      band.remove();
      if (!moved) return daw("transport", { action: "seek", time: Math.max(0, timeAt(e.clientX)) });
      if (inRuler) {
        const from = screen.snapTime(startTime, e), to = screen.snapTime(timeAt(e.clientX), e);
        if (Math.abs(to - from) > 0.05) daw("loop", { on: true, start: Math.min(from, to), end: Math.max(from, to) });
      }
    };
    wave.setPointerCapture?.(event.pointerId);
    wave.addEventListener("pointermove", move);
    wave.addEventListener("pointerup", up);
    wave.addEventListener("pointercancel", up);
  };

  wave.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    element.focus({ preventScroll: true });
    if (tool()?.pointerdown(event)) return;
    plainPress(event);
  });
  wave.addEventListener("pointermove", (event) => { if (!event.buttons) tool()?.hover?.(event); });
  wave.addEventListener("pointerleave", () => tool()?.leave?.());

  // The overview: drag the box to scroll, its edges to zoom, click elsewhere to centre there.
  overview.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    const lane = active(), c = lane.clip;
    const total = overviewPeaks?.total ?? (c.fileLength > 0 ? c.fileLength : c.offset + c.length);
    const w = Math.max(1, overview.clientWidth);
    const left = overview.getBoundingClientRect().left;
    const timeOfX = (clientX) => lane.toTimeline((clientX - left) / w * total);
    const [a, b] = span();
    const xa = (lane.toRendered(a) / total) * w, xb = (lane.toRendered(b) / total) * w, x = event.clientX - left;
    const grab = Math.abs(x - xa) < 6 ? "start" : Math.abs(x - xb) < 6 ? "end" : x > xa && x < xb ? "move" : "centre";
    const at = timeOfX(event.clientX);
    if (grab === "centre") setView([at - (b - a) / 2, at + (b - a) / 2]);
    const [a0, b0] = span();
    const move = (e) => {
      const now = timeOfX(e.clientX);
      if (grab === "start") setView([Math.min(now, b0 - 0.05), b0]);
      else if (grab === "end") setView([a0, Math.max(now, a0 + 0.05)]);
      else setView([a0 + now - at, b0 + now - at]);
    };
    const up = () => {
      overview.removeEventListener("pointermove", move);
      overview.removeEventListener("pointerup", up);
      overview.removeEventListener("pointercancel", up);
    };
    overview.setPointerCapture?.(event.pointerId);
    overview.addEventListener("pointermove", move);
    overview.addEventListener("pointerup", up);
    overview.addEventListener("pointercancel", up);
  });

  //================================================================ keys while the editor has focus
  element.addEventListener("keydown", (event) => {
    if (mode !== "wave" || event.target.closest?.("input, select, textarea, [role=slider], .notes-editor")) return;
    const handled = () => { event.preventDefault(); event.stopPropagation(); };
    const plain = !event.ctrlKey && !event.metaKey && !event.altKey;
    const section = plain && SECTIONS.find(([, , shortcut]) => shortcut && shortcut === event.key);
    if (section) { handled(); setOpen(open === section[0] ? null : section[0]); return; }
    if (tool()?.keydown?.(event)) { handled(); return; }
    if (event.key === "Escape" && open && open !== "pitch") { handled(); setOpen(null); }
  });

  //================================================================ the session changed
  let lastShape = "";
  const update = (t, c, more = []) => {
    owner = t;
    current = c;
    primary.clip = c;
    primary.track = t;
    for (const other of more) {
      const lane = lanes.find((l) => l.clip.id === other.clip.id);
      if (lane) { lane.clip = other.clip; lane.track = other.track; }
    }
    pitch.update(c.transpose);
    keyInfo.textContent = keyCaption(screen.session, (t.transpose || 0) + (c.transpose || 0));
    if (Math.abs(t.transpose || 0) > 1e-4) keyInfo.textContent += ` Includes the track's ${semitoneText(t.transpose)}.`;
    algorithm.update([c]);
    musical.classList.toggle("is-accent", Boolean(c.musical));
    musical.setAttribute("aria-pressed", String(Boolean(c.musical)));
    clipTempoLabel.hidden = !c.musical || mode === "notes";
    if (document.activeElement !== clipTempo) clipTempo.value = c.clipTempo ? String(Math.round(c.clipTempo * 100) / 100) : "";
    resetTime.disabled = !(c.warp?.length);

    const changesPitch = Math.abs((c.transpose || 0) + (t.transpose || 0)) > 1e-4 || Boolean(c.pitchEdited);
    comparePitch.classList.toggle("is-selected", Boolean(c.warpBypass));
    comparePitch.setAttribute("aria-pressed", String(Boolean(c.warpBypass)));
    comparePitch.disabled = !changesPitch && !c.warpBypass;
    compareTime.classList.toggle("is-selected", Boolean(c.timeBypass));
    compareTime.setAttribute("aria-pressed", String(Boolean(c.timeBypass)));
    compareTime.disabled = !(c.warp?.length) && !c.timeBypass;
    solo.classList.toggle("is-accent", Boolean(t.solo));
    loop.classList.toggle("is-accent", loopOf());

    if (t.colour) element.style.setProperty("--wave", t.colour);
    notesTab.classList.toggle("has-edits", Boolean(c.pitchEdited));
    tools.warp.update();
    hint.textContent = tool()?.hint?.() ?? HINTS[open ?? "none"];
    if (mode === "notes") notesView.update();
    const shape = lanes.map((l) => { const x = l.clip; return `${x.audioKey}|${x.start}|${x.offset}|${x.length}|${x.gain}|${JSON.stringify(x.warp ?? [])}|${x.timeBypass}`; }).join("/");
    if (shape !== lastShape) {
      lastShape = shape;
      draw();
      lanes.forEach(fetchHits);
    } else drawOverlay();
  };

  // The playhead, from the DAW's status (a few times a second); pages along while playing.
  const setPlayStatus = (s) => {
    if (mode !== "wave" || !wave.isConnected) return;
    movePlayhead();
    if (s?.playing && view && !screen.dragging) {
      const [a, b] = span();
      if (s.position > b || s.position < a - 0.05) {
        const [lo, hi] = full();
        if (s.position >= lo && s.position <= hi) setView([s.position - (b - a) * 0.05, s.position + (b - a) * 0.95]);
      }
    }
  };

  setMode(mode);
  setOpen(open);
  setEditAll(editAll);
  update(track, clip, others);
  requestAnimationFrame(draw); // sizes are known once it's on screen

  return {
    element,
    update,
    setStatus: setPlayStatus,
    dispose: () => {
      disposed = true;
      lanes.forEach((lane) => clearTimeout(lane.retry));
      clearTimeout(hitsTimer);
      cancelAnimationFrame(frame);
      resize.disconnect();
      tools.warp.dispose?.();
      notesView?.dispose();
    },
  };
}
