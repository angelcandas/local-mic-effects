// The note pitch editor: an audio clip's sung notes as blocks on a keyboard, under the
// clip editor's "Notes" view (rhythm.js). Each note can be moved up or down, have its drift
// straightened, its vibrato narrowed or widened, its formants moved, its level changed and
// its glide in from the note before made quicker or slower; notes can be split and joined,
// and moved in time (warping, like the waveform view). The pitch line as sung shows faintly,
// as it will sound brightly, and the voice's loudness runs along the bottom.
//
// Everything is non-destructive: the edits are stored on the clip (setPitchNotes) and the
// C++ side renders the take again in the background (see src/analysis/Pitch.h and Warp.h).
import { NOTE_NAMES, SCALES } from "./modules.js";
import { clamp, html } from "./widgets.js";
import { livePreview, toSource } from "./rhythm.js";

const KEYS = 40;     // the keyboard's width
const RULER = 16;    // bar and beat numbers on top
const LANE = 54;     // the loudness lane's height

// Tools: [key, name, shortcut, what it does, icon path (24x24, stroked)]. With any tool, Shift+click
// picks a note (or drops it), and a drag across empty space picks every note it touches. The
// shape of the picked notes (straightness, vibrato, formants, level, glide) is set with the
// sliders under the tools, all of them at once.
const TOOLS = [
  ["select", "Select", "V", "Drag a note up or down to move its pitch (it lands on a semitone; Shift or Alt: free, in cents; Ctrl/Cmd: whole semitones from where it is). Picked notes move together. Up/Down arrows move picked notes a semitone, Shift+arrow 10 cents. Double-click a note to put it right on its semitone",
    '<path d="M6 3l12 9-5.5 1.2L16 20l-2.6 1.2-2.7-6.4L6 19z"/>'],
  ["split", "Split", "C", "Click inside a note to cut it in two there; Alt-click a note to join it to the next one",
    '<circle cx="6" cy="7" r="2.6"/><circle cx="6" cy="17" r="2.6"/><path d="M8.2 8.4L20 17M8.2 15.6L20 7"/>'],
  ["timing", "Timing", "W", "Drag a note left or right to move it in time; picked notes move together and the notes around them stay put (the clip is warped)",
    '<path d="M4 12h16M8 8l-4 4 4 4M16 8l4 4-4 4"/>'],
];

// How far a note is pulled onto its semitone (0..1), and the note pulled that far. The note's
// semitone is the one nearest where it is now (so a note moved by hand keeps its new note); at 0
// it keeps the cents it was sung with, at 1 it sits right on the semitone.
export function quantizeOf(note, transpose = 0) {
  const sung = note.pitch + transpose, now = sung + note.shift;
  const off = sung - Math.round(sung);
  if (Math.abs(off) < 0.02) return null; // sung right on it: nothing to tell
  return clamp(1 - (now - Math.round(now)) / off, 0, 1);
}
export function quantizeNote(note, amount, transpose = 0) {
  const sung = note.pitch + transpose, now = sung + note.shift;
  const target = Math.round(now), off = sung - Math.round(sung);
  return { ...note, shift: clamp(target + off * (1 - amount) - sung, -24, 24) };
}

// The sliders for the picked notes: what each one reads from a note and writes into it.
const SHAPES = [
  { key: "quantize", label: "Quantize", min: 0, max: 100, step: 1, neutral: 0,
    title: "Pitch quantize: pull the picked notes onto their semitone, 100% = right on it (their drift and vibrato stay)",
    text: (v) => `${Math.round(v)}%`,
    read: (n, tr) => { const q = quantizeOf(n, tr); return q == null ? null : q * 100; },
    write: (n, v, tr) => quantizeNote(n, v / 100, tr) },
  { key: "straighten", label: "Straighten", min: 0, max: 100, step: 1, neutral: 0,
    title: "Straighten pitch: take the slow wander out of the picked notes, 100% = flat on the note (their vibrato stays)",
    text: (v) => `${Math.round(v)}%`, read: (n) => n.straighten * 100, write: (n, v) => ({ ...n, straighten: v / 100 }) },
  { key: "vibrato", label: "Vibrato", min: 0, max: 200, step: 1, neutral: 100,
    title: "The picked notes' vibrato: 0% = none, 100% = as sung, 200% = twice as wide",
    text: (v) => `${Math.round(v)}%`, read: (n) => n.vibrato * 100, write: (n, v) => ({ ...n, vibrato: v / 100 }) },
  { key: "formant", label: "Formant", min: -12, max: 12, step: 0.1, neutral: 0,
    title: "The picked notes' formants: up makes the voice smaller and brighter, down bigger and darker, at the same pitch",
    text: (v) => `${signed(v, 1)} st`, read: (n) => n.formant, write: (n, v) => ({ ...n, formant: v }) },
  { key: "gain", label: "Volume", min: -24, max: 12, step: 0.1, neutral: 0,
    title: "The picked notes' level (also in the loudness lane)",
    text: (v) => `${signed(v, 1)} dB`, read: (n) => n.gain, write: (n, v) => ({ ...n, gain: v }) },
  { key: "transition", label: "Glide", min: 5, max: 500, step: 1, neutral: 60,
    title: "How long the picked notes take to glide in from the note before",
    text: (v) => `${Math.round(v)} ms`, read: (n) => n.transition * 1000, write: (n, v) => ({ ...n, transition: v / 1000 }) },
];

const ICON = (path) => `<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${path}</svg>`;

const prefs = {
  get(key, fallback) { try { const v = localStorage.getItem(`micfx.notes.${key}`); return v == null ? fallback : JSON.parse(v); } catch { return fallback; } },
  set(key, value) { try { localStorage.setItem(`micfx.notes.${key}`, JSON.stringify(value)); } catch { /* private mode */ } },
};

export const noteName = (midi) => {
  const n = Math.round(midi);
  return `${NOTE_NAMES[((n % 12) + 12) % 12]}${Math.floor(n / 12) - 1}`;
};

const centsText = (midi) => {
  const cents = Math.round((midi - Math.round(midi)) * 100);
  return cents ? ` ${cents > 0 ? "+" : "−"}${Math.abs(cents)}¢` : "";
};

const signed = (v, digits = 0) => `${v > 0 ? "+" : v < 0 ? "−" : ""}${Math.abs(v).toFixed(digits)}`;

// What a note's edits say, short.
export function describeNote(note, transpose = 0) {
  const sung = note.pitch + transpose, now = sung + note.shift;
  const parts = [`${noteName(now)}${centsText(now)}`];
  if (Math.abs(note.shift) > 0.004) parts[0] += ` (sung ${noteName(sung)}${centsText(sung)})`;
  if (note.straighten > 0.004) parts.push(`drift ${Math.round(note.straighten * 100)}% straighter`);
  if (Math.abs(note.vibrato - 1) > 0.004) parts.push(`vibrato ${Math.round(note.vibrato * 100)}%`);
  if (Math.abs(note.formant) > 0.04) parts.push(`formant ${signed(note.formant, 1)} st`);
  if (Math.abs(note.gain) > 0.04) parts.push(`${signed(note.gain, 1)} dB`);
  if (Math.abs(note.transition - 0.06) > 0.002) parts.push(`glide ${Math.round(note.transition * 1000)} ms`);
  return parts.join(" · ");
}

const isEdited = (n) => Math.abs(n.shift) > 1e-3 || n.straighten > 1e-3 || Math.abs(n.vibrato - 1) > 1e-3
                      || Math.abs(n.formant) > 1e-3 || Math.abs(n.gain) > 1e-3;

// The median of the sung pitch line between two rendered times (for split and joined notes).
function medianPitch(data, at, until) {
  const values = [];
  const length = until - at;
  for (const [from, to] of [[at + 0.2 * length, until - 0.2 * length], [at, until]]) {
    for (let i = 0; i < data.times.length; ++i)
      if (data.times[i] >= from && data.times[i] <= to && data.sung[i] > 0) values.push(data.sung[i]);
    if (values.length >= 3) break;
  }
  if (!values.length) return null;
  values.sort((x, y) => x - y);
  return values[Math.floor(values.length / 2)];
}

// `host` is the clip editor: { screen, daw, toast, getClip(), getTrack(), setStatus(text) }.
export function createNoteEditor(host) {
  const { screen, daw, toast } = host;
  const element = html("div", "note-editor");
  element.tabIndex = 0;

  // Tools and actions.
  const bar = html("div", "note-editor-bar");
  let tool = prefs.get("tool", "select");
  if (!TOOLS.some(([key]) => key === tool)) tool = "select";
  const toolButtons = new Map();
  const tools = html("div", "note-tools");
  tools.setAttribute("role", "toolbar");
  for (const [key, name, shortcut, about, icon] of TOOLS) {
    const b = html("button", "note-tool");
    b.innerHTML = ICON(icon);
    b.title = `${name} (${shortcut}): ${about}`;
    b.setAttribute("aria-label", name);
    b.addEventListener("click", () => setTool(key));
    toolButtons.set(key, b);
    tools.append(b);
  }
  const action = (text, title, run) => {
    const b = html("button", "tool", text);
    b.title = title;
    b.addEventListener("click", run);
    return b;
  };
  const toKey = action("To key", "Put the picked notes (or all of them) right on their semitone, or on the nearest note of the song's key when it's set (S)", () => snapNotes());
  const reset = action("Reset", "Put the picked notes (or all of them) back as sung", () => resetNotes());
  const listenBox = html("input");
  listenBox.type = "checkbox";
  listenBox.checked = prefs.get("listen", true);
  listenBox.addEventListener("change", () => prefs.set("listen", listenBox.checked));
  const listen = html("label", "rhythm-check note-listen");
  listen.title = "Hear a note as you change it: it plays when it lands on a new semitone, after each arrow key and when you let go of a slider (not while the song plays: then the song itself has the change)";
  listen.append(listenBox, document.createTextNode(" Listen"));
  const readout = html("span", "note-readout");
  bar.append(tools, toKey, reset, listen, readout);

  // The picked notes' shape: one slider per quality, acting on every picked note at once (on
  // all the notes when none is picked). Double-click a slider's name to put it back.
  const shapeRow = html("div", "note-shape");
  const scope = html("span", "note-shape-scope");
  shapeRow.append(scope);
  const sliders = [];
  for (const def of SHAPES) {
    const box = html("label", "note-slider");
    box.title = def.title;
    const name = html("span", "note-slider-name", def.label);
    name.title = `${def.title}. Double-click to put it back`;
    const input = html("input");
    Object.assign(input, { type: "range", min: def.min, max: def.max, step: def.step });
    const out = html("output", "note-slider-value");
    box.append(name, input, out);
    shapeRow.append(box);
    sliders.push({ def, input, out, box });
    input.addEventListener("input", () => shapeInput(def, Number(input.value)));
    input.addEventListener("change", () => shapeDone());
    name.addEventListener("dblclick", (event) => { event.preventDefault(); shapeInput(def, def.neutral); shapeDone(); });
  }

  const area = html("div", "note-area");
  const canvas = html("canvas", "note-canvas");
  const playhead = html("div", "note-playhead");
  const band = html("div", "note-band");
  band.hidden = true;
  area.append(canvas, playhead, band);
  const lane = html("div", "note-lane");
  const laneCanvas = html("canvas", "note-canvas");
  const laneLabel = html("span", "note-lane-label", "Loudness");
  lane.append(laneCanvas, laneLabel);
  const message = html("div", "note-message");
  message.hidden = true;
  element.append(bar, shapeRow, area, lane, message);

  // What's shown: the clip's notes (edited locally until stored), its pitch lines and loudness.
  let data = null, notes = [], picked = new Set(), preview = null, hover = -1;
  let view = null, rows = null, disposed = false, asking = "", shownKey = "";

  const clip = () => host.getClip();
  const transpose = () => data?.transpose ?? 0;
  const span = () => {
    const c = clip();
    const a = c.offset, b = c.offset + Math.max(0.01, c.length);
    if (!view) return [a, b];
    const length = clamp(view[1] - view[0], 0.05, b - a);
    const start = clamp(view[0], a, b - length);
    return [start, start + length];
  };
  const width = () => Math.max(1, area.clientWidth - KEYS);
  const xOf = (t) => { const [a, b] = span(); return KEYS + (t - a) / (b - a) * width(); };
  const timeAt = (clientX) => { const [a, b] = span(); return a + (clientX - area.getBoundingClientRect().left - KEYS) / width() * (b - a); };
  const shown = (n) => n.pitch + n.shift + transpose();
  const visibleNotes = () => { const [a, b] = span(); return notes.filter((n) => n.until > a && n.at < b); };

  // Pitch rows: the notes in view with room around them (at least an octave).
  const fitRows = () => {
    const list = visibleNotes().map(shown);
    const all = list.length ? list : notes.map(shown);
    if (!all.length) { rows = [55, 72]; return; }
    let low = Math.floor(Math.min(...all)) - 2, high = Math.ceil(Math.max(...all)) + 2;
    while (high - low < 9) { --low; ++high; }
    rows = [low, high];
  };
  const rowHeight = () => (area.clientHeight - RULER) / (rows[1] - rows[0] + 1);
  const yOf = (midi) => RULER + (rows[1] + 0.5 - midi) * rowHeight();
  const midiAt = (clientY) => rows[1] + 0.5 - (clientY - area.getBoundingClientRect().top - RULER) / rowHeight();

  // The song's key, for snapping and for shading the notes outside it.
  const keyNotes = () => {
    const key = screen.session?.key?.key ?? -1;
    if (key < 0) return null;
    const scale = SCALES[screen.session.key.minor ? 2 : 1];
    return new Set(scale.map((s) => (key + s) % 12));
  };

  const fetchData = async () => {
    const c = clip();
    const key = `${c.id}|${c.audioKey}|${c.offset}|${c.length}|${JSON.stringify(c.warp ?? [])}`;
    if (key === shownKey || key === asking) return;
    asking = key;
    const result = await daw("pitchNotes", { id: c.id });
    if (disposed || asking !== key) return;
    asking = "";
    if (!result?.ok) {
      showMessage(result?.error || "The clip's notes couldn't be found.");
      return;
    }
    shownKey = key;
    const first = !data;
    data = result;
    notes = result.notes.map((n) => ({ ...n }));
    picked = new Set([...picked].filter((i) => i < notes.length));
    preview = null;
    if (first || !rows) fitRows();
    showMessage(result.linked ? "This track is linked: notes on linked tracks can't be edited one by one. Unlink it in the track's Pitch & time card."
                : !notes.length ? "No sung notes found in this clip. The note editor is for a single voice or instrument line." : "");
    paint();
  };

  const showMessage = (text) => {
    message.textContent = text;
    message.hidden = !text;
  };

  //--------------------------------------------------------------------------
  // Drawing.
  const css = (name, fallback) => getComputedStyle(element).getPropertyValue(name).trim() || fallback;

  const paint = () => {
    if (!area.isConnected || !rows) return;
    const ratio = window.devicePixelRatio || 1;
    const w = area.clientWidth, h = area.clientHeight;
    if (w < 2 || h < 2) return;
    if (canvas.width !== Math.round(w * ratio)) canvas.width = Math.round(w * ratio);
    if (canvas.height !== Math.round(h * ratio)) canvas.height = Math.round(h * ratio);
    const g = canvas.getContext("2d");
    g.setTransform(ratio, 0, 0, ratio, 0, 0);
    g.clearRect(0, 0, w, h);
    const colour = css("--wave", "#8c7bff");
    const [a, b] = span();
    const c = clip();
    const rh = rowHeight();
    const inKey = keyNotes();

    // Rows: dark for the black keys, darker still outside the song's key.
    for (let m = rows[0]; m <= rows[1]; ++m) {
      const y = yOf(m) - rh / 2;
      const pc = ((m % 12) + 12) % 12;
      const black = [1, 3, 6, 8, 10].includes(pc);
      g.fillStyle = black ? "rgba(0,0,0,0.22)" : "rgba(255,255,255,0.02)";
      if (inKey && !inKey.has(pc)) g.fillStyle = "rgba(0,0,0,0.34)";
      g.fillRect(KEYS, y, w - KEYS, rh);
      if (pc === 0 || pc === 5) { g.fillStyle = "rgba(255,255,255,0.07)"; g.fillRect(KEYS, Math.round(y + rh) - 0.5, w - KEYS, 1); }
    }

    // The ruler (click: move the song's cursor there, double-click: play from there), then bars
    // and beats from the project's tempo (the clip's time on the song).
    g.fillStyle = "rgba(255,255,255,0.05)";
    g.fillRect(KEYS, 0, w - KEYS, RULER);
    g.fillStyle = "rgba(255,255,255,0.12)";
    g.fillRect(KEYS, RULER - 1, w - KEYS, 1);
    const map = screen.tempoMap, perBar = screen.beatsPerBar || 4;
    if (map) {
      const songOf = (t) => c.start + (t - c.offset);
      const firstBeat = Math.floor(map.toBeats(Math.max(0, songOf(a)))), lastBeat = Math.ceil(map.toBeats(Math.max(0, songOf(b))));
      const beatPx = width() / Math.max(1e-6, lastBeat - firstBeat + 1);
      const every = beatPx < 6 ? perBar : 1;
      g.font = "10px system-ui, sans-serif";
      for (let beat = Math.ceil(firstBeat / every) * every; beat <= lastBeat; beat += every) {
        const x = Math.round(xOf(map.toSeconds(beat) - c.start + c.offset)) + 0.5;
        if (x < KEYS || x > w) continue;
        const bar = beat % perBar === 0;
        g.fillStyle = bar ? "rgba(255,255,255,0.16)" : "rgba(255,255,255,0.06)";
        g.fillRect(x, RULER, 1, h - RULER);
        if (bar || beatPx > 28) {
          g.fillStyle = bar ? "rgba(255,255,255,0.6)" : "rgba(255,255,255,0.32)";
          g.fillText(bar ? `${beat / perBar + 1}` : `${Math.floor(beat / perBar) + 1}.${(beat % perBar) + 1}`, x + 3, 11);
        }
      }
    }

    // The pitch line as sung (faint), as it will sound (bright).
    const line = (values, style, lineWidth, offsets) => {
      g.strokeStyle = style;
      g.lineWidth = lineWidth;
      g.beginPath();
      let drawing = false, lastT = -Infinity;
      for (let i = 0; i < data.times.length; ++i) {
        const t = data.times[i];
        if (t < a - 0.05 || t > b + 0.05) { drawing = false; continue; }
        const v = values[i];
        if (!(v > 0) || t - lastT > 0.03) drawing = false;
        lastT = t;
        if (!(v > 0)) continue;
        const y = yOf(v + transpose() + (offsets ? offsets(t) : 0));
        if (drawing) g.lineTo(xOf(t), y); else g.moveTo(xOf(t), y);
        drawing = true;
      }
      g.stroke();
    };

    // Notes: where they were sung (dashed, when moved), then where they are.
    const moving = preview?.offsets;
    const offsetAt = moving ? (t) => { for (const [i, d] of moving) if (t >= notes[i].at && t <= notes[i].until) return d; return 0; } : null;
    g.save();
    g.beginPath();
    g.rect(KEYS, RULER, w - KEYS, h - RULER);
    g.clip();
    if (data) line(data.sung, "rgba(255,255,255,0.28)", 1, null);

    notes.forEach((n, i) => {
      if (n.until < a || n.at > b) return;
      const moved = preview?.times?.get(i) ?? 0;
      const x0 = xOf(n.at + moved), x1 = xOf(n.until);
      const extra = moving?.get(i) ?? 0;
      const y = yOf(shown(n) + extra);
      const hgt = Math.max(6, rh * 0.9);
      const isPicked = picked.has(i);

      if (Math.abs(n.shift + extra) > 0.004) {
        g.setLineDash([3, 3]);
        g.strokeStyle = "rgba(255,255,255,0.3)";
        g.lineWidth = 1;
        g.strokeRect(x0 + 0.5, yOf(n.pitch + transpose()) - hgt / 2 + 0.5, Math.max(2, x1 - x0 - 1), hgt - 1);
        g.setLineDash([]);
      }

      g.globalAlpha = isPicked ? 0.95 : i === hover ? 0.7 : 0.5;
      g.fillStyle = colour;
      roundRect(g, x0, y - hgt / 2, Math.max(2, x1 - x0), hgt, 3);
      g.fill();
      g.globalAlpha = 1;
      if (isPicked) {
        g.strokeStyle = "#fff";
        g.lineWidth = 1.5;
        roundRect(g, x0 + 0.5, y - hgt / 2 + 0.5, Math.max(2, x1 - x0 - 1), hgt - 1, 3);
        g.stroke();
      }
      if (isEdited(n)) { // a dot on edited notes
        g.fillStyle = "#fff";
        g.beginPath();
        g.arc(x0 + 5, y - hgt / 2 + 5, 2, 0, Math.PI * 2);
        g.fill();
      }

      // Its name inside.
      g.font = "10px system-ui, sans-serif";
      const name = `${noteName(shown(n) + extra)}${centsText(shown(n) + extra)}`;
      if (x1 - x0 > g.measureText(name).width + 12 && hgt >= 10) {
        g.fillStyle = "rgba(10,12,18,0.85)";
        g.font = `${Math.min(10, hgt - 2)}px system-ui, sans-serif`;
        g.fillText(name, x0 + 9, y + Math.min(10, hgt - 2) / 2 - 1);
      }
    });

    if (data) line(data.pitch, "rgba(255,255,255,0.92)", 1.5, offsetAt);
    g.restore();

    // The keyboard.
    for (let m = rows[0]; m <= rows[1]; ++m) {
      const y = yOf(m) - rh / 2;
      const pc = ((m % 12) + 12) % 12;
      const black = [1, 3, 6, 8, 10].includes(pc);
      g.fillStyle = "#d9dce6";
      g.fillRect(0, y, KEYS - 2, rh);
      g.fillStyle = "rgba(0,0,0,0.25)";
      if (pc === 4 || pc === 11) g.fillRect(0, Math.round(y), KEYS - 2, 1); // between E-F and B-C
      if (black) { g.fillStyle = "#1d2029"; g.fillRect(0, y + 1, (KEYS - 2) * 0.62, rh - 2); }
      if (pc === 0 && rh >= 8) {
        g.fillStyle = "#3b3f4c";
        g.font = `${Math.min(10, rh - 1)}px system-ui, sans-serif`;
        g.fillText(noteName(m), 3, y + rh - 2);
      }
    }
    g.fillStyle = css("--panel", "#13161e");
    g.fillRect(0, 0, KEYS, RULER);

    paintLane();
    placePlayhead();
    readout.textContent = readoutText();
    refreshShape();
  };

  const paintLane = () => {
    const ratio = window.devicePixelRatio || 1;
    const w = lane.clientWidth, h = lane.clientHeight;
    if (w < 2 || h < 2) return;
    if (laneCanvas.width !== Math.round(w * ratio)) laneCanvas.width = Math.round(w * ratio);
    if (laneCanvas.height !== Math.round(h * ratio)) laneCanvas.height = Math.round(h * ratio);
    const g = laneCanvas.getContext("2d");
    g.setTransform(ratio, 0, 0, ratio, 0, 0);
    g.clearRect(0, 0, w, h);
    if (!data) return;
    const [a, b] = span();
    const colour = css("--wave", "#8c7bff");

    // Loudness (dB, -60..0) as sung, plus each note's level change.
    const levelAt = (t) => { for (const n of notes) if (t >= n.at && t <= n.until) return n.gain; return 0; };
    g.fillStyle = colour;
    g.globalAlpha = 0.35;
    g.beginPath();
    g.moveTo(KEYS, h);
    for (let i = 0; i < data.times.length; ++i) {
      const t = data.times[i];
      if (t < a - 0.05 || t > b + 0.05) continue;
      const db = clamp(data.energy[i] + levelAt(t), -60, 6);
      g.lineTo(xOf(t), h - (db + 60) / 66 * (h - 4));
    }
    g.lineTo(w, h);
    g.closePath();
    g.fill();
    g.globalAlpha = 1;

    // Level handles: a line across each note, 0 dB in the middle (-24 .. +12 dB).
    const mid = h * 0.6;
    g.fillStyle = "rgba(255,255,255,0.12)";
    g.fillRect(KEYS, Math.round(mid), w - KEYS, 1);
    notes.forEach((n, i) => {
      if (n.until < a || n.at > b) return;
      const y = levelY(n.gain, h);
      g.fillStyle = picked.has(i) ? "#fff" : "rgba(255,255,255,0.6)";
      g.fillRect(xOf(n.at), y - 1, Math.max(2, xOf(n.until) - xOf(n.at)), 2);
      if (Math.abs(n.gain) > 0.04 && xOf(n.until) - xOf(n.at) > 34) {
        g.font = "9px system-ui, sans-serif";
        g.fillText(`${signed(n.gain, 1)} dB`, xOf(n.at) + 2, y - 3);
      }
    });
    g.fillStyle = css("--panel", "#13161e");
    g.fillRect(0, 0, KEYS, h);
  };
  const levelY = (gain, h) => h * 0.6 - clamp(gain, -24, 12) / 24 * (h * 0.6 - 4);
  const gainAtY = (y, h) => clamp(-(y - h * 0.6) / (h * 0.6 - 4) * 24, -24, 12);

  const placePlayhead = () => {
    const c = clip();
    const t = (screen.status?.position ?? -1) - c.start + c.offset;
    const [a, b] = span();
    playhead.hidden = !(t >= a && t <= b);
    if (!playhead.hidden) playhead.style.left = `${xOf(t)}px`;
  };
  let lastPosition = null, frame = 0;
  const tick = () => {
    if (disposed) return;
    if (element.isConnected && screen.status?.position !== lastPosition) {
      lastPosition = screen.status?.position;
      // While the song plays, page along with it (when zoomed in and the cursor is in the clip).
      if (screen.status?.playing && view && !dragging && !screen.dragging) {
        const c = clip(), t = lastPosition - c.start + c.offset;
        const [a, b] = span();
        if ((t > b || t < a - 0.05) && t >= c.offset && t <= c.offset + c.length) {
          view = [t - (b - a) * 0.05, t + (b - a) * 0.95];
          view = span();
          paint();
        }
      }
      placePlayhead();
    }
    frame = requestAnimationFrame(tick);
  };
  frame = requestAnimationFrame(tick);

  // The sliders show the picked notes' values (the first one's, "mixed" when they differ).
  let shaping = null; // a slider being moved: { before, live }
  const refreshShape = () => {
    if (shaping) return;
    const list = targets();
    const off = !notes.length || Boolean(data?.linked);
    scope.textContent = !notes.length ? "Notes" : picked.size ? `${list.length} picked note${list.length === 1 ? "" : "s"}` : `All ${notes.length} notes`;
    scope.title = picked.size ? "The sliders change the picked notes (Esc drops them)" : "Nothing picked: the sliders change every note. Click a note, or drag across empty space, to pick some";
    for (const { def, input, out, box } of sliders) {
      input.disabled = off;
      const values = list.map((i) => def.read(notes[i], transpose())).filter((v) => v != null);
      const value = values.length ? values[0] : def.neutral;
      const mixed = values.some((v) => Math.abs(v - value) > def.step);
      input.value = String(value);
      out.textContent = mixed ? "mixed" : def.text(value);
      box.classList.toggle("is-mixed", mixed);
      box.classList.toggle("is-changed", values.some((v) => Math.abs(v - def.neutral) > def.step / 2));
    }
  };
  const shapeInput = (def, value) => {
    if (!notes.length || data?.linked) return;
    if (!shaping) shaping = { before: notes.map((n) => ({ ...n })), live: livePreview(daw, screen, "previewPitchNotes", clip().id) };
    const chosen = new Set(targets());
    notes = shaping.before.map((n, i) => (chosen.has(i) ? def.write(n, value, transpose()) : n));
    const slider = sliders.find((x) => x.def === def);
    slider.out.textContent = def.text(value);
    slider.box.classList.remove("is-mixed");
    shaping.live.send({ notes: payload(notes) });
    paint();
  };
  const shapeDone = () => {
    if (!shaping) return;
    const { live } = shaping;
    shaping = null;
    live.end(true);
    store(notes);
    const list = targets();
    if (list.length <= 3) audition(notes, list[0]);
  };

  const readoutText = () => {
    const list = [...picked].map((i) => notes[i]).filter(Boolean);
    if (list.length === 1) return describeNote(list[0], transpose());
    if (list.length > 1) return `${list.length} notes picked`;
    if (hover >= 0 && notes[hover]) return describeNote(notes[hover], transpose());
    return notes.length ? `${notes.length} notes${notes.some(isEdited) ? `, ${notes.filter(isEdited).length} edited` : ""}` : "";
  };

  //--------------------------------------------------------------------------
  // Editing.
  const payload = (list) => list.map(({ start, end, pitch, shift, straighten, vibrato, formant, gain, transition }) =>
    ({ start, end, pitch, shift, straighten, vibrato, formant, gain, transition }));
  let nudgeTimer = 0;
  const flushNudges = () => {
    if (!nudgeTimer) return;
    clearTimeout(nudgeTimer);
    nudgeTimer = 0;
    daw("setPitchNotes", { id: clip().id, notes: payload(notes) });
  };
  const store = (next) => {
    clearTimeout(nudgeTimer);
    nudgeTimer = 0;
    notes = next;
    paint();
    daw("setPitchNotes", { id: clip().id, notes: payload(notes) });
  };
  // Arrow keys and the like: painted at once, but a run of them within 250 ms is sent as
  // one edit, so it's one undo step and one render.
  const storeSoon = (next) => {
    notes = next;
    paint();
    clearTimeout(nudgeTimer);
    nudgeTimer = setTimeout(flushNudges, 250);
  };

  // Hear one note as it would sound with these edits (only that stretch renders).
  let lastAudition = 0;
  const audition = (list, index) => {
    const n = list[index];
    if (!listenBox.checked || !n || !data || data.linked || screen.status?.playing) return;
    const now = performance.now();
    if (now - lastAudition < 80) return;
    lastAudition = now;
    daw("auditionNote", { id: clip().id, from: Math.max(0, n.at - 0.03), to: n.until + 0.05, notes: payload(list) });
  };
  const targets = () => (picked.size ? [...picked] : notes.map((_, i) => i));
  const editPicked = (change) => {
    if (!notes.length) return;
    const chosen = new Set(targets());
    store(notes.map((n, i) => (chosen.has(i) ? change(n) : n)));
  };
  const snapNotes = () => {
    const inKey = keyNotes();
    editPicked((n) => {
      const sung = n.pitch + transpose();
      let target = Math.round(sung + n.shift);
      if (inKey) {
        let best = target, bestDistance = Infinity;
        for (let m = target - 2; m <= target + 2; ++m)
          if (inKey.has(((m % 12) + 12) % 12) && Math.abs(m - (sung + n.shift)) < bestDistance) { best = m; bestDistance = Math.abs(m - (sung + n.shift)); }
        target = best;
      }
      return { ...n, shift: target - sung };
    });
  };
  const resetNotes = () => {
    if (!picked.size) { store(notes.map((n) => ({ ...n, shift: 0, straighten: 0, vibrato: 1, formant: 0, gain: 0, transition: 0.06 }))); return; }
    editPicked((n) => ({ ...n, shift: 0, straighten: 0, vibrato: 1, formant: 0, gain: 0, transition: 0.06 }));
  };

  // Splitting and joining (times go back to the file through the warp map).
  const sourceOf = (t) => toSource(clip().warp ?? [], t);
  const splitAt = (index, t) => {
    const n = notes[index];
    if (t - n.at < 0.03 || n.until - t < 0.03) return;
    const cut = n.start + (n.end - n.start) * (t - n.at) / (n.until - n.at);
    const left = { ...n, end: cut, until: t, pitch: medianPitch(data, n.at, t) ?? n.pitch };
    const right = { ...n, start: cut, at: t, pitch: medianPitch(data, t, n.until) ?? n.pitch };
    // Each half keeps where the note was heading.
    left.shift = n.pitch + n.shift - left.pitch;
    right.shift = n.pitch + n.shift - right.pitch;
    if (!isEdited(n)) left.shift = right.shift = 0;
    picked = new Set([index + 1]);
    store([...notes.slice(0, index), left, right, ...notes.slice(index + 1)]);
  };
  const joinNext = (index) => {
    const n = notes[index], next = notes[index + 1];
    if (!next) { toast("That's the last note: there's nothing after it to join."); return; }
    const joined = { ...n, end: next.end, until: next.until };
    joined.pitch = medianPitch(data, n.at, next.until) ?? n.pitch;
    joined.shift = isEdited(n) ? n.pitch + n.shift - joined.pitch : 0;
    picked = new Set([index]);
    store([...notes.slice(0, index), joined, ...notes.slice(index + 2)]);
  };

  //--------------------------------------------------------------------------
  // Pointer.
  const noteAt = (event) => {
    const t = timeAt(event.clientX), m = midiAt(event.clientY);
    const rh = rowHeight();
    let best = -1, bestDistance = Infinity;
    notes.forEach((n, i) => {
      if (t < n.at || t > n.until) return;
      const d = Math.abs(shown(n) - m) * rh;
      if (d < Math.max(8, rh * 0.6) && d < bestDistance) { best = i; bestDistance = d; }
    });
    return best;
  };

  const inRuler = (event) => event.clientY - area.getBoundingClientRect().top < RULER;
  area.addEventListener("pointermove", (event) => {
    if (dragging) return;
    const i = inRuler(event) ? -1 : noteAt(event);
    if (i !== hover) { hover = i; paint(); }
    area.style.cursor = inRuler(event) ? "pointer" : cursorFor(i, event);
  });
  area.addEventListener("pointerleave", () => { if (!dragging && hover >= 0) { hover = -1; paint(); } });
  const cursorFor = (i, event) => {
    if (i < 0 || event.shiftKey) return i < 0 ? "crosshair" : "copy";
    return { select: "ns-resize", split: "col-resize", timing: "ew-resize" }[tool] ?? "default";
  };

  let dragging = false;
  const drag = (event, onMove, onUp) => {
    dragging = true;
    screen.dragging = true;
    area.setPointerCapture?.(event.pointerId);
    const move = (e) => onMove(e);
    const up = (e) => {
      area.removeEventListener("pointermove", move);
      area.removeEventListener("pointerup", up);
      area.removeEventListener("pointercancel", up);
      dragging = false;
      screen.dragging = false;
      onUp(e);
      screen.flushSession?.();
    };
    area.addEventListener("pointermove", move);
    area.addEventListener("pointerup", up);
    area.addEventListener("pointercancel", up);
  };

  // The song's cursor: the clip's rendered time t is at c.start + (t - c.offset) in the song.
  const songTime = (clientX) => { const c = clip(); return Math.max(0, c.start + timeAt(clientX) - c.offset); };
  const locate = (clientX, play = false) => {
    daw("transport", { action: "seek", time: songTime(clientX) });
    if (play && !screen.status?.playing) daw("transport", { action: "play" });
  };
  // The ruler: click (or drag) moves the cursor, double-click plays from there.
  const rulerPress = (event) => {
    if (event.detail >= 2) { locate(event.clientX, true); return; }
    locate(event.clientX);
    let last = 0;
    drag(event, (e) => {
      const now = performance.now();
      if (now - last < 40) return;
      last = now;
      locate(e.clientX);
    }, (e) => locate(e.clientX));
  };

  area.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || !data) return;
    event.preventDefault();
    element.focus({ preventScroll: true });
    if (inRuler(event)) { rulerPress(event); return; }
    // Double-click on empty space: play from there.
    const index = data.linked ? -1 : noteAt(event);
    if (index < 0 && event.detail >= 2) { locate(event.clientX, true); return; }
    if (data.linked) return;

    // With any tool: a drag across empty space picks notes (Shift adds to them), a click
    // there drops them; Shift+click a note picks it or drops it.
    if (index < 0) { rubberBand(event); return; }
    if (event.shiftKey) {
      if (picked.has(index)) picked.delete(index); else picked.add(index);
      paint();
      return;
    }

    if (tool === "split") {
      if (event.altKey) joinNext(index); else splitAt(index, timeAt(event.clientX));
      return;
    }
    if (!picked.has(index)) picked = new Set([index]);
    if (tool === "timing") { moveInTime(event, index); return; }

    if (event.detail >= 2) {
      const n = notes[index];
      const target = Math.round(shown(n));
      store(notes.map((x, i) => (i === index ? { ...x, shift: target - x.pitch - transpose() } : x)));
      return;
    }

    // Moving pitch: the grabbed note lands on a semitone, the other picked notes move with it.
    // Shift or Alt: free (cents). Ctrl/Cmd: whole semitones, keeping its offset from one.
    const chosen = [...picked];
    const before = notes.map((n) => ({ ...n }));
    const startY = event.clientY;
    const rh = rowHeight();
    let changed = false, heard = null;
    const live = livePreview(daw, screen, "previewPitchNotes", clip().id);
    const grabbed = before[index];
    drag(event, (e) => {
      const dy = startY - e.clientY;
      if (Math.abs(dy) > 2) changed = true;
      let delta = dy / rh;
      if (e.ctrlKey || e.metaKey) delta = Math.round(delta);
      else if (!e.altKey && !e.shiftKey) delta = Math.round(shown(grabbed) + delta) - shown(grabbed);
      preview = { offsets: new Map(chosen.map((i) => [i, delta])) };
      paint();
      const landed = Math.round((shown(grabbed) + delta) * 100);
      const moved = before.map((n, i) => (preview.offsets.has(i) ? { ...n, shift: clamp(n.shift + preview.offsets.get(i), -24, 24) } : n));
      if (landed !== heard && Math.abs(delta) > 1e-3) {
        heard = landed;
        audition(moved, index);
      }
      live.send({ notes: payload(moved) });
    }, () => {
      live.end(changed);
      const offsets = preview?.offsets ?? new Map();
      preview = null;
      if (!changed) { paint(); return; }
      store(before.map((n, i) => (offsets.has(i) ? { ...n, shift: clamp(n.shift + offsets.get(i), -24, 24) } : n)));
    });
  });

  // A box across the notes: picks the ones it touches (Shift: adds them to the picked ones).
  // A click without a drag drops the picked notes.
  const rubberBand = (event) => {
    const box = area.getBoundingClientRect();
    const x0 = event.clientX, y0 = event.clientY;
    const keep = event.shiftKey ? new Set(picked) : new Set();
    let moved = false;
    const place = (e) => {
      if (!moved && Math.abs(e.clientX - x0) + Math.abs(e.clientY - y0) < 4) return;
      moved = true;
      band.hidden = false;
      const left = Math.min(x0, e.clientX) - box.left, top = Math.max(RULER, Math.min(y0, e.clientY) - box.top);
      Object.assign(band.style, { left: `${left}px`, top: `${top}px`, width: `${Math.abs(e.clientX - x0)}px`, height: `${Math.max(0, Math.max(y0, e.clientY) - box.top - top)}px` });
      const t0 = timeAt(Math.min(x0, e.clientX)), t1 = timeAt(Math.max(x0, e.clientX));
      const m0 = midiAt(Math.max(y0, e.clientY)), m1 = midiAt(Math.min(y0, e.clientY));
      picked = new Set(keep);
      notes.forEach((n, i) => { if (n.until >= t0 && n.at <= t1 && shown(n) >= m0 - 0.5 && shown(n) <= m1 + 0.5) picked.add(i); });
      paint();
    };
    drag(event, place, () => {
      band.hidden = true;
      if (!moved && !event.shiftKey) picked.clear();
      paint();
    });
  };

  // Timing: the picked notes' starts move together; the starts of the notes either side of
  // them stay put (warp markers, as the waveform view makes them).
  const moveInTime = (event, index) => {
    const c = clip();
    const markers = (c.warp ?? []).map((m) => [...m]);
    const [a, b] = span();
    const perPx = (b - a) / width();
    if (!markers.length) markers.push([c.offset, c.offset], [c.offset + c.length, c.offset + c.length]);
    const markerAt = (t) => {
      const near = markers.find(([, m]) => Math.abs(m - t) / perPx < 3);
      if (near) return near;
      const added = [toSource(markers, t), t];
      markers.push(added);
      markers.sort((x, y) => x[0] - y[0]);
      return added;
    };
    const moving = [...picked].sort((x, y) => x - y);
    const pins = new Set();
    for (const i of moving) for (const j of [i - 1, i + 1]) if (notes[j] && !picked.has(j)) pins.add(markerAt(notes[j].at));
    const movers = new Map(); // marker -> where it was
    for (const i of moving) { const m = markerAt(notes[i].at); if (!pins.has(m)) movers.set(m, m[1]); }
    const grabbedMarker = markers.find(([, m]) => Math.abs(m - notes[index].at) / perPx < 3);
    if (!movers.size || !grabbedMarker || !movers.has(grabbedMarker)) return;
    // How far they can go: not past a marker that stays.
    let low = -Infinity, high = Infinity;
    markers.forEach((m, k) => {
      if (!movers.has(m)) return;
      let p = k - 1; while (p >= 0 && movers.has(markers[p])) --p;
      let q = k + 1; while (q < markers.length && movers.has(markers[q])) ++q;
      if (p >= 0) low = Math.max(low, markers[p][1] + 0.01 - m[1]);
      if (q < markers.length) high = Math.min(high, markers[q][1] - 0.01 - m[1]);
    });
    const grabbed = grabbedMarker[1];
    const startX = event.clientX;
    let delta = 0;
    const live = livePreview(daw, screen, "previewWarp", c.id);
    const moved = () => markers.map((m) => (movers.has(m) ? [m[0], movers.get(m) + delta] : m));
    drag(event, (e) => {
      const free = grabbed + (e.clientX - startX) * perPx;
      const snapped = screen.snapTime ? screen.snapTime(c.start + (free - c.offset), e) - c.start + c.offset : free;
      delta = clamp(snapped - grabbed, low, high);
      preview = { times: new Map(moving.map((i) => [i, delta])) };
      live.send({ markers: moved() });
      host.setStatus?.(`Moving ${moving.length > 1 ? `${moving.length} notes` : noteName(shown(notes[index]))} ${signed(delta * 1000)} ms`);
      paint();
    }, () => {
      preview = null;
      host.setStatus?.("");
      const changed = Math.abs(delta) > 1e-4;
      live.end(changed);
      if (changed) daw("setWarp", { id: c.id, markers: moved() });
      paint();
    });
  };

  // Scroll moves along, Ctrl/Cmd + scroll zooms time, Shift + scroll moves the pitch rows,
  // Alt + scroll zooms them.
  area.addEventListener("wheel", (event) => {
    event.preventDefault();
    if (!rows) return;
    if (event.shiftKey || event.altKey) {
      const steps = Math.sign(event.deltaY || event.deltaX);
      if (event.altKey) {
        const size = clamp(rows[1] - rows[0] + steps * 2, 8, 60), middle = (rows[0] + rows[1]) / 2;
        rows = [Math.round(middle - size / 2), Math.round(middle - size / 2) + size];
      } else {
        rows = [rows[0] - steps * 2, rows[1] - steps * 2];
      }
      const size = rows[1] - rows[0];
      rows[0] = clamp(rows[0], 0, 127 - size);
      rows[1] = rows[0] + size;
      paint();
      return;
    }
    const [a, b] = span();
    if (event.ctrlKey || event.metaKey) {
      const at = timeAt(event.clientX), factor = Math.pow(1.0015, event.deltaY);
      view = [at - (at - a) * factor, at + (b - at) * factor];
    } else {
      const move = (event.deltaX || event.deltaY) / width() * (b - a);
      view = [a + move, b + move];
    }
    view = span();
    paint();
  }, { passive: false });

  lane.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || !data || data.linked) return;
    event.preventDefault();
    const t = timeAt(event.clientX);
    const index = notes.findIndex((n) => t >= n.at && t <= n.until);
    if (index < 0) return;
    if (!picked.has(index)) picked = new Set([index]);
    const box = lane.getBoundingClientRect();
    const before = notes.map((n) => ({ ...n }));
    const live = livePreview(daw, screen, "previewPitchNotes", clip().id);
    const set = (e) => {
      const gain = Math.round(gainAtY(e.clientY - box.top, box.height) * 10) / 10;
      const delta = gain - before[index].gain;
      notes = before.map((n, i) => (picked.has(i) ? { ...n, gain: clamp(Math.round((n.gain + delta) * 10) / 10, -24, 12) } : n));
      live.send({ notes: payload(notes) });
      paint();
    };
    set(event);
    dragging = true;
    lane.setPointerCapture?.(event.pointerId);
    const up = () => {
      lane.removeEventListener("pointermove", set);
      lane.removeEventListener("pointerup", up);
      lane.removeEventListener("pointercancel", up);
      dragging = false;
      live.end(true);
      store(notes);
    };
    lane.addEventListener("pointermove", set);
    lane.addEventListener("pointerup", up);
    lane.addEventListener("pointercancel", up);
  });

  // Keys, while the editor has focus.
  element.addEventListener("keydown", (event) => {
    if (event.target !== element) return;
    const key = event.key.toLowerCase();
    const mod = event.ctrlKey || event.metaKey;
    const handled = () => { event.preventDefault(); event.stopPropagation(); };
    const shortcut = TOOLS.find(([, , s]) => s.toLowerCase() === key);
    if (!mod && !event.altKey && shortcut) { handled(); setTool(shortcut[0]); }
    else if (mod && key === "a") { handled(); picked = new Set(notes.map((_, i) => i)); paint(); }
    else if (key === "escape") { handled(); picked.clear(); paint(); }
    else if (!mod && key === "s") { handled(); snapNotes(); }
    else if ((key === "arrowup" || key === "arrowdown") && picked.size) {
      handled();
      // A semitone, or 10 cents with Shift (or Alt).
      const step = (key === "arrowup" ? 1 : -1) * (event.altKey || event.shiftKey ? 0.1 : 1);
      const chosen = new Set(picked);
      storeSoon(notes.map((n, i) => (chosen.has(i) ? { ...n, shift: clamp(Math.round((n.shift + step) * 100) / 100, -24, 24) } : n)));
      audition(notes, [...chosen][0]);
    }
  });

  const setTool = (key) => {
    tool = key;
    prefs.set("tool", key);
    for (const [k, b] of toolButtons) b.classList.toggle("is-active", k === key);
    paint();
  };
  setTool(tool);

  const resize = new ResizeObserver(() => paint());
  resize.observe(area);
  resize.observe(lane);

  return {
    element,
    // The clip changed (or its render did): fetch its notes again when what they depend on changed.
    update: () => { fetchData(); paint(); },
    // Called when the view is shown again.
    shown: () => { fetchData(); requestAnimationFrame(paint); },
    fit: () => { view = null; fitRows(); paint(); },
    dispose: () => { flushNudges(); disposed = true; cancelAnimationFrame(frame); resize.disconnect(); },
  };
}

function roundRect(g, x, y, w, h, r) {
  const radius = Math.min(r, w / 2, h / 2);
  g.beginPath();
  g.moveTo(x + radius, y);
  g.arcTo(x + w, y, x + w, y + h, radius);
  g.arcTo(x + w, y + h, x, y + h, radius);
  g.arcTo(x, y + h, x, y, radius);
  g.arcTo(x, y, x + w, y, radius);
  g.closePath();
}
