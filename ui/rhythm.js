// Pitch and time for audio in the DAW screen: per-track and per-clip transpose, tempo
// from a leading track, quantize to the grid or to a groove, and the warp tool (drag
// inside a clip to stretch it). Everything is non-destructive: the C++ side renders a
// warped copy of the take and the recording stays as it was (see src/daw/Warp.h).
import { html } from "./widgets.js";

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

// The "Pitch & time" card for a track (and its selected clip), on the bottom panel's Track
// tab: Transpose, Quantize and Tempo side by side, each folding away to its title.
// `ui` carries what the DAW screen shares: daw (command, args), ValueKnob, button, toast.
export function createRhythmPanel(screen, track, clip, ui) {
  const { daw, ValueKnob, button, toast } = ui;
  const element = html("section", "panel rhythm-panel");
  const head = html("div", "rhythm-head");
  head.append(html("h3", "", "Pitch & time"),
              html("span", "panel-caption", "Non-destructive: the take stays as recorded, and every change undoes."));
  const sections = html("div", "rhythm-sections");
  element.append(head, sections);

  // Transpose: whole semitones and cents, for the track and for the selected clip.
  const knobs = html("div", "insert-controls rhythm-knobs");
  const transposeKnobs = (label, value, send) => {
    let current = value || 0;
    const st = new ValueKnob({ label, min: -24, max: 24, step: 1, bipolar: true, value: Math.round(current), defaultValue: 0,
                               format: (v) => `${v > 0 ? "+" : ""}${Math.round(v)} st`,
                               onChange: (v, first) => { current = Math.round(v) + (current - Math.round(current)); send(current, first); } });
    const fine = new ValueKnob({ label: "Fine", min: -50, max: 50, step: 1, bipolar: true, value: Math.round((current - Math.round(current)) * 100), defaultValue: 0,
                                 format: (v) => `${v > 0 ? "+" : ""}${Math.round(v)}¢`,
                                 onChange: (v, first) => { current = Math.round(current) + Math.round(v) / 100; send(current, first); } });
    knobs.append(st.element, fine.element);
    return (v) => { current = v || 0; st.update(Math.round(current)); fine.update(Math.round((current - Math.round(current)) * 100)); };
  };
  const updateTrack = transposeKnobs("Track key", track.transpose,
    (v, first) => daw("setTrack", { id: track.id, transpose: v, continue: !first }));
  const updateClip = clip ? transposeKnobs("Clip key", clip.transpose,
    (v, first) => daw("setClip", { id: clip.id, transpose: v, continue: !first })) : null;
  // Algorithm and formants: for the selected clip, or every audio clip on the track.
  const audioClips = () => (clip ? [clip] : track.clips.filter((c) => !c.midi));
  const algorithm = html("select", "dropdown");
  algorithm.title = "How the audio is stretched and transposed";
  for (const [key, name, about] of ALGORITHMS) {
    const option = new Option(name, key);
    option.title = about;
    algorithm.append(option);
  }
  algorithm.addEventListener("change", () => {
    audioClips().forEach((c, i) => daw("setClip", { id: c.id, stretch: algorithm.value, continue: i > 0 }));
    algorithmInfo.textContent = ALGORITHMS.find(([key]) => key === algorithm.value)?.[2] ?? "";
  });
  const algorithmInfo = html("p", "panel-caption");

  const formant = html("label", "rhythm-check");
  const formantBox = html("input");
  formantBox.type = "checkbox";
  formantBox.addEventListener("change", () =>
    audioClips().forEach((c, i) => daw("setClip", { id: c.id, formant: formantBox.checked, continue: i > 0 })));
  formant.append(formantBox, document.createTextNode(" Keep formants"));
  formant.title = "Transpose a voice without the chipmunk or giant sound: its character stays where it was";

  const algorithmRow = html("div", "panel-actions");
  algorithmRow.append(algorithm, formant);
  sections.append(section("transpose", "Transpose", true, knobs, algorithmRow, algorithmInfo));

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

  const percent = (key, label, fallback, title) => {
    const knob = new ValueKnob({ label, min: 0, max: 100, step: 1, value: prefs.get(key, fallback), defaultValue: fallback,
                                 format: (v) => `${Math.round(v)}%`, onChange: (v) => prefs.set(key, Math.round(v)) });
    knob.element.title = title;
    return knob;
  };
  const strength = percent("strength", "Strength", 100, "How far onsets move towards the grid (less keeps some feel)");
  const swing = percent("swing", "Swing", 0, "Delay every second line, up to a triplet feel");
  const quantizeKnobs = html("div", "insert-controls rhythm-knobs");
  quantizeKnobs.append(strength.element, swing.element);

  const quantizeArgs = () => {
    const value = steps.value === "bar" ? screen.session.beatsPerBar ?? 4 : Number(steps.value);
    return { steps: value, strength: strength.value / 100, swing: swing.value / 100, groove: Number(grid.value) };
  };
  const report = (result, what) => {
    if (!result?.ok) return;
    toast(result.onsets ? `${what}: ${result.moved} of ${result.onsets} notes moved onto the grid.` : `${what}: no clear note starts found.`);
  };

  const quantizeRow = html("div", "panel-actions");
  if (clip) quantizeRow.append(button("tool", "Quantize clip", "Warp the selected clip so its notes start on the grid",
    async () => report(await daw("quantize", { clips: [clip.id], ...quantizeArgs() }), "Quantize")));
  quantizeRow.append(
    button("tool", "Quantize track", "Warp every clip on this track so its notes start on the grid",
      async () => report(await daw("quantize", { track: track.id, ...quantizeArgs() }), `Quantize ${track.name}`)),
    button("tool", "Unwarp", clip ? "Put the selected clip back as recorded (time only)" : "Put this track's clips back as recorded (time only)",
      () => daw("clearWarp", clip ? { clips: [clip.id] } : { track: track.id })));

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
      const result = await daw("detectTempo", { track: track.id, apply: true, lineUp: lineUpBox.checked });
      if (!result?.ok) return;
      const moved = Math.abs(result.shift) > 0.0005 ? `, song moved ${result.shift > 0 ? "later" : "earlier"} by ${Math.abs(result.shift).toFixed(3)} s` : "";
      toast(`Tempo ${result.bpm.toFixed(2)} BPM from ${track.name}${moved}.${result.confidence < 0.25 ? " The beat wasn't very clear: check it with the click." : ""}`);
    }), lineUp);

  const warpInfo = html("p", "panel-caption rhythm-warp-info");
  sections.append(section("quantize", "Quantize", true, grid, steps, quantizeKnobs, quantizeRow, warpInfo),
                  section("tempo", "Tempo", true, tempoRow));

  const update = (t, c) => {
    updateTrack(t.transpose);
    const shown = c ? [c] : t.clips.filter((x) => !x.midi);
    const keys = [...new Set(shown.map((x) => x.stretch ?? "mix"))];
    algorithm.value = keys.length === 1 ? keys[0] : "mix";
    algorithm.disabled = formantBox.disabled = !shown.length;
    algorithmInfo.textContent = keys.length > 1 ? "Clips on this track use different algorithms: picking one sets them all."
                                                : ALGORITHMS.find(([key]) => key === algorithm.value)?.[2] ?? "";
    formantBox.checked = shown.length > 0 && shown.every((x) => x.formant);
    formantBox.indeterminate = shown.some((x) => x.formant) && !formantBox.checked;
    formant.title = algorithm.value === "efficient" ? "Efficient doesn't keep formants: pick Mix, Solo or Drums for that"
                                                    : "Transpose a voice without the chipmunk or giant sound: its character stays where it was";
    formantBox.disabled = formantBox.disabled || algorithm.value === "efficient";
    if (c && updateClip) updateClip(c.transpose);
    const count = c?.warp?.length ?? 0;
    warpInfo.textContent = c ? (count ? `Clip warped: ${count} markers. Warp tool (W): drag in a clip to stretch, double-click a marker to remove it.`
                                      : "Warp tool (W): drag in a clip to stretch time between markers.")
                             : "Select a clip to transpose, quantize or warp just that clip.";
  };
  update(track, clip);
  return { element, update };
}

// Draws a clip's warp markers (where its audio has been pinned in time).
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

// The warp tool: drag a moment of the clip to a new time. Grabs an existing marker within
// 6 px, or pins a new one (holding the clip's edges the first time). Double-click a
// marker to remove it.
export function startWarpDrag(screen, event, clip, element, daw) {
  const zoom = screen.zoom;
  const box = element.getBoundingClientRect();
  const markers = (clip.warp ?? []).map((m) => [...m]);
  const at = clip.offset + (event.clientX - box.left) / zoom; // rendered time under the pointer

  if (event.detail >= 2) {
    const hit = markers.findIndex(([, t]) => Math.abs((t - at) * zoom) < 6);
    if (hit >= 0) {
      markers.splice(hit, 1);
      daw("setWarp", { id: clip.id, markers: markers.length > 1 ? markers : [] });
    }
    return;
  }

  let index = markers.findIndex(([, t]) => Math.abs((t - at) * zoom) < 6);
  if (index < 0) {
    if (!markers.length) {
      const a = clip.offset, b = clip.offset + clip.length;
      markers.push([a, a], [b, b]);
    }
    markers.push([toSource(markers, at), at]);
    markers.sort((x, y) => x[0] - y[0]);
    index = markers.findIndex(([, t]) => t === at);
  }

  const startTarget = markers[index][1];
  const low = index > 0 ? markers[index - 1][1] + 0.01 : -Infinity;
  const high = index < markers.length - 1 ? markers[index + 1][1] - 0.01 : Infinity;
  const line = html("div", "warp-marker is-dragging");
  element.append(line);
  screen.dragging = true;

  const place = (target) => { line.style.left = `${(target - clip.offset) * zoom}px`; };
  place(startTarget);
  let target = startTarget;

  const move = (e) => {
    const timeline = clip.start + (startTarget - clip.offset) + (e.clientX - event.clientX) / zoom;
    target = Math.min(high, Math.max(low, screen.snapTime(timeline, e) - clip.start + clip.offset));
    place(target);
  };
  const up = () => {
    window.removeEventListener("pointermove", move);
    window.removeEventListener("pointerup", up);
    screen.dragging = false;
    line.remove();
    if (Math.abs(target - startTarget) > 1e-4 || !(clip.warp ?? []).length) {
      markers[index][1] = target;
      daw("setWarp", { id: clip.id, markers });
    }
    screen.flushSession?.();
  };
  window.addEventListener("pointermove", move);
  window.addEventListener("pointerup", up);
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
    groove,
  };
}
