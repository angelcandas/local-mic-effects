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
import { toSource } from "./rhythm.js";

const KEYS = 40;     // the keyboard's width
const RULER = 16;    // bar and beat numbers on top
const LANE = 54;     // the loudness lane's height

// Tools: [key, name, shortcut, what it does, icon path (24x24, stroked)].
const TOOLS = [
  ["select", "Select", "V", "Click a note to pick it (Shift adds), drag it up or down to move it (snaps to semitones; hold Shift or Alt for cents, Ctrl/Cmd for whole semitones from where it is). Up/Down arrows move picked notes a semitone, Shift+arrow 10 cents. Drag empty space to pick several. Double-click a note to put it right on its semitone",
    '<path d="M6 3l12 9-5.5 1.2L16 20l-2.6 1.2-2.7-6.4L6 19z"/>'],
  ["split", "Split", "C", "Click inside a note to cut it in two there; Alt-click a note to join it to the next one",
    '<circle cx="6" cy="7" r="2.6"/><circle cx="6" cy="17" r="2.6"/><path d="M8.2 8.4L20 17M8.2 15.6L20 7"/>'],
  ["drift", "Drift", "D", "Drag a note up to straighten the slow wander of its pitch (100% = flat on the note), down to give it back",
    '<path d="M3 15c3-6 5 4 8-2s4-3 5-1"/><path d="M14 9h7"/>'],
  ["vibrato", "Vibrato", "M", "Drag a note up to widen its vibrato, down to calm it (0% = none, 200% = twice as wide)",
    '<path d="M2 12c1.5-5 3-5 4.5 0s3 5 4.5 0 3-5 4.5 0 3 5 4.5 0 2-3 2-3"/>'],
  ["transition", "Glide", "T", "Drag a note left or right to make its glide in from the note before quicker or slower",
    '<path d="M3 17h5c4 0 4-10 8-10h5"/>'],
  ["formant", "Formant", "F", "Drag a note up to make the voice smaller and brighter, down to make it bigger and darker, at the same pitch",
    '<path d="M3 18c3 0 3-10 6-10s3 10 6 10 3-6 6-6"/><path d="M12 3v3"/>'],
  ["level", "Level", "L", "Drag a note up or down to make it louder or softer (also in the loudness lane)",
    '<path d="M5 20V12M10 20V8M15 20V4M20 20V10"/>'],
  ["timing", "Timing", "W", "Drag a note left or right to move it in time (the notes either side stay put); the clip is warped",
    '<path d="M4 12h16M8 8l-4 4 4 4M16 8l4 4-4 4"/>'],
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
  const toKey = action("Snap", "Put the picked notes (or all of them) right on their semitone, or on the nearest note of the song's key when it's set (S)", () => snapNotes());
  const straighten = action("Straighten", "Take the drift out of the picked notes (or all of them), keeping their vibrato", () => editPicked((n) => ({ ...n, straighten: 1 })));
  const reset = action("Reset", "Put the picked notes (or all of them) back as sung", () => resetNotes());
  const listenBox = html("input");
  listenBox.type = "checkbox";
  listenBox.checked = prefs.get("listen", true);
  listenBox.addEventListener("change", () => prefs.set("listen", listenBox.checked));
  const listen = html("label", "rhythm-check note-listen");
  listen.title = "Hear a note as you move it: it plays when it lands on a new semitone, and after each arrow key (not while the song plays)";
  listen.append(listenBox, document.createTextNode(" Listen"));
  const readout = html("span", "note-readout");
  bar.append(tools, toKey, straighten, reset, listen, readout);

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
  element.append(bar, area, lane, message);

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

    // Bars and beats, from the project's tempo (the clip's time on the song).
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
      const moved = preview?.shiftTime?.[0] === i ? preview.shiftTime[1] : 0;
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

      // Name inside, and the active tool's value above.
      g.font = "10px system-ui, sans-serif";
      const name = `${noteName(shown(n) + extra)}${centsText(shown(n) + extra)}`;
      if (x1 - x0 > g.measureText(name).width + 12 && hgt >= 10) {
        g.fillStyle = "rgba(10,12,18,0.85)";
        g.font = `${Math.min(10, hgt - 2)}px system-ui, sans-serif`;
        g.fillText(name, x0 + 9, y + Math.min(10, hgt - 2) / 2 - 1);
      }
      const value = toolValue(n);
      if (value && (isPicked || i === hover)) {
        g.fillStyle = "rgba(255,255,255,0.85)";
        g.fillText(value, x0 + 2, y - hgt / 2 - 3);
      }

      // The glide in: a short slope at the note's start.
      if (tool === "transition" && (isPicked || i === hover)) {
        const gx = xOf(n.at - n.transition / 2), gx1 = xOf(n.at + n.transition / 2);
        g.strokeStyle = "rgba(255,255,255,0.8)";
        g.lineWidth = 1.5;
        g.beginPath();
        g.moveTo(gx, y + hgt / 2 + 3);
        g.lineTo(gx1, y - hgt / 2 - 1);
        g.stroke();
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
      placePlayhead();
    }
    frame = requestAnimationFrame(tick);
  };
  frame = requestAnimationFrame(tick);

  // The active tool's value for a note, shown above it.
  const toolValue = (n) => {
    switch (tool) {
      case "drift": return `drift ${Math.round(n.straighten * 100)}%`;
      case "vibrato": return `vibrato ${Math.round(n.vibrato * 100)}%`;
      case "formant": return `formant ${signed(n.formant, 1)}`;
      case "level": return `${signed(n.gain, 1)} dB`;
      case "transition": return `glide ${Math.round(n.transition * 1000)} ms`;
      default: return "";
    }
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

  area.addEventListener("pointermove", (event) => {
    if (dragging) return;
    const i = noteAt(event);
    if (i !== hover) { hover = i; paint(); }
    area.style.cursor = cursorFor(i);
  });
  area.addEventListener("pointerleave", () => { if (!dragging && hover >= 0) { hover = -1; paint(); } });
  const cursorFor = (i) => {
    if (i < 0) return tool === "select" ? "crosshair" : "default";
    return { select: "ns-resize", split: "col-resize", transition: "ew-resize", timing: "ew-resize" }[tool] ?? "ns-resize";
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

  area.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || !data || data.linked) return;
    event.preventDefault();
    element.focus({ preventScroll: true });
    const index = noteAt(event);

    if (tool === "split") {
      if (index < 0) return;
      if (event.altKey) joinNext(index); else splitAt(index, timeAt(event.clientX));
      return;
    }
    if (tool === "timing") { if (index >= 0) moveInTime(event, index); return; }

    if (index < 0) {
      if (tool === "select") rubberBand(event);
      else if (!event.shiftKey && picked.size) { picked.clear(); paint(); }
      return;
    }

    if (event.shiftKey) {
      if (picked.has(index)) picked.delete(index); else picked.add(index);
      paint();
      return;
    }
    if (!picked.has(index)) picked = new Set([index]);

    if (tool === "select" && event.detail >= 2) {
      const n = notes[index];
      const target = Math.round(shown(n));
      store(notes.map((x, i) => (i === index ? { ...x, shift: target - x.pitch - transpose() } : x)));
      return;
    }

    const chosen = [...picked];
    const before = notes.map((n) => ({ ...n }));
    const startX = event.clientX, startY = event.clientY;
    const rh = rowHeight();
    let changed = false, heard = null;

    drag(event, (e) => {
      const dy = (startY - e.clientY), dx = e.clientX - startX;
      if (Math.abs(dx) + Math.abs(dy) > 2) changed = true;
      if (tool === "select") {
        // Moving pitch: the grabbed note lands on a semitone, the rest move with it.
        // Shift or Alt: free (cents). Ctrl/Cmd: whole semitones, keeping its offset from one.
        const grabbed = before[index];
        let delta = dy / rh;
        if (e.ctrlKey || e.metaKey) delta = Math.round(delta);
        else if (!e.altKey && !e.shiftKey) delta = Math.round(shown(grabbed) + delta) - shown(grabbed);
        preview = { offsets: new Map(chosen.map((i) => [i, delta])) };
        paint();
        const landed = Math.round((shown(grabbed) + delta) * 100);
        if (landed !== heard && Math.abs(delta) > 1e-3) {
          heard = landed;
          audition(before.map((n, i) => (preview.offsets.has(i) ? { ...n, shift: clamp(n.shift + preview.offsets.get(i), -24, 24) } : n)), index);
        }
        return;
      }
      notes = before.map((n, i) => (picked.has(i) ? adjust(n, dx, dy, e) : n));
      paint();
    }, (e) => {
      if (!changed) { preview = null; paint(); return; }
      if (tool === "select") {
        const offsets = preview?.offsets ?? new Map();
        preview = null;
        store(before.map((n, i) => (offsets.has(i) ? { ...n, shift: clamp(n.shift + offsets.get(i), -24, 24) } : n)));
        return;
      }
      const next = before.map((n, i) => (picked.has(i) ? adjust(n, e.clientX - startX, startY - e.clientY, e) : n));
      store(next);
    });
  });

  // A tool's drag on one note: dx, dy in pixels (up is positive).
  const adjust = (n, dx, dy, e) => {
    const fine = e.altKey ? 0.2 : 1;
    switch (tool) {
      case "drift": return { ...n, straighten: clamp(n.straighten + dy * fine / 120, 0, 1) };
      case "vibrato": return { ...n, vibrato: clamp(n.vibrato + dy * fine / 80, 0, 2) };
      case "formant": return { ...n, formant: Math.round(clamp(n.formant + dy * fine / 12, -12, 12) * 10) / 10 };
      case "level": return { ...n, gain: Math.round(clamp(n.gain + dy * fine / 6, -24, 12) * 10) / 10 };
      case "transition": {
        const [a, b] = span();
        return { ...n, transition: clamp(n.transition + dx * fine * (b - a) / width(), 0.005, 0.5) };
      }
      default: return n;
    }
  };

  const rubberBand = (event) => {
    const box = area.getBoundingClientRect();
    const x0 = event.clientX, y0 = event.clientY;
    const keep = event.shiftKey ? new Set(picked) : new Set();
    band.hidden = false;
    const place = (e) => {
      const left = Math.min(x0, e.clientX) - box.left, top = Math.min(y0, e.clientY) - box.top;
      Object.assign(band.style, { left: `${left}px`, top: `${top}px`, width: `${Math.abs(e.clientX - x0)}px`, height: `${Math.abs(e.clientY - y0)}px` });
      const t0 = timeAt(Math.min(x0, e.clientX)), t1 = timeAt(Math.max(x0, e.clientX));
      const m0 = midiAt(Math.max(y0, e.clientY)), m1 = midiAt(Math.min(y0, e.clientY));
      picked = new Set(keep);
      notes.forEach((n, i) => { if (n.until >= t0 && n.at <= t1 && shown(n) >= m0 - 0.5 && shown(n) <= m1 + 0.5) picked.add(i); });
      paint();
    };
    place(event);
    drag(event, place, () => { band.hidden = true; paint(); });
  };

  // Timing: the note's start moves; the starts of the notes either side stay put (warp
  // markers, as the waveform view makes them).
  const moveInTime = (event, index) => {
    const c = clip();
    const markers = (c.warp ?? []).map((m) => [...m]);
    const [a, b] = span();
    const perPx = (b - a) / width();
    if (!markers.length) markers.push([c.offset, c.offset], [c.offset + c.length, c.offset + c.length]);
    const pin = (t) => {
      if (t === undefined || markers.some(([, m]) => Math.abs(m - t) / perPx < 3)) return;
      markers.push([toSource(markers, t), t]);
      markers.sort((x, y) => x[0] - y[0]);
    };
    pin(notes[index - 1]?.at);
    pin(notes[index + 1]?.at);
    const grabbed = notes[index].at;
    let at = markers.findIndex(([, t]) => Math.abs(t - grabbed) / perPx < 3);
    if (at < 0) {
      markers.push([toSource(markers, grabbed), grabbed]);
      markers.sort((x, y) => x[0] - y[0]);
      at = markers.findIndex(([, t]) => t === grabbed);
    }
    const low = at > 0 ? markers[at - 1][1] + 0.01 : -Infinity;
    const high = at < markers.length - 1 ? markers[at + 1][1] - 0.01 : Infinity;
    const startX = event.clientX;
    let target = grabbed;
    drag(event, (e) => {
      const free = grabbed + (e.clientX - startX) * perPx;
      const snapped = screen.snapTime ? screen.snapTime(c.start + (free - c.offset), e) - c.start + c.offset : free;
      target = clamp(snapped, low, high);
      preview = { shiftTime: [index, target - grabbed] };
      host.setStatus?.(`Moving ${noteName(shown(notes[index]))} ${signed((target - grabbed) * 1000)} ms`);
      paint();
    }, () => {
      preview = null;
      host.setStatus?.("");
      if (Math.abs(target - grabbed) > 1e-4) {
        markers[at][1] = target;
        daw("setWarp", { id: c.id, markers });
      }
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
    const set = (e) => {
      const gain = Math.round(gainAtY(e.clientY - box.top, box.height) * 10) / 10;
      const delta = gain - before[index].gain;
      notes = before.map((n, i) => (picked.has(i) ? { ...n, gain: clamp(Math.round((n.gain + delta) * 10) / 10, -24, 12) } : n));
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
