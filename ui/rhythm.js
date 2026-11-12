// Pitch and time for audio in the DAW screen, on the bottom panel's Track tab:
//  - the clip editor (a selected audio clip's waveform): its pitch, its algorithm, and
//    warping by dragging note starts or any moment of the clip;
//  - the track's Pitch & time card: the track's pitch, quantize, tempo from the track and
//    linked tracks.
// Everything is non-destructive: the C++ side renders a warped/transposed copy of the take
// in the background and the recording stays as it was (see src/daw/Warp.h).
import { NOTE_NAMES } from "./modules.js";
import { clamp, html } from "./widgets.js";

// How clips are stretched and transposed (see src/daw/Warp.h), best use first.
export const ALGORITHMS = [
  ["mix", "Mix", "Full mixes, chords, pads and loops: keeps harmonies clean"],
  ["solo", "Solo", "Voices and solo instruments: smooth glides and consonants"],
  ["drums", "Drums", "Drums, plucked and percussive parts: keeps hits sharp"],
  ["efficient", "Efficient", "Quick edits and background parts: much less work, a little rougher"],
  ["slice", "Slice", "No stretching: cuts at the markers and moves the pieces (gaps stay silent)"],
  ["varispeed", "Varispeed", "Like tape: time and pitch change together"],
];

const STEPS = [[1, "1/4 (beats)"], [0.5, "1/8"], [1 / 3, "1/8 triplets"], [0.25, "1/16"], ["bar", "Bars"]];

// Warp maps as the session sends them: [[source, target], ...] in seconds.
export function toSource(markers, target) {
  if (!markers?.length) return target;
  const first = markers[0], last = markers[markers.length - 1];
  if (target <= first[1]) return target - first[1] + first[0];
  if (target >= last[1]) return target - last[1] + last[0];
  for (let i = 1; i < markers.length; ++i) {
    const [s0, t0] = markers[i - 1], [s1, t1] = markers[i];
    if (target <= t1) return s0 + (target - t0) * (s1 - s0) / (t1 - t0);
  }
  return target;
}

const prefs = {
  get(key, fallback) { try { const v = localStorage.getItem(`micfx.rhythm.${key}`); return v == null ? fallback : JSON.parse(v); } catch { return fallback; } },
  set(key, value) { try { localStorage.setItem(`micfx.rhythm.${key}`, JSON.stringify(value)); } catch { /* private mode */ } },
};

const semitoneText = (v) => {
  const st = Math.round(v), cents = Math.round((v - st) * 100);
  return `${st > 0 ? "+" : ""}${st} st${cents ? ` ${cents > 0 ? "+" : ""}${cents}¢` : ""}`;
};

// A section of the Pitch & time card that folds to its title; open or closed is remembered.
function section(key, title, open, ...content) {
  const details = html("details", "rhythm-section");
  details.open = prefs.get(`open.${key}`, open);
  const summary = html("summary", "rhythm-summary", title);
  summary.title = "Show or hide";
  details.addEventListener("toggle", () => prefs.set(`open.${key}`, details.open));
  const body = html("div", "rhythm-section-body");
  body.append(...content);
  details.append(summary, body);
  return details;
}

// Pitch knobs: whole semitones and cents for one value (a track's or a clip's).
// Returns { elements, update(value) }.
function pitchKnobs(ValueKnob, label, value, send) {
  let current = value || 0;
  const st = new ValueKnob({ label, min: -24, max: 24, step: 1, bipolar: true, value: Math.round(current), defaultValue: 0,
                             format: (v) => `${v > 0 ? "+" : ""}${Math.round(v)} st`,
                             onChange: (v, first) => { current = Math.round(v) + (current - Math.round(current)); send(current, first); } });
  const fine = new ValueKnob({ label: "Fine", min: -50, max: 50, step: 1, bipolar: true, value: Math.round((current - Math.round(current)) * 100), defaultValue: 0,
                               format: (v) => `${v > 0 ? "+" : ""}${Math.round(v)}¢`,
                               onChange: (v, first) => { current = Math.round(current) + Math.round(v) / 100; send(current, first); } });
  st.element.title = "Semitones up or down. Drag, scroll or type a value; double-click to reset";
  fine.element.title = "Cents (hundredths of a semitone), for tuning. Double-click to reset";
  return {
    elements: [st.element, fine.element],
    update: (v) => {
      current = v || 0;
      if (!st.dragging) st.update(Math.round(current));
      if (!fine.dragging) fine.update(Math.round((current - Math.round(current)) * 100));
    },
  };
}

// Where a transpose takes the song's key, for the caption under the pitch knobs.
function keyCaption(session, shift) {
  const key = session.key?.key ?? -1;
  if (key < 0) return "The song's key isn't set (Key button, top bar). Pitch works without it.";
  const name = (k) => `${NOTE_NAMES[((k % 12) + 12) % 12]}${session.key.minor ? " minor" : " major"}`;
  const semis = Math.round(shift);
  if (Math.abs(shift) < 0.005) return `In the song's key, ${name(key)}.`;
  const cents = Math.round((shift - semis) * 100);
  return `Sounds in ${name(key + semis)}${cents ? ` (${cents > 0 ? "+" : ""}${cents}¢)` : ""}; the song is in ${name(key)}.`;
}

// The algorithm picker and "Keep formants", for some clips (one, or every clip of a track).
function algorithmControls(daw, clipsOf) {
  const algorithm = html("select", "dropdown");
  algorithm.title = "How the audio is stretched and transposed";
  for (const [key, name, about] of ALGORITHMS) {
    const option = new Option(name, key);
    option.title = about;
    algorithm.append(option);
  }
  const info = html("p", "panel-caption rhythm-algorithm-info");
  algorithm.addEventListener("change", () => {
    clipsOf().forEach((c, i) => daw("setClip", { id: c.id, stretch: algorithm.value, continue: i > 0 }));
    info.textContent = ALGORITHMS.find(([key]) => key === algorithm.value)?.[2] ?? "";
  });

  const formant = html("label", "rhythm-check");
  const box = html("input");
  box.type = "checkbox";
  box.addEventListener("change", () => clipsOf().forEach((c, i) => daw("setClip", { id: c.id, formant: box.checked, continue: i > 0 })));
  formant.append(box, document.createTextNode(" Keep formants"));

  const row = html("div", "panel-actions rhythm-algorithm-row");
  row.append(algorithm, formant);
  return {
    elements: [row, info],
    update: (shown) => {
      const keys = [...new Set(shown.map((x) => x.stretch ?? "mix"))];
      algorithm.value = keys.length === 1 ? keys[0] : "mix";
      algorithm.disabled = !shown.length;
      info.textContent = keys.length > 1 ? "Clips here use different algorithms: picking one sets them all."
                                         : ALGORITHMS.find(([key]) => key === algorithm.value)?.[2] ?? "";
      box.checked = shown.length > 0 && shown.every((x) => x.formant);
      box.indeterminate = shown.some((x) => x.formant) && !box.checked;
      formant.title = algorithm.value === "efficient" ? "Efficient doesn't keep formants: pick Mix, Solo or Drums for that"
                                                      : "Transpose a voice without the chipmunk or giant sound: its character stays where it was";
      box.disabled = !shown.length || algorithm.value === "efficient";
    },
  };
}

// The clip editor: a selected audio clip's waveform, big, with its pitch on the left.
// Drag a note start (faint line) to move just that note, drag anywhere else to stretch,
// double-click a marker to remove it. Ctrl/Cmd + scroll zooms, scroll moves along.
// `ui` carries what the DAW screen shares: daw, peaks (no error toasts), ValueKnob,
// button, toast.
export function createClipEditor(screen, track, clip, ui) {
  const { daw, peaks, ValueKnob, button, toast } = ui;
  const id = clip.id;
  let current = clip, owner = track;
  const element = html("section", "panel clip-editor");

  // Left: the clip's pitch and how it's rendered.
  const side = html("div", "clip-editor-side");
  const head = html("div", "rhythm-head");
  head.append(html("h3", "", "Pitch & time"), html("span", "panel-caption", "this clip"));
  head.title = "Non-destructive: the take stays as recorded, and every change undoes";
  const knobs = html("div", "insert-controls rhythm-knobs");
  const pitch = pitchKnobs(ValueKnob, "Pitch", clip.transpose, (v, first) => daw("setClip", { id, transpose: v, continue: !first }));
  knobs.append(...pitch.elements);
  const keyInfo = html("p", "panel-caption rhythm-key-info");
  const algorithm = algorithmControls(daw, () => [current]);
  side.append(head, knobs, keyInfo, ...algorithm.elements);

  // Right: tools above the waveform.
  const main = html("div", "clip-editor-main");
  const bar = html("div", "clip-editor-bar");
  const report = (result) => {
    if (!result?.ok) return;
    toast(result.onsets ? `Quantize: ${result.moved} of ${result.onsets} notes moved onto the grid.` : "Quantize: no clear note starts found.");
  };
  const quantize = button("tool", "Quantize", "Warp this clip so its notes start on the grid (settings in the track's Pitch & time card) (Q)",
    async () => report(await daw("quantize", { clips: [id], ...savedQuantizeArgs(screen.session, owner.id) })));
  const unwarp = button("tool", "Unwarp", "Put the clip's timing back as recorded (its pitch stays)", () => daw("clearWarp", { clips: [id] }));

  const musical = html("label", "rhythm-check");
  const musicalBox = html("input");
  musicalBox.type = "checkbox";
  musicalBox.addEventListener("change", () => daw("setClip", { id, musical: musicalBox.checked }));
  musical.append(musicalBox, document.createTextNode(" Follow tempo"));
  musical.title = "Musical mode: tie the clip to bars and beats, so it stretches to the project tempo and follows tempo changes, keeping its pitch. Switching on finds the clip's tempo from its beat";
  const clipTempo = html("input", "tempo-input rhythm-clip-tempo");
  Object.assign(clipTempo, { type: "number", min: 30, max: 300, step: 0.01, title: "The clip's own tempo (BPM): set it if it was detected wrong" });
  clipTempo.addEventListener("change", () => { if (Number(clipTempo.value) > 0) daw("setClip", { id, clipTempo: Number(clipTempo.value) }); });
  const clipTempoLabel = html("label", "tempo");
  clipTempoLabel.append(clipTempo, html("span", "", "BPM"));

  const fit = button("tool", "Fit", "Show the whole clip", () => { view = null; draw(); });
  const status = html("span", "clip-editor-status");
  bar.append(quantize, unwarp, musical, clipTempoLabel, status, fit);

  const wave = html("div", "clip-editor-wave");
  wave.title = "Drag a note start (faint line) to move just that note, or anywhere to stretch. Double-click a marker to remove it. Ctrl/Cmd + scroll to zoom";
  const canvas = html("canvas", "clip-editor-canvas");
  const overlay = html("div", "clip-editor-overlay");
  wave.append(canvas, overlay);
  const hint = html("p", "panel-caption clip-editor-hint");
  main.append(bar, wave, hint);
  element.append(side, main);

  // The view: a stretch of the clip's rendered time (offset .. offset + length).
  let view = null; // null: the whole clip
  const span = () => {
    const a = current.offset, b = current.offset + Math.max(0.01, current.length);
    if (!view) return [a, b];
    const length = clamp(view[1] - view[0], 0.05, b - a);
    const start = clamp(view[0], a, b - length);
    return [start, start + length];
  };
  const width = () => Math.max(1, wave.clientWidth);
  const timeAt = (clientX) => { const [a, b] = span(); return a + (clientX - wave.getBoundingClientRect().left) / width() * (b - a); };
  const xOf = (time) => { const [a, b] = span(); return (time - a) / (b - a) * width(); };

  // Waveform peaks for the view, asked for again while the copy is still rendering.
  let shownPeaks = null, asking = null, retry = 0, disposed = false;
  const fetchPeaks = async () => {
    const [a, b] = span();
    const audioKey = current.audioKey || current.file;
    const key = `${audioKey}|${a}|${b}|${width()}`;
    if (shownPeaks?.key === key || asking === key) return;
    asking = key;
    const result = await peaks({ file: audioKey, clip: id, start: a, end: b, seconds: (b - a) / (width() * (window.devicePixelRatio || 1)) });
    if (asking === key) asking = null;
    if (disposed) return;
    if (result?.max) {
      shownPeaks = { key, ...result };
      setStatus();
      paint();
    } else {
      // Still decoding or rendering the new pitch/timing: try again shortly.
      setStatus("Rendering…");
      clearTimeout(retry);
      retry = setTimeout(() => { if (!disposed && element.isConnected) fetchPeaks(); }, 400);
    }
  };
  const setStatus = (text = "") => {
    status.textContent = text;
    status.classList.toggle("is-busy", Boolean(text));
  };

  const paint = () => {
    const ratio = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(width() * ratio)), h = Math.max(1, Math.round(wave.clientHeight * ratio));
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    const context = canvas.getContext("2d");
    context.clearRect(0, 0, w, h);
    const [a, b] = span();
    const colour = getComputedStyle(element).getPropertyValue("--wave").trim() || "#8c7bff";

    // A light time ruler: seconds from the clip's start on the timeline.
    const ruler = 14 * ratio;
    const perPixel = (b - a) / w;
    const steps = [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60];
    const step = steps.find((s) => s / perPixel > 70 * ratio) ?? 120;
    context.fillStyle = "rgba(255,255,255,0.45)";
    context.font = `${10 * ratio}px system-ui, sans-serif`;
    for (let t = Math.ceil((a - current.offset) / step) * step; t + current.offset <= b; t += step) {
      const x = Math.round((t + current.offset - a) / perPixel);
      context.fillRect(x, 0, ratio, ruler * 0.5);
      context.fillText(step < 1 ? t.toFixed(step < 0.1 ? 2 : 1) : String(Math.round(t)), x + 3 * ratio, 10 * ratio);
    }

    const result = shownPeaks;
    if (!result) return;
    const gain = Math.pow(10, (current.gain || 0) / 20);
    const middle = ruler + (h - ruler) / 2, reach = (h - ruler) / 2 - ratio;
    context.fillStyle = colour;
    for (let x = 0; x < w; ++x) {
      const t = a + x * perPixel - result.start;
      const i0 = Math.floor(t / result.seconds), i1 = Math.max(i0 + 1, Math.floor((t + perPixel) / result.seconds));
      let lo = Infinity, hi = -Infinity;
      for (let i = Math.max(0, i0); i < Math.min(i1, result.max.length); ++i) {
        lo = Math.min(lo, result.min[i]);
        hi = Math.max(hi, result.max[i]);
      }
      if (lo > hi) continue;
      const top = middle - clamp(hi * gain, -1, 1) * reach, bottom = middle - clamp(lo * gain, -1, 1) * reach;
      context.fillRect(x, Math.min(top, middle - 0.5 * ratio), 1, Math.max(ratio, bottom - top));
    }
  };

  // Note starts (hitpoints), found in the background for the clip as it plays now.
  let hits = [], hitsKey = "";
  const fetchHits = async () => {
    const sensitivity = prefs.get("sensitivity", 50) / 100;
    const key = `${current.audioKey}|${current.offset}|${current.length}|${JSON.stringify(current.warp ?? [])}|${sensitivity}`;
    if (key === hitsKey) return;
    hitsKey = key;
    const result = await daw("hitpoints", { id, sensitivity });
    if (disposed || hitsKey !== key || !result?.ok) return;
    hits = result.hits;
    drawOverlay();
  };

  const drawOverlay = () => {
    overlay.replaceChildren();
    const w = width();
    for (const [time, strength] of hits) {
      const x = xOf(time);
      if (x < 0 || x > w) continue;
      const line = html("div", "hitpoint");
      line.style.left = `${x}px`;
      line.style.opacity = String(0.35 + 0.65 * strength);
      overlay.append(line);
    }
    for (const [, target] of current.warp ?? []) {
      const x = xOf(target);
      if (x < 0 || x > w) continue;
      const marker = html("div", "warp-marker");
      marker.style.left = `${x}px`;
      overlay.append(marker);
    }
  };

  const draw = () => {
    if (!wave.isConnected) return;
    paint();
    drawOverlay();
    fetchPeaks();
  };

  const resize = new ResizeObserver(() => { shownPeaks = shownPeaks && { ...shownPeaks, key: "" }; draw(); });
  resize.observe(wave);

  // Ctrl/Cmd + scroll zooms around the pointer; scroll moves along the clip.
  wave.addEventListener("wheel", (event) => {
    event.preventDefault();
    const [a, b] = span();
    if (event.ctrlKey || event.metaKey) {
      const at = timeAt(event.clientX), factor = Math.pow(1.0015, event.deltaY);
      view = [at - (at - a) * factor, at + (b - at) * factor];
    } else {
      const move = (event.deltaX || event.deltaY) / width() * (b - a);
      view = [a + move, b + move];
    }
    view = span(); // clamped
    draw();
  }, { passive: false });

  // Warping: grab a marker within 6 px, a note start within 8 px (pinning the notes either
  // side so only it moves), or pin a new marker where you press (holding the clip's edges
  // the first time). Double-click a marker to remove it.
  wave.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    const [a, b] = span();
    const perPx = (b - a) / width();
    const markers = (current.warp ?? []).map((m) => [...m]);
    const at = timeAt(event.clientX);
    const near = (t, px) => Math.abs(t - at) / perPx < px;

    if (event.detail >= 2) {
      const hit = markers.findIndex(([, t]) => near(t, 6));
      if (hit >= 0) {
        markers.splice(hit, 1);
        daw("setWarp", { id, markers: markers.length > 1 ? markers : [] });
      }
      return;
    }

    let index = markers.findIndex(([, t]) => near(t, 6));
    if (index < 0) {
      if (!markers.length) {
        const s = current.offset, e = current.offset + current.length;
        markers.push([s, s], [e, e]);
      }
      const times = hits.map(([t]) => t);
      const hit = times.findIndex((t) => near(t, 8));
      const grabbed = hit >= 0 ? times[hit] : at;
      const pin = (t) => {
        if (t === undefined || markers.some(([, m]) => Math.abs(m - t) / perPx < 3)) return;
        markers.push([toSource(markers, t), t]);
      };
      if (hit >= 0) { pin(times[hit - 1]); pin(times[hit + 1]); }
      markers.sort((x, y) => x[0] - y[0]);
      markers.push([toSource(markers, grabbed), grabbed]);
      markers.sort((x, y) => x[0] - y[0]);
      index = markers.findIndex(([, t]) => t === grabbed);
    }

    const startTarget = markers[index][1];
    const low = index > 0 ? markers[index - 1][1] + 0.01 : -Infinity;
    const high = index < markers.length - 1 ? markers[index + 1][1] - 0.01 : Infinity;
    const line = html("div", "warp-marker is-dragging");
    overlay.append(line);
    const place = (target) => { line.style.left = `${xOf(target)}px`; };
    place(startTarget);
    let target = startTarget;
    screen.dragging = true;
    wave.setPointerCapture?.(event.pointerId);

    const move = (e) => {
      const free = startTarget + (e.clientX - event.clientX) * perPx;
      const timeline = current.start + (free - current.offset);           // where it lands in the song
      const snapped = screen.snapTime(timeline, e) - current.start + current.offset;
      target = clamp(snapped, low, high);
      place(target);
    };
    const up = () => {
      wave.removeEventListener("pointermove", move);
      wave.removeEventListener("pointerup", up);
      wave.removeEventListener("pointercancel", up);
      screen.dragging = false;
      line.remove();
      if (Math.abs(target - startTarget) > 1e-4) {
        markers[index][1] = target;
        daw("setWarp", { id, markers });
      }
      screen.flushSession?.();
    };
    wave.addEventListener("pointermove", move);
    wave.addEventListener("pointerup", up);
    wave.addEventListener("pointercancel", up);
  });

  let lastShape = "";
  const update = (t, c) => {
    owner = t;
    current = c;
    pitch.update(c.transpose);
    keyInfo.textContent = keyCaption(screen.session, (t.transpose || 0) + (c.transpose || 0));
    if (Math.abs(t.transpose || 0) > 1e-4) keyInfo.textContent += ` Includes the track's ${semitoneText(t.transpose)}.`;
    algorithm.update([c]);
    musicalBox.checked = Boolean(c.musical);
    clipTempoLabel.hidden = !c.musical;
    if (document.activeElement !== clipTempo) clipTempo.value = c.clipTempo ? String(Math.round(c.clipTempo * 100) / 100) : "";
    unwarp.disabled = !(c.warp?.length);
    const count = c.warp?.length ?? 0;
    hint.textContent = count ? `${count} warp markers. Drag a note start or a marker to move it; double-click a marker to remove it.`
                             : "Drag a note start (faint line) to move just that note, or anywhere in the clip to stretch it.";
    if (t.colour) element.style.setProperty("--wave", t.colour);
    const shape = `${c.audioKey}|${c.offset}|${c.length}|${c.gain}|${JSON.stringify(c.warp ?? [])}`;
    if (shape !== lastShape) {
      lastShape = shape;
      draw();
      fetchHits();
    }
  };
  update(track, clip);
  requestAnimationFrame(draw); // sizes are known once it's on screen

  return {
    element,
    update,
    dispose: () => { disposed = true; clearTimeout(retry); resize.disconnect(); },
  };
}

// The track's "Pitch & time" card on the bottom panel's Track tab: the track's pitch,
// Quantize, Tempo and Linked tracks side by side, each folding away to its title. The
// selected clip's own pitch and warping are in the clip editor.
export function createRhythmPanel(screen, track, ui) {
  const { daw, ValueKnob, button, toast } = ui;
  const element = html("section", "panel rhythm-panel");
  const head = html("div", "rhythm-head");
  head.append(html("h3", "", "Pitch & time"), html("span", "panel-caption", "whole track"));
  head.title = "Non-destructive: the takes stay as recorded, and every change undoes";
  const sections = html("div", "rhythm-sections");
  element.append(head, sections);

  // Pitch: the track's transpose, and how every clip on it is rendered.
  let owner = track;
  const knobs = html("div", "insert-controls rhythm-knobs");
  const pitch = pitchKnobs(ValueKnob, "Pitch", track.transpose, (v, first) => daw("setTrack", { id: owner.id, transpose: v, continue: !first }));
  knobs.append(...pitch.elements);
  const keyInfo = html("p", "panel-caption rhythm-key-info");
  const audioClips = () => owner.clips.filter((c) => !c.midi);
  const algorithm = algorithmControls(daw, audioClips);
  sections.append(section("transpose", "Pitch", true, knobs, keyInfo, ...algorithm.elements));

  // Quantize.
  const grid = html("select", "dropdown");
  grid.title = "What to quantize to: the project's tempo grid, or the groove of another track";
  grid.append(new Option("Project grid", -1));
  for (const other of screen.session.tracks) if (other.id !== track.id && other.clips.length) grid.append(new Option(`Groove of ${other.name}`, other.id));
  grid.value = String(prefs.get("groove", -1));
  if (grid.selectedIndex < 0) grid.value = "-1";
  grid.addEventListener("change", () => prefs.set("groove", Number(grid.value)));

  const steps = html("select", "dropdown");
  steps.title = "Note value: the grid lines onsets snap to";
  STEPS.forEach(([v, t]) => steps.append(new Option(t, v)));
  steps.value = String(prefs.get("steps", 0.5));
  if (steps.selectedIndex < 0) steps.value = "0.5";
  steps.addEventListener("change", () => prefs.set("steps", steps.value));
  const gridRow = html("div", "panel-actions rhythm-grid-row");
  gridRow.append(grid, steps);

  const percent = (key, label, fallback, title) => {
    const knob = new ValueKnob({ label, min: 0, max: 100, step: 1, value: prefs.get(key, fallback), defaultValue: fallback,
                                 format: (v) => `${Math.round(v)}%`, onChange: (v) => prefs.set(key, Math.round(v)) });
    knob.element.title = title;
    return knob;
  };
  const strength = percent("strength", "Strength", 100, "How far onsets move towards the grid (less keeps some feel)");
  const swing = percent("swing", "Swing", 0, "Delay every second line, up to a triplet feel");
  const sensitivity = percent("sensitivity", "Detect", 50, "How soft a note start still counts (also the note starts shown in the clip editor)");
  const quantizeKnobs = html("div", "insert-controls rhythm-knobs");
  quantizeKnobs.append(strength.element, swing.element, sensitivity.element);

  const option = (key, text, title) => {
    const label = html("label", "rhythm-check");
    const box = html("input");
    Object.assign(box, { type: "checkbox", checked: prefs.get(key, false) });
    box.addEventListener("change", () => prefs.set(key, box.checked));
    label.append(box, document.createTextNode(` ${text}`));
    label.title = title;
    return label;
  };
  const quantizeOptions = html("div", "panel-actions");
  quantizeOptions.append(
    option("iterative", "Iterative", "Each press moves the notes from where they are now: at 50%, two presses close three quarters of the gap"),
    option("slice", "Slice", "Cut at the notes and move the pieces instead of stretching (best for hard drums; gaps stay silent)"));

  const quantizeRow = html("div", "panel-actions");
  quantizeRow.append(
    button("tool", "Quantize track", "Warp every clip on this track so its notes start on the grid",
      async () => {
        const result = await daw("quantize", { track: owner.id, ...savedQuantizeArgs(screen.session, owner.id) });
        if (result?.ok) toast(result.onsets ? `Quantize ${owner.name}: ${result.moved} of ${result.onsets} notes moved onto the grid.` : `Quantize ${owner.name}: no clear note starts found.`);
      }),
    button("tool", "Unwarp track", "Put this track's clips back as recorded (time only; pitch stays)", () => daw("clearWarp", { track: owner.id })));

  // Tempo from this track.
  const lineUp = html("label", "rhythm-check");
  const lineUpBox = html("input");
  Object.assign(lineUpBox, { type: "checkbox", checked: prefs.get("lineUp", true) });
  lineUpBox.addEventListener("change", () => prefs.set("lineUp", lineUpBox.checked));
  lineUp.append(lineUpBox, document.createTextNode(" Line the song up with the bars"));
  lineUp.title = "Slide every clip (together) so this track's downbeats fall on bar lines";
  const tempoRow = html("div", "panel-actions");
  tempoRow.append(button("tool", "Tempo from this track", "Find this track's tempo and beats, and use it as the project tempo",
    async () => {
      const result = await daw("detectTempo", { track: owner.id, apply: true, lineUp: lineUpBox.checked });
      if (!result?.ok) return;
      const moved = Math.abs(result.shift) > 0.0005 ? `, song moved ${result.shift > 0 ? "later" : "earlier"} by ${Math.abs(result.shift).toFixed(3)} s` : "";
      toast(`Tempo ${result.bpm.toFixed(2)} BPM from ${owner.name}${moved}.${result.confidence < 0.25 ? " The beat wasn't very clear: check it with the click." : ""}`);
    }));
  const tempoInfo = html("p", "panel-caption", "For a track that sets the beat (drums, a guitar, a loop). To make one clip follow the tempo, tick Follow tempo in its clip editor.");

  // Linked tracks: recorded together (drum mics, a split stereo pair), warped as one.
  const linkSelect = html("select", "dropdown");
  linkSelect.title = "Link this track with another recorded at the same time: their clips warp and quantize together, in phase";
  const linkInfo = html("p", "panel-caption");
  const unlink = button("tool", "Unlink", "Warp this track on its own again", () => daw("unlinkTrack", { track: owner.id }));
  linkSelect.addEventListener("change", async () => {
    const other = Number(linkSelect.value);
    linkSelect.value = "";
    if (other) await daw("linkTracks", { tracks: [owner.id, other] });
  });
  const linkRow = html("div", "panel-actions");
  linkRow.append(linkSelect, unlink);

  sections.append(section("quantize", "Quantize", true, gridRow, quantizeKnobs, quantizeOptions, quantizeRow),
                  section("tempo", "Tempo", true, tempoRow, lineUp, tempoInfo),
                  section("link", "Linked tracks", false, linkRow, linkInfo));

  const update = (t) => {
    owner = t;
    pitch.update(t.transpose);
    keyInfo.textContent = keyCaption(screen.session, t.transpose || 0);
    algorithm.update(audioClips());
    const linked = screen.session.tracks.filter((o) => t.link && o.link === t.link && o.id !== t.id);
    const candidates = screen.session.tracks.filter((o) => o.id !== t.id && o.kind !== "midi" && !(t.link && o.link === t.link));
    linkSelect.replaceChildren(new Option(linked.length ? "Link another track…" : "Link with track…", ""),
                               ...candidates.map((o) => new Option(o.name, o.id)));
    linkSelect.disabled = t.kind === "midi" || !candidates.length;
    unlink.hidden = !t.link;
    linkInfo.textContent = linked.length
      ? `Linked with ${linked.map((o) => o.name).join(", ")}. Clips that start together warp as one, rendered together so the mics stay in phase.`
      : "For tracks recorded at the same time, like drum mics: warp one and the others follow, sample for sample.";
  };
  update(track);
  return { element, update };
}

// Draws a clip's warp markers in the arrangement (where its audio has been pinned in time).
export function drawWarpMarkers(element, clip, zoom) {
  element.querySelectorAll(".warp-marker").forEach((m) => m.remove());
  for (const [index, [, target]] of (clip.warp ?? []).entries()) {
    const x = (target - clip.offset) * zoom;
    if (x < 0 || x > clip.length * zoom) continue;
    const marker = html("div", "warp-marker");
    marker.style.left = `${x}px`;
    marker.dataset.index = index;
    element.append(marker);
  }
}

// The quantize settings last chosen in the "Pitch & time" panel, for the Q shortcut.
export function savedQuantizeArgs(session, trackId) {
  const steps = prefs.get("steps", 0.5);
  let groove = Number(prefs.get("groove", -1));
  const leader = session.tracks.find((t) => t.id === groove);
  if (groove === trackId || !leader?.clips.length) groove = -1;
  return {
    steps: steps === "bar" ? session.beatsPerBar ?? 4 : Number(steps) || 0.5,
    strength: prefs.get("strength", 100) / 100,
    swing: prefs.get("swing", 0) / 100,
    sensitivity: prefs.get("sensitivity", 50) / 100,
    iterative: prefs.get("iterative", false),
    slice: prefs.get("slice", false),
    groove,
  };
}
