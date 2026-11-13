// Pitch and time for audio in the DAW screen, on the bottom panel's Track tab:
//  - the clip editor (ui/clipeditor.js, re-exported here): a selected audio clip's
//    waveform on the project's bars and beats, with a tool per job (ui/warptool.js...);
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


// While the song plays, a drag can be heard before it's let go: `send` passes what the
// drag would store ({ markers } for previewWarp, { notes } for previewPitchNotes) at most
// every 120 ms and only when it changed, and the clip plays it (not stored, not an undo
// step). `end(stored)`: the drag is over; when it stored nothing (back where it started),
// the preview is cancelled, otherwise the real edit replaces it.
export function livePreview(daw, screen, command, id) {
  let last = "", pending = null, timer = 0, sentAt = 0, sent = false;
  const flush = () => {
    timer = 0;
    if (!pending) return;
    const text = JSON.stringify(pending);
    const args = pending;
    pending = null;
    if (text === last) return;
    last = text;
    sentAt = performance.now();
    sent = true;
    daw(command, { id, ...args });
  };
  return {
    send(args) {
      if (!screen.status?.playing && !sent) return;
      pending = args;
      if (!timer) timer = setTimeout(flush, Math.max(0, 120 - (performance.now() - sentAt)));
    },
    end(stored) {
      clearTimeout(timer);
      timer = 0;
      pending = null;
      if (sent && !stored) daw("cancelPreview", { id });
      sent = false;
    },
  };
}

// Warp maps as the session sends them: [[source, target], ...] in seconds.
export { createClipEditor } from "./clipeditor.js";

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

// The other way: where a moment of the file (source seconds) plays in the rendered copy.
export function toTarget(markers, source) {
  if (!markers?.length) return source;
  const first = markers[0], last = markers[markers.length - 1];
  if (source <= first[0]) return source - first[0] + first[1];
  if (source >= last[0]) return source - last[0] + last[1];
  for (let i = 1; i < markers.length; ++i) {
    const [s0, t0] = markers[i - 1], [s1, t1] = markers[i];
    if (source <= s1) return t0 + (source - s0) * (t1 - t0) / (s1 - s0);
  }
  return source;
}

export const prefs = {
  get(key, fallback) { try { const v = localStorage.getItem(`micfx.rhythm.${key}`); return v == null ? fallback : JSON.parse(v); } catch { return fallback; } },
  set(key, value) { try { localStorage.setItem(`micfx.rhythm.${key}`, JSON.stringify(value)); } catch { /* private mode */ } },
};

export const semitoneText = (v) => {
  const st = Math.round(v), cents = Math.round((v - st) * 100);
  return `${st > 0 ? "+" : ""}${st} st${cents ? ` ${cents > 0 ? "+" : ""}${cents}¢` : ""}`;
};

// A section of the Pitch & time card that folds to its title; open or closed is remembered.
export function section(key, title, open, ...content) {
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
export function pitchKnobs(ValueKnob, label, value, send) {
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
export function keyCaption(session, shift) {
  const key = session.key?.key ?? -1;
  if (key < 0) return "The song's key isn't set (Key button, top bar). Pitch works without it.";
  const name = (k) => `${NOTE_NAMES[((k % 12) + 12) % 12]}${session.key.minor ? " minor" : " major"}`;
  const semis = Math.round(shift);
  if (Math.abs(shift) < 0.005) return `In the song's key, ${name(key)}.`;
  const cents = Math.round((shift - semis) * 100);
  return `Sounds in ${name(key + semis)}${cents ? ` (${cents > 0 ? "+" : ""}${cents}¢)` : ""}; the song is in ${name(key)}.`;
}

// The algorithm picker and "Keep formants", for some clips (one, or every clip of a track).
export function algorithmControls(daw, clipsOf) {
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

  // Quantize (the same controls as the clip editor's Quantize section).
  const quantize = createQuantizeControls(screen, ui, {
    target: () => ({ track: owner.id }),
    trackId: () => owner.id,
    quantizeText: "Quantize track", quantizeTitle: "Warp every clip on this track so its notes start on the grid",
    resetText: "Reset track", resetTitle: "Put this track's clips back as recorded (time only; pitch stays)",
    name: () => owner.name,
  });

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

  sections.append(section("quantize", "Quantize", true, ...quantize.elements),
                  section("tempo", "Tempo", true, tempoRow, lineUp, tempoInfo),
                  section("link", "Linked tracks", false, linkRow, linkInfo));

  const update = (t) => {
    owner = t;
    pitch.update(t.transpose);
    keyInfo.textContent = keyCaption(screen.session, t.transpose || 0);
    algorithm.update(audioClips());
    quantize.update();
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

// Grid note values (beats) and feels; "bar" is a whole bar.
const NOTE_VALUES = [[1, "1/4"], [0.5, "1/8"], [0.25, "1/16"], [0.125, "1/32"], ["bar", "Bars"]];
const FEELS = [["straight", "Straight", 1], ["triplet", "Triplet", 2 / 3], ["dotted", "Dotted", 1.5]];

// The note value and feel last chosen (older settings kept a triplet eighth as 1/3).
function gridChoice() {
  let steps = prefs.get("steps", 0.5), feel = prefs.get("feel", "straight");
  if (Math.abs(Number(steps) - 1 / 3) < 1e-6) { steps = 0.5; feel = "triplet"; }
  if (!NOTE_VALUES.some(([v]) => String(v) === String(steps))) steps = 0.5;
  if (!FEELS.some(([k]) => k === feel)) feel = "straight";
  return { steps, feel };
}

// The quantize settings last chosen (the clip editor's Quantize section and the track's
// Pitch & time card share them), for the Quantize buttons and the Q shortcut.
export function savedQuantizeArgs(session, trackId) {
  const { steps, feel } = gridChoice();
  const multiplier = FEELS.find(([k]) => k === feel)?.[2] ?? 1;
  const groove = prefs.get("groove", -1);
  const args = {
    steps: steps === "bar" ? session.beatsPerBar ?? 4 : (Number(steps) || 0.5) * multiplier,
    strength: prefs.get("strength", 100) / 100,
    swing: prefs.get("swing", 0) / 100,
    catch: prefs.get("catch", 100) / 200,
    safe: prefs.get("safe", 0) / 200,
    maxMove: prefs.get("maxMove", 0) / 1000,
    iterative: prefs.get("iterative", false),
    slice: prefs.get("slice", false),
    groove: -1,
  };
  if (typeof groove === "string" && groove.startsWith("saved:")) {
    const name = groove.slice(6);
    if ((session.grooves ?? []).some((g) => g.name === name)) args.savedGroove = name;
  } else {
    const leader = session.tracks.find((t) => t.id === Number(groove));
    if (Number(groove) !== trackId && leader?.clips.length) args.groove = Number(groove);
  }
  return args;
}

// The quantize controls: grid (note value, feel, or a groove: another track's or a saved one),
// Strength, Swing, Catch, Safe zone, Max move, Iterative, Slice, Auto, and the Quantize and
// Reset buttons. `options`: target() -> the quantize/clearWarp args ({ clips } or { track }),
// trackId(), name(), and the buttons' texts. With Auto on, changing a control quantizes again
// (joined to the last Quantize as one undo step).
export function createQuantizeControls(screen, ui, options) {
  const { daw, ValueKnob, button, toast } = ui;
  let autoChained = false;
  const run = async (chained) => {
    const result = await daw("quantize", { ...options.target(), ...savedQuantizeArgs(screen.session, options.trackId()), continue: chained });
    if (!result?.ok) return;
    autoChained = true;
    if (!chained) toast(result.onsets ? `Quantize ${options.name()}: ${result.moved} of ${result.onsets} notes moved onto the grid.` : `Quantize ${options.name()}: no clear note starts found.`);
  };
  let autoTimer = 0;
  const changed = () => {
    if (!prefs.get("autoQuantize", false) || !autoChained) return;
    clearTimeout(autoTimer);
    autoTimer = setTimeout(() => run(true), 250);
  };

  const { steps, feel } = gridChoice();
  const noteValue = html("select", "dropdown");
  noteValue.title = "The grid's note value";
  NOTE_VALUES.forEach(([v, t]) => noteValue.append(new Option(t, v)));
  noteValue.value = String(steps);
  const feelSelect = html("select", "dropdown");
  feelSelect.title = "Straight, triplet or dotted lines";
  FEELS.forEach(([k, t]) => feelSelect.append(new Option(t, k)));
  feelSelect.value = feel;
  const groove = html("select", "dropdown rhythm-groove");
  groove.title = "What to quantize to: the grid, the groove of another track, or a groove saved from hitpoints (Hitpoints: Create groove)";
  const fillGrooves = () => {
    const keep = String(prefs.get("groove", -1));
    groove.replaceChildren(new Option("Grid", -1));
    for (const other of screen.session.tracks) if (other.id !== options.trackId() && other.clips.length && other.kind !== "midi") groove.append(new Option(`Groove of ${other.name}`, other.id));
    for (const g of screen.session.grooves ?? []) groove.append(new Option(`Groove: ${g.name}`, `saved:${g.name}`));
    groove.value = keep;
    if (groove.selectedIndex < 0) groove.value = "-1";
    const byGroove = groove.value !== "-1";
    noteValue.disabled = byGroove && groove.value.startsWith("saved:");
    feelSelect.disabled = noteValue.disabled || noteValue.value === "bar";
  };
  noteValue.addEventListener("change", () => { prefs.set("steps", noteValue.value); fillGrooves(); changed(); });
  feelSelect.addEventListener("change", () => { prefs.set("feel", feelSelect.value); changed(); });
  groove.addEventListener("change", () => {
    const v = groove.value;
    prefs.set("groove", v.startsWith("saved:") ? v : Number(v));
    fillGrooves();
    changed();
  });
  const gridRow = html("div", "panel-actions rhythm-grid-row");
  gridRow.append(noteValue, feelSelect, groove);

  const knob = (key, label, max, fallback, unit, title) => {
    const k = new ValueKnob({ label, min: 0, max, step: 1, value: prefs.get(key, fallback), defaultValue: fallback,
                              format: (v) => (unit === "ms" ? (Math.round(v) ? `${Math.round(v)} ms` : "Off") : `${Math.round(v)}%`),
                              onChange: (v) => { prefs.set(key, Math.round(v)); changed(); } });
    k.element.title = title;
    return k;
  };
  const knobs = html("div", "insert-controls rhythm-knobs");
  knobs.append(
    knob("strength", "Strength", 100, 100, "%", "How far note starts move towards the grid (less keeps some feel)").element,
    knob("swing", "Swing", 100, 0, "%", "Delay every second line, up to a triplet feel").element,
    knob("catch", "Catch", 100, 100, "%", "Only note starts this close to a grid line move (100%: all of them)").element,
    knob("safe", "Safe zone", 100, 0, "%", "Note starts already this close to a grid line stay where they are").element,
    knob("maxMove", "Max move", 500, 0, "ms", "The farthest a note start moves (Off: no limit)").element);

  const option = (key, text, title, onChange) => {
    const label = html("label", "rhythm-check");
    const box = html("input");
    Object.assign(box, { type: "checkbox", checked: prefs.get(key, false) });
    box.addEventListener("change", () => { prefs.set(key, box.checked); onChange?.(box.checked); });
    label.append(box, document.createTextNode(` ${text}`));
    label.title = title;
    return label;
  };
  const optionRow = html("div", "panel-actions");
  optionRow.append(
    option("iterative", "Iterative", "Each press moves the notes from where they are now: at 50%, two presses close three quarters of the gap", changed),
    option("slice", "Slice", "Cut at the notes and move the pieces instead of stretching (best for hard drums; gaps stay silent)", changed),
    option("autoQuantize", "Auto", "After a Quantize, changing a setting quantizes again at once (one undo step)"));

  const buttons = html("div", "panel-actions");
  const reset = button("tool", options.resetText ?? "Reset", options.resetTitle ?? "Put the timing back as recorded (pitch stays)",
    () => { autoChained = false; daw("clearWarp", options.target()); });
  buttons.append(button("tool", options.quantizeText ?? "Quantize", options.quantizeTitle ?? "Warp so the note starts land on the grid (Q)", () => run(false)), reset);

  const caption = html("p", "panel-caption", "Uses the hitpoints the clip editor shows (Hitpoints sets which count).");
  fillGrooves();
  return {
    elements: [gridRow, knobs, optionRow, buttons, caption],
    reset,
    update: () => fillGrooves(),
    dispose: () => clearTimeout(autoTimer),
  };
}
