// The pattern looper in the DAW screen: the Patterns tab (a rack of rows with step buttons, as
// in a drum machine) and the Patterns lane under the ruler (blocks that place a pattern on the
// song). The model and the engine are in src/daw/Patterns.cpp; every gesture here is one
// command (one undo step) and the page redraws from the session that comes back.
//
// Patterns are stored in BEATS (the rest of the project is in seconds); a step is `pattern.step`
// beats (a 16th by default). A row is one pitch of a track's part: a drum track (an instrument
// with named keys such as Beat or Kit) has a row per sound, any other MIDI track one row on its
// "row note".
import { clamp, html } from "./widgets.js";
import { noteName, PianoRoll } from "./pianoroll.js";

const COLOURS = ["#ff8a5c", "#3fd9c6", "#8c7bff", "#ffb547", "#ff6fb1", "#5aafff", "#9be15d", "#c78bff"];
export const patternColour = (pattern, index = 0) => (/^#[0-9a-f]{6}$/i.test(pattern?.colour ?? "") ? pattern.colour : COLOURS[(pattern?.id ?? index) % COLOURS.length]);

// The General MIDI drum names: labels for a drum plug-in (or an instrument without key names).
const GM = {
  35: "Kick 2", 36: "Kick", 37: "Side stick", 38: "Snare", 39: "Clap", 40: "Snare 2", 41: "Tom 1", 42: "Hat", 43: "Tom 2", 44: "Pedal hat",
  45: "Tom 3", 46: "Open hat", 47: "Tom 4", 48: "Tom 5", 49: "Crash", 50: "Tom 6", 51: "Ride", 52: "China", 53: "Ride bell", 54: "Tambourine",
  55: "Splash", 56: "Cowbell", 57: "Crash 2", 59: "Ride 2", 60: "Bongo", 61: "Bongo 2", 62: "Conga", 63: "Conga 2", 64: "Conga 3",
  69: "Cabasa", 70: "Maracas", 75: "Claves", 76: "Block", 77: "Block 2",
};
const START_ROWS = [36, 38, 39, 42, 46]; // Kick, Snare, Clap, Hat, Open
const STEP_SIZES = [[1, "1/4"], [0.5, "1/8"], [0.25, "1/16"], [1 / 6, "1/16 triplet"], [0.125, "1/32"]];
const STEP_W = 24, STEP_GAP = 2;
const DEFAULT_NOTE = 60;
const STEP_VELOCITY = 100;

// A row's name in the rack.
const soundName = (track, pitch) => track.keyNames?.[String(pitch)] || GM[pitch] || noteName(pitch);

// The track's view of a pattern: its part and the rows to show. A drum track shows the part's
// rows (a starter set when it has none, plus any pitch that already holds notes); other tracks
// one row on the row note, or a preview when the part holds more than steps.
export function trackRows(track, part) {
  const named = Object.keys(track.keyNames ?? {}).length > 0;
  const drum = named || Boolean(part?.drum);
  const notes = part?.notes ?? [];
  if (drum) {
    let pitches = part?.rows?.length ? [...part.rows] : START_ROWS.filter((p) => (named ? track.keyNames[String(p)] : true));
    if (!pitches.length) pitches = Object.keys(track.keyNames ?? {}).map(Number).sort((a, b) => a - b).slice(0, 5);
    for (const note of notes) if (!pitches.includes(note.pitch)) pitches.push(note.pitch);
    return { drum: true, named, rows: pitches.map((pitch) => ({ pitch, name: soundName(track, pitch) })) };
  }
  return { drum: false, named, rows: [{ pitch: part?.stepNote ?? DEFAULT_NOTE, name: track.name }] };
}

const stepCount = (pattern) => Math.max(1, Math.round(pattern.length / pattern.step - 1e-6));
// The step a note sits on: up to a twentieth of a step early counts as on it, later is a shifted
// step (as Edit::getStepIndex).
const noteIndex = (note, pattern) => Math.floor(note.time / pattern.step + 0.05);
const MAX_SHIFT = 0.9, MIN_LEN = 0.1, MAX_LEN = 4; // step values, in steps
let stepClipboard = null; // "Copy steps": [{ index, velocity, length, shift (steps), offset (semitones) }]

// Can a track row's steps show the part? Each step holds one note of any pitch; chords, two notes
// on a step or notes past the end need the piano roll (the rack shows a preview).
function isMelody(part, pattern) {
  if (!part?.notes?.length) return false;
  const seen = new Set(), count = stepCount(pattern);
  for (const n of part.notes) {
    const i = noteIndex(n, pattern);
    if (i < 0 || i >= count || seen.has(i)) return true;
    seen.add(i);
  }
  return false;
}

//==================================================================================== the tab
// screen: the DAW screen (session, status, selectedPattern, tempoMap, beatsPerBar, setDockTab,
// popMenu). Returns { element, update(session), setStatus(status), ensurePattern() }.
export function createPatternsPanel(screen, daw, { popMenu, toast = () => {} }) {
  const element = html("section", "patterns-panel");
  let signature = "";
  let columns = [];       // the step buttons of each column, for the playing light
  let lit = -1;
  let rollTrack = null;   // a track id while the piano roll shows that track's part
  let roll = null;        // { roll, element, key } of the open piano roll
  let rollKey = "";

  const selected = () => {
    const list = screen.session?.patterns ?? [];
    return list.find((p) => p.id === screen.selectedPattern) ?? list[0] ?? null;
  };

  const choose = (id) => {
    if (id == null || id < 0) return;
    screen.selectedPattern = id;
    screen.rememberView?.();
    // In Pattern mode the loop follows the pattern you look at.
    if ((screen.session?.patternMode ?? -1) >= 0 && screen.session.patternMode !== id) daw("patternMode", { pattern: id });
    render(true);
    screen.renderTransport?.();
  };

  const midiTracks = () => (screen.session?.tracks ?? []).filter((t) => t.kind === "midi");

  const newPattern = async (from) => {
    const list = screen.session?.patterns ?? [];
    // A new one takes the first unused number.
    const names = new Set(list.map((p) => p.name));
    let number = 1;
    while (names.has(`Pattern ${number}`)) ++number;
    const id = await daw("addPattern", from != null ? { from } : { name: `Pattern ${number}` });
    if (typeof id === "number") {
      // The session that holds it arrives next and draws it.
      screen.selectedPattern = id;
      if ((screen.session?.patternMode ?? -1) >= 0) daw("patternMode", { pattern: id });
    }
    return id;
  };

  // A pattern to work in: the selected one, or a new one. Used by the Pattern switch.
  const ensurePattern = async () => selected()?.id ?? newPattern();

  const addInstrumentTrack = async (instrument) => {
    const result = await daw("addMidiTrack", instrument ? { instrument: instrument.id, name: instrument.name } : {});
    if (typeof result === "number") screen.showWhenDrawn?.({ track: result });
    return result;
  };

  // An empty project: a Beat drum track and a first beat.
  const startWithKit = async () => {
    const found = await daw("instruments");
    const beat = (found?.instruments ?? []).find((i) => i.name === "Beat");
    const track = await addInstrumentTrack(beat);
    if (typeof track !== "number") return;
    const id = (await ensurePattern()) ?? -1;
    if (id < 0) return;
    await daw("setPart", { pattern: id, track, drum: true, rows: START_ROWS });
    for (const [pitch, steps] of [[36, [0, 8, 10]], [38, [4, 12]], [42, [0, 2, 4, 6, 8, 10, 12, 14]]])
      await daw("setSteps", { pattern: id, track, pitch, steps: steps.map((i) => [i, STEP_VELOCITY]) });
    screen.selectedPattern = id;
    screen.render?.();
  };

  const instrumentMenu = async (anchor) => {
    const found = await daw("instruments");
    const list = found?.instruments ?? [];
    const box = anchor.getBoundingClientRect();
    popMenu(box.left, box.bottom + 2, ({ item, heading }) => {
      heading("Add a MIDI track with…");
      if (!list.length) item("No instruments found yet", () => {}, { disabled: true });
      for (const instrument of list) item(instrument.name, () => addInstrumentTrack(instrument), { hint: instrument.vendor || instrument.kind });
    }, anchor);
  };

  //---------------------------------------------------------------- row actions
  const partOf = (pattern, trackId) => pattern.parts.find((p) => p.track === trackId);
  const notesAt = (part, pitch) => (part?.notes ?? []).filter((n) => n.pitch === pitch);
  // The notes a row's steps show: a drum row's pitch, or every note of a track row (a note per step).
  const rowNotes = (part, view, row) => (view.drum ? notesAt(part, row.pitch) : part?.notes ?? []);

  const setSteps = (pattern, track, view, row, steps) =>
    daw("setSteps", { pattern: pattern.id, track: track.id, pitch: row.pitch, steps, ...(view.drum ? {} : { anyPitch: true }) });

  // What the row menu and the keys do to one row.
  const rowActions = (pattern, track, view, row) => {
    const part = partOf(pattern, track.id);
    const count = stepCount(pattern);
    const step = pattern.step;
    const notes = rowNotes(part, view, row);
    const byIndex = new Map(notes.map((n) => [noteIndex(n, pattern), n]));
    const edit = (args) => daw("editPattern", { pattern: pattern.id, track: track.id, ...args });
    return {
      fill: (every) => setSteps(pattern, track, view, row, Array.from({ length: count }, (_, i) => [i, i % every === 0 ? STEP_VELOCITY : 0])),
      rotate: (by) => edit({ change: notes.map((n) => ({ id: n.id, time: ((((n.time + by * step) % pattern.length) + pattern.length) % pattern.length) })) }),
      clear: () => edit({ remove: notes.map((n) => n.id) }),
      // Each step on with the chance `density`, velocities a little apart (FL's randomize).
      randomize: (density) => setSteps(pattern, track, view, row, Array.from({ length: count },
        (_, i) => [i, Math.random() < density ? Math.round(STEP_VELOCITY - 18 + Math.random() * 27) : 0])),
      flip: () => setSteps(pattern, track, view, row, Array.from({ length: count }, (_, i) => [i, byIndex.has(i) ? 0 : STEP_VELOCITY])),
      copy: () => {
        stepClipboard = [...byIndex].map(([index, n]) => ({
          index, velocity: n.velocity, length: n.length / step, shift: clamp(n.time / step - index, 0, MAX_SHIFT), offset: n.pitch - row.pitch,
        }));
        toast(`Copied ${stepClipboard.length} step${stepClipboard.length === 1 ? "" : "s"} of ${row.name}`);
      },
      // The copied steps replace the row's, one undo step. A drum row plays its own sound.
      paste: () => {
        if (!stepClipboard) return;
        edit({
          remove: notes.map((n) => n.id),
          add: stepClipboard.filter((s) => s.index < count).map((s) => ({
            pitch: clamp(row.pitch + (view.drum ? 0 : s.offset), 0, 127), time: (s.index + s.shift) * step,
            length: s.length * step, velocity: s.velocity,
          })),
        });
      },
    };
  };

  const rowMenu = (event, pattern, track, view, row) => {
    event.preventDefault();
    const part = partOf(pattern, track.id);
    const act = rowActions(pattern, track, view, row);
    const edit = (change) => daw("editPattern", { pattern: pattern.id, track: track.id, change });
    selectRow(track, row.pitch, false);
    popMenu(event.clientX, event.clientY, ({ item, rule, heading }) => {
      heading(row.name);
      item("Fill every 2 steps", () => act.fill(2));
      item("Fill every 4 steps", () => act.fill(4));
      item("Fill every 8 steps", () => act.fill(8));
      item("Randomize: a few steps", () => act.randomize(0.25), { hint: "Turns about a quarter of the steps on, with slightly different velocities" });
      item("Randomize: half the steps", () => act.randomize(0.5));
      item("Randomize: most steps", () => act.randomize(0.75));
      item("Flip", () => act.flip(), { hint: "Steps that are on go off, and the other way round" });
      rule();
      item("Rotate left", () => act.rotate(-1), { shortcut: "Ctrl+Shift+Left" });
      item("Rotate right", () => act.rotate(1), { shortcut: "Ctrl+Shift+Right" });
      item("Copy steps", () => act.copy(), { shortcut: "Ctrl+C" });
      item("Paste steps", () => act.paste(), { shortcut: "Ctrl+V", disabled: !stepClipboard });
      item("Clear", () => act.clear());
      rule();
      if (!view.drum || !view.named) {
        // The row's note: "rows per note" for a plug-in, or the base note of a track row.
        const move = async (delta) => {
          const to = clamp(row.pitch + delta, 0, 127);
          if (view.drum) {
            const rows = view.rows.map((r) => (r.pitch === row.pitch ? to : r.pitch));
            await daw("setPart", { pattern: pattern.id, track: track.id, drum: true, rows });
            await edit(notesAt(part, row.pitch).map((n) => ({ id: n.id, pitch: to })));
          } else {
            // Every step moves with the row, so a melody keeps its shape.
            await daw("setPart", { pattern: pattern.id, track: track.id, stepNote: to });
            await edit((part?.notes ?? []).map((n) => ({ id: n.id, pitch: clamp(n.pitch + delta, 0, 127) })));
          }
        };
        item(`Note ${noteName(row.pitch)}: up a semitone`, () => move(1));
        item("Down a semitone", () => move(-1));
        item("Up an octave", () => move(12));
        item("Down an octave", () => move(-12));
        rule();
      }
      if (view.drum) {
        item("Remove row", async () => {
          await daw("editPattern", { pattern: pattern.id, track: track.id, remove: notesAt(part, row.pitch).map((n) => n.id) });
          await daw("setPart", { pattern: pattern.id, track: track.id, drum: true, rows: view.rows.filter((r) => r.pitch !== row.pitch).map((r) => r.pitch) });
        }, { danger: true });
      }
    });
  };

  const trackMenu = (event, pattern, track, view) => {
    event.preventDefault();
    const part = partOf(pattern, track.id);
    popMenu(event.clientX, event.clientY, ({ item, heading }) => {
      heading(track.name);
      if (!view.named) item("Rows per note (drum view)", () => daw("setPart", { pattern: pattern.id, track: track.id, drum: !view.drum,
        ...(view.drum ? {} : { rows: [part?.stepNote ?? DEFAULT_NOTE] }) }), { checked: view.drum });
      item("Edit notes in the piano roll", () => openRoll(track));
      item("Show this track in the Track tab", () => { screen.select?.(track.id, null); screen.setDockTab("effects"); });
      item("Clear this track in the pattern", () => daw("editPattern", { pattern: pattern.id, track: track.id, remove: (part?.notes ?? []).map((n) => n.id) }));
    });
  };

  const addSoundMenu = (event, pattern, track, view) => {
    const box = event.currentTarget.getBoundingClientRect();
    const have = new Set(view.rows.map((r) => r.pitch));
    const named = Object.keys(track.keyNames ?? {}).map(Number);
    const choices = (named.length ? named : Object.keys(GM).map(Number)).filter((p) => !have.has(p)).sort((a, b) => a - b);
    popMenu(box.left, box.bottom + 2, ({ item, heading }) => {
      heading("Add a sound");
      if (!choices.length) item("Every sound is here", () => {}, { disabled: true });
      for (const pitch of choices)
        item(soundName(track, pitch), () => daw("setPart", { pattern: pattern.id, track: track.id, drum: true, rows: [...view.rows.map((r) => r.pitch), pitch] }),
          { shortcut: noteName(pitch) });
    }, event.currentTarget);
  };

  //---------------------------------------------------------------- mute, solo, level and pan
  // The tracks the rack shows: all MIDI tracks, or only those with notes in the pattern.
  const visibleTracks = (pattern) => {
    const all = midiTracks();
    if (!screen.patternOnlyUsed || !pattern) return all;
    const used = all.filter((t) => partOf(pattern, t.id)?.notes?.length);
    return used.length ? used : all;
  };

  // A drum row mutes on its own (the part's `muted` list); a track row mutes the track's part.
  const rowMuted = (part, view, row) => (view.drum ? (part?.muted ?? []).includes(row.pitch) : Boolean(part?.mute));

  const toggleMute = (pattern, track, view, row) => {
    const part = partOf(pattern, track.id);
    if (!view.drum) return daw("setPart", { pattern: pattern.id, track: track.id, mute: !part?.mute });
    const muted = new Set(part?.muted ?? []);
    if (muted.has(row.pitch)) muted.delete(row.pitch); else muted.add(row.pitch);
    return daw("setPart", { pattern: pattern.id, track: track.id, muted: [...muted] });
  };

  // Right-click on a light: only this row plays; again: every row plays (FL's solo).
  const soloRow = (pattern, track, row) => {
    const entries = visibleTracks(pattern).map((t) => { const part = partOf(pattern, t.id); return { t, part, view: trackRows(t, part) }; });
    const isSelf = (t, r) => t.id === track.id && r.pitch === row.pitch;
    const soloed = entries.every(({ t, part, view }) => view.rows.every((r) => {
      const muted = rowMuted(part, view, r) || (view.drum && Boolean(part?.mute));
      return isSelf(t, r) ? !muted : muted;
    }));
    const parts = entries.map(({ t, view }) => {
      if (soloed) return view.drum ? { track: t.id, mute: false, muted: [] } : { track: t.id, mute: false };
      if (view.drum) return { track: t.id, mute: false, muted: view.rows.filter((r) => !isSelf(t, r)).map((r) => r.pitch) };
      return { track: t.id, mute: t.id !== track.id };
    });
    daw("setPartMutes", { pattern: pattern.id, parts });
  };

  // A small knob: drag up or down (Shift: fine), double-click for the default. One command on release.
  const knob = ({ value, min, max, def, title, format, commit }) => {
    const element = html("span", "pat-knob");
    let current = clamp(Number(value) || 0, min, max);
    const show = () => {
      element.style.setProperty("--a", `${-135 + ((current - min) / (max - min)) * 270}deg`);
      element.title = `${title}: ${format(current)}. Drag up or down, double-click to reset.`;
    };
    show();
    element.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      e.stopPropagation();
      const y0 = e.clientY, start = current;
      let moved = false;
      screen.dragging = true;
      element.setPointerCapture?.(e.pointerId);
      const move = (m) => {
        const dy = y0 - m.clientY;
        if (Math.abs(dy) > 1) moved = true;
        current = clamp(start + (dy / (m.shiftKey ? 600 : 120)) * (max - min), min, max);
        show();
      };
      const up = () => {
        element.removeEventListener("pointermove", move);
        element.removeEventListener("pointerup", up);
        element.removeEventListener("pointercancel", up);
        screen.dragging = false;
        if (moved && current !== start) commit(current);
        screen.flushSession?.();
      };
      element.addEventListener("pointermove", move);
      element.addEventListener("pointerup", up);
      element.addEventListener("pointercancel", up);
    });
    element.addEventListener("dblclick", (e) => { e.stopPropagation(); current = def; show(); commit(def); });
    return element;
  };

  const panText = (v) => (Math.abs(v) < 0.01 ? "centre" : `${Math.round(Math.abs(v) * 100)}% ${v < 0 ? "left" : "right"}`);
  const dbText = (v) => (v <= -59.5 ? "off" : `${v > 0 ? "+" : ""}${v.toFixed(1)} dB`);

  // Left of a row's name: the mute light, pan and level. A track row's knobs are the track's own
  // volume and pan (the mixer's); a drum row's level scales its velocities, and on a Kit track the
  // pan knob is the pad's pan.
  const rowStrip = (pattern, track, view, row) => {
    const part = partOf(pattern, track.id);
    const strip = html("span", "pat-strip");
    const muted = rowMuted(part, view, row);
    const light = html("button", `pat-light${muted ? "" : " is-on"}`);
    light.type = "button";
    light.tabIndex = -1;
    light.title = `${muted ? "Muted" : "Playing"}: click to ${muted ? "unmute" : "mute"} ${view.drum ? "this row" : "this track"} in the pattern, right-click to solo it`;
    light.addEventListener("click", () => toggleMute(pattern, track, view, row));
    light.addEventListener("contextmenu", (e) => { e.preventDefault(); e.stopPropagation(); soloRow(pattern, track, row); });
    strip.append(light);
    if (!view.drum)
      strip.append(knob({ value: track.pan ?? 0, min: -1, max: 1, def: 0, title: `${track.name} pan`, format: panText,
                          commit: (v) => daw("setTrack", { id: track.id, pan: v }) }),
                   knob({ value: Math.max(-60, track.volume ?? 0), min: -60, max: 6, def: 0, title: `${track.name} volume`, format: dbText,
                          commit: (v) => daw("setTrack", { id: track.id, volume: v <= -59.5 ? -100 : v }) }));
    else {
      const pan = track.kitPans?.[String(row.pitch)];
      strip.append(pan != null && row.pitch >= 36 && row.pitch <= 51
        ? knob({ value: pan, min: -1, max: 1, def: 0, title: `${row.name} pan`, format: panText,
                 commit: (v) => daw("kitPad", { track: track.id, note: row.pitch, pan: v }) })
        : html("span", "pat-knob is-blank"));
      strip.append(knob({ value: part?.levels?.[String(row.pitch)] ?? 1, min: 0, max: 2, def: 1, title: `${row.name} level`,
                          format: (v) => `${Math.round(v * 100)}%`,
                          commit: (v) => daw("setPart", { pattern: pattern.id, track: track.id, levels: { [row.pitch]: v } }) }));
    }
    return strip;
  };

  //---------------------------------------------------------------- selected row and step values
  const selectedRow = () => screen.patternRow ?? null; // { track, pitch }
  const selectRow = (track, pitch, redraw = true) => {
    screen.patternRow = { track: track.id, pitch };
    if (redraw) render(true);
  };

  // The rows in rack order, for the arrow keys: [{ track, view, row }].
  const rackRows = (pattern) => visibleTracks(pattern).flatMap((track) => {
    const part = partOf(pattern, track.id);
    const view = trackRows(track, part);
    if (!view.drum && isMelody(part, pattern)) return [];
    return view.rows.map((row) => ({ track, view, row }));
  });
  const chosenRow = (pattern) => {
    const chosen = selectedRow();
    return chosen ? rackRows(pattern).find((r) => r.track.id === chosen.track && r.row.pitch === chosen.pitch) ?? null : null;
  };

  // What the bars under the selected row edit (FL's graph editor): a step's velocity, how long it
  // holds, how late it plays, or (track rows) its note.
  const TARGETS = [["velocity", "Velocity"], ["length", "Length"], ["shift", "Shift"], ["pitch", "Pitch"]];
  const stepValues = (pattern, track, view, row) => {
    const part = partOf(pattern, track.id);
    const count = stepCount(pattern);
    const step = pattern.step;
    const byIndex = new Map();
    for (const n of rowNotes(part, view, row)) { const i = noteIndex(n, pattern); if (!byIndex.has(i)) byIndex.set(i, n); }
    let target = screen.patternTarget ?? "velocity";
    if (target === "pitch" && view.drum) target = "velocity";
    const base = row.pitch;

    const frac = (i, n) => {
      if (target === "velocity") return n.velocity / 127;
      if (target === "length") return Math.log(clamp(n.length / step, MIN_LEN, MAX_LEN) / MIN_LEN) / Math.log(MAX_LEN / MIN_LEN);
      if (target === "shift") return clamp(n.time / step - i, 0, MAX_SHIFT) / MAX_SHIFT;
      return clamp((n.pitch - (base - 12)) / 24, 0, 1);
    };
    // A height (0..1) into the change for the note and the text that tells it.
    const value = (i, f) => {
      if (target === "velocity") { const v = clamp(Math.round(f * 127), 1, 127); return [{ velocity: v }, `velocity ${v}`]; }
      if (target === "length") {
        let r = MIN_LEN * (MAX_LEN / MIN_LEN) ** f;
        if (Math.abs(r - 1) < 0.06) r = 1;
        r = Math.round(r * 100) / 100;
        return [{ length: r * step }, `length ${Math.round(r * 100)}%`];
      }
      if (target === "shift") { const s = Math.round(f * MAX_SHIFT * 100) / 100; return [{ time: (i + s) * step }, `shift ${Math.round(s * 100)}%`]; }
      const p = clamp(Math.round(base - 12 + f * 24), 0, 127);
      return [{ pitch: p }, noteName(p)];
    };
    const describe = (i, n) => value(i, frac(i, n))[1];

    const line = html("div", "pat-row pat-vel-row");
    const head = html("span", "pat-vel-head");
    const pick = html("select", "dropdown pat-target");
    pick.title = "What the bars set for each step";
    for (const [key, text] of TARGETS) if (key !== "pitch" || !view.drum) pick.append(new Option(text, key));
    pick.value = target;
    pick.addEventListener("change", () => { screen.patternTarget = pick.value; render(true); });
    head.append(html("span", "pat-vel-name", row.name), pick);
    const strip = html("div", `pat-vel is-${target}`);
    strip.style.setProperty("--steps", count);
    strip.title = {
      velocity: "Velocity of each step: drag up or down to set it, drag across steps to draw a ramp",
      length: "How long each step holds (10% to 400% of a step): drag up or down",
      shift: "How late each step plays (up to 90% of a step): drag up to push it later",
      pitch: "The note of each step (an octave either side of the row note): drag up or down, you hear it as you go",
    }[target];
    const cells = [];
    for (let i = 0; i < count; ++i) {
      const note = byIndex.get(i);
      const cell = html("div", `pat-vel-cell${note ? " has-note" : ""}`);
      const bar = html("i");
      const label = html("b");
      if (note) {
        bar.style.height = `${frac(i, note) * 100}%`;
        cell.title = `Step ${i + 1}: ${describe(i, note)}`;
        if (target === "pitch") label.textContent = noteName(note.pitch);
      }
      cell.append(bar, label);
      strip.append(cell);
      cells.push({ cell, bar, label });
    }
    strip.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      const changes = new Map();
      let heard = -1;
      const set = (ev) => {
        const box = strip.getBoundingClientRect();
        const i = clamp(Math.floor((ev.clientX - box.left) / (STEP_W + STEP_GAP)), 0, count - 1);
        const note = byIndex.get(i);
        if (!note) return;
        const f = clamp(1 - (ev.clientY - box.top) / box.height, 0, 1);
        const [change, text] = value(i, f);
        changes.set(note.id, change);
        cells[i].bar.style.height = `${f * 100}%`;
        cells[i].cell.title = `Step ${i + 1}: ${text}`;
        if (target === "pitch") {
          cells[i].label.textContent = text;
          if (change.pitch !== heard) { heard = change.pitch; audition(track, change.pitch); }
        }
      };
      set(e);
      screen.dragging = true;
      strip.setPointerCapture?.(e.pointerId);
      const up = () => {
        strip.removeEventListener("pointermove", set);
        strip.removeEventListener("pointerup", up);
        strip.removeEventListener("pointercancel", up);
        screen.dragging = false;
        if (changes.size) daw("editPattern", { pattern: pattern.id, track: track.id, change: [...changes].map(([id, change]) => ({ id, ...change })) });
        screen.flushSession?.();
      };
      strip.addEventListener("pointermove", set);
      strip.addEventListener("pointerup", up);
      strip.addEventListener("pointercancel", up);
    });
    line.append(head, strip);
    return line;
  };

  //---------------------------------------------------------------- piano roll on a part
  // The piano roll (pianoroll.js) edits a MIDI clip with notes in seconds. A part is notes in
  // beats from the pattern's start, so an adapter shows the part as a clip at the tempo of the
  // song's start and turns every edit back into beats for editPattern.
  const secondsPerBeat = () => 60 / (screen.session?.tempo || 120);

  const partClip = (pattern, track) => {
    const spb = secondsPerBeat();
    const part = partOf(pattern, track.id);
    return {
      id: pattern.id, name: `${pattern.name} · ${track.name}`, midi: true, start: 0, offset: 0, length: pattern.length * spb,
      notes: (part?.notes ?? []).map((n) => ({ id: n.id, pitch: n.pitch, time: n.time * spb, length: n.length * spb, velocity: n.velocity })),
    };
  };

  const openRoll = (track) => {
    rollTrack = track.id;
    render(true);
  };
  const closeRoll = () => {
    rollTrack = null;
    render(true);
  };

  const buildRoll = (pattern, track) => {
    const spb = secondsPerBeat();
    const toBeats = (edits) => {
      const out = {};
      const fix = (n) => {
        const r = { ...n };
        if (r.time != null) r.time = r.time / spb;
        if (r.length != null) r.length = r.length / spb;
        return r;
      };
      if (edits.add) out.add = edits.add.map(fix);
      if (edits.change) out.change = edits.change.map(fix);
      if (edits.remove) out.remove = edits.remove;
      return out;
    };
    const rollDaw = (command, args) => {
      if (command === "editNotes") return daw("editPattern", { pattern: pattern.id, track: track.id, ...toBeats(args) });
      if (command === "liveNote") return daw(command, { ...args, track: track.id });
      return daw(command, args);
    };
    const instance = new PianoRoll({ clip: partClip(pattern, track), track, session: screen.session, daw: rollDaw,
                                     toast, colour: track.colour || COLOURS[0] });
    return { roll: instance, element: instance.element, track: track.id, pattern: pattern.id };
  };

  const disposeRoll = () => {
    roll?.roll.dispose?.();
    roll = null;
  };

  const rollView = (pattern, track) => {
    const box = html("div", "pat-roll");
    const bar = html("div", "pat-roll-bar");
    const back = html("button", "tool", "← Steps");
    back.type = "button";
    back.title = "Back to the step rack";
    back.addEventListener("click", closeRoll);
    bar.append(back, html("span", "pat-roll-title", `${pattern.name} · ${track.name}`),
      html("span", "pat-hint", "Notes are placed in beats from the pattern's start. Double-click to add, drag to move."));
    if (!roll || roll.track !== track.id || roll.pattern !== pattern.id) {
      disposeRoll();
      roll = buildRoll(pattern, track);
    } else roll.roll.update(partClip(pattern, track), track, screen.session);
    box.append(bar, roll.element);
    return box;
  };

  //---------------------------------------------------------------- drawing
  const audition = (track, pitch) => {
    daw("liveNote", { note: pitch, velocity: 100, track: track.id });
    setTimeout(() => daw("liveNote", { note: pitch, velocity: 0, track: track.id }), 220);
  };

  const stepRow = (pattern, track, view, row) => {
    const part = partOf(pattern, track.id);
    const count = stepCount(pattern);
    const step = pattern.step;
    const on = new Map();
    for (const n of rowNotes(part, view, row)) { const i = noteIndex(n, pattern); if (!on.has(i)) on.set(i, n); }
    const strip = html("div", "pat-steps");
    strip.style.setProperty("--steps", count);
    const buttons = [];
    for (let i = 0; i < count; ++i) {
      const note = on.get(i);
      const b = html("button", `pat-step${(i >> 2) % 2 ? " is-alt" : ""}${note ? " is-on" : ""}`);
      b.type = "button";
      b.tabIndex = -1;
      b.setAttribute("aria-label", `${row.name} step ${i + 1}`);
      if (note) {
        b.style.setProperty("--vel", String(clamp(note.velocity / 127, 0.25, 1)));
        // A track row's step on another note shows it; a longer, shorter or later step shows a bar.
        if (!view.drum && note.pitch !== row.pitch) b.textContent = noteName(note.pitch).replace(/-?\d+$/, "");
        const len = note.length / step, shift = clamp(note.time / step - i, 0, MAX_SHIFT);
        if (Math.abs(len - 1) > 0.02 || shift > 0.005) {
          b.classList.add("has-shape");
          b.style.setProperty("--len", String(clamp(len, 0.08, 1 - shift)));
          b.style.setProperty("--shift", String(shift));
        }
        b.title = `Step ${i + 1}: ${noteName(note.pitch)}, velocity ${note.velocity}${Math.abs(len - 1) > 0.02 ? `, length ${Math.round(len * 100)}%` : ""}${shift > 0.005 ? `, shift ${Math.round(shift * 100)}%` : ""}`;
      }
      strip.append(b);
      buttons.push(b);
      (columns[i] ??= []).push(b);
    }

    const indexAt = (e) => {
      const box = strip.getBoundingClientRect();
      return clamp(Math.floor((e.clientX - box.left) / (STEP_W + STEP_GAP)), 0, count - 1);
    };
    strip.addEventListener("contextmenu", (e) => e.preventDefault());
    strip.addEventListener("pointerdown", (e) => {
      if (e.button !== 0 && e.button !== 2) return;
      e.preventDefault();
      const first = indexAt(e);
      const target = e.button === 2 ? false : !on.has(first); // paint on, or clear
      const changes = new Map();
      const apply = (i) => {
        if (changes.has(i) || on.has(i) === target) return;
        changes.set(i, target ? STEP_VELOCITY : 0);
        buttons[i].classList.toggle("is-on", target);
        buttons[i].style.setProperty("--vel", "0.78");
        if (!target) buttons[i].textContent = "";
      };
      apply(first);
      if (target && changes.size) audition(track, row.pitch);
      screen.dragging = true;
      strip.setPointerCapture?.(e.pointerId);
      const move = (m) => {
        const to = indexAt(m);
        for (let i = Math.min(first, to); i <= Math.max(first, to); ++i) apply(i);
      };
      const up = () => {
        strip.removeEventListener("pointermove", move);
        strip.removeEventListener("pointerup", up);
        strip.removeEventListener("pointercancel", up);
        screen.dragging = false;
        if (changes.size) setSteps(pattern, track, view, row, [...changes]);
        screen.flushSession?.();
      };
      strip.addEventListener("pointermove", move);
      strip.addEventListener("pointerup", up);
      strip.addEventListener("pointercancel", up);
    });
    return strip;
  };

  // A part with notes that the steps can't show (chords, or two notes on a step): a small preview
  // of them (edit them in the piano roll).
  const preview = (pattern, part) => {
    const box = html("div", "pat-melody");
    const notes = part.notes;
    const lo = Math.min(...notes.map((n) => n.pitch)), hi = Math.max(...notes.map((n) => n.pitch));
    box.style.width = `${stepCount(pattern) * (STEP_W + STEP_GAP) - STEP_GAP}px`;
    for (const n of notes) {
      const bar = html("i");
      bar.style.left = `${(n.time / pattern.length) * 100}%`;
      bar.style.width = `${Math.max(0.6, (n.length / pattern.length) * 100)}%`;
      bar.style.top = `${hi === lo ? 40 : 8 + (1 - (n.pitch - lo) / (hi - lo)) * 70}%`;
      box.append(bar);
    }
    box.title = `${notes.length} notes, some of them together on a step: edit them with Notes, or clear the track in the pattern to use steps.`;
    return box;
  };

  // The beat numbers over the steps, and the grip at their end that adds or removes steps
  // (whole beats; Alt: single steps), like FL's "drag here for more steps".
  const ruler = (pattern) => {
    const count = stepCount(pattern);
    const step = pattern.step;
    const line = html("div", "pat-row pat-ruler");
    line.append(html("span", "pat-strip is-blank"), html("span", "pat-label is-blank"));
    const beats = html("div", "pat-beats");
    beats.style.setProperty("--steps", count);
    const perBar = screen.beatsPerBar || 4;
    for (let i = 0; i < count; ++i) {
      const at = i * step;
      if (Math.abs(at - Math.round(at)) > 1e-6) continue;
      const beat = Math.round(at);
      const mark = html("span", `pat-beat${beat % perBar === 0 ? " is-bar" : ""}`, beat % perBar === 0 ? `${beat / perBar + 1}` : `${(beat % perBar) + 1}`);
      mark.style.gridColumn = `${i + 1} / span ${Math.max(1, Math.round(1 / step))}`;
      beats.append(mark);
    }
    const grip = html("span", "pat-grip", "⟷");
    grip.title = "Drag to add or remove steps (whole beats; hold Alt for single steps)";
    grip.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      const x0 = e.clientX;
      let length = pattern.length;
      screen.dragging = true;
      grip.setPointerCapture?.(e.pointerId);
      const move = (m) => {
        const steps = Math.max(1, count + Math.round((m.clientX - x0) / (STEP_W + STEP_GAP)));
        let beatsLong = steps * step;
        if (!m.altKey) beatsLong = Math.max(1, Math.round(beatsLong));
        length = clamp(beatsLong, 0.25, 256);
        grip.style.transform = `translateX(${(length / step - count) * (STEP_W + STEP_GAP)}px)`;
        grip.dataset.length = length % perBar === 0 ? `${length / perBar} bar${length / perBar === 1 ? "" : "s"}` : `${+length.toFixed(2)} beats`;
      };
      const up = () => {
        grip.removeEventListener("pointermove", move);
        grip.removeEventListener("pointerup", up);
        grip.removeEventListener("pointercancel", up);
        screen.dragging = false;
        grip.style.transform = "";
        delete grip.dataset.length;
        if (Math.abs(length - pattern.length) > 1e-6) daw("setPattern", { id: pattern.id, length });
        screen.flushSession?.();
      };
      grip.addEventListener("pointermove", move);
      grip.addEventListener("pointerup", up);
      grip.addEventListener("pointercancel", up);
    });
    line.append(beats, grip);
    return line;
  };

  const trackBlock = (pattern, track, index) => {
    const part = partOf(pattern, track.id);
    const view = trackRows(track, part);
    const block = html("div", `pat-track${part?.mute && view.drum ? " is-muted" : ""}`);
    block.style.setProperty("--track", track.colour || COLOURS[(index + 3) % COLOURS.length]);
    block.dataset.track = track.id;

    const head = html("div", "pat-head-row");
    const name = html("button", "pat-track-name", track.name);
    name.type = "button";
    name.title = `${track.name}${track.instrument?.name ? ` · ${track.instrument.name}` : ""}. Click to select the track, right-click for more.`;
    name.addEventListener("click", () => screen.select?.(track.id, null));
    name.addEventListener("contextmenu", (e) => trackMenu(e, pattern, track, view));
    head.append(html("span", "pat-dot"), name);
    if (view.drum) {
      const mute = html("button", `pat-mute${part?.mute ? " is-on" : ""}`, "M");
      mute.type = "button";
      mute.title = "Mute the whole track in the pattern";
      mute.addEventListener("click", () => daw("setPart", { pattern: pattern.id, track: track.id, mute: !part?.mute }));
      const add = html("button", "pat-add-sound", "+ sound");
      add.type = "button";
      add.title = "Add a row for another sound";
      add.addEventListener("click", (e) => addSoundMenu(e, pattern, track, view));
      head.append(mute, add);
    }
    const edit = html("button", "pat-edit", "Notes");
    edit.type = "button";
    edit.title = "Open the piano roll on this track's part of the pattern";
    edit.addEventListener("click", () => openRoll(track));
    head.append(edit);
    const more = html("button", "pat-more", "⋯");
    more.type = "button";
    more.title = "Track options";
    more.addEventListener("click", (e) => trackMenu(e, pattern, track, view));
    head.append(more);
    block.append(head);

    if (!view.drum && isMelody(part, pattern)) {
      const row = html("div", "pat-row");
      row.append(html("span", "pat-strip is-blank"), html("span", "pat-label is-fixed", "Notes"), preview(pattern, part));
      block.append(row);
      return block;
    }
    for (const row of view.rows) {
      const line = html("div", `pat-row${rowMuted(part, view, row) ? " is-row-muted" : ""}`);
      const label = html("button", "pat-label", view.drum ? row.name : noteName(row.pitch));
      label.type = "button";
      label.dataset.track = String(track.id);
      label.dataset.pitch = String(row.pitch);
      label.title = `${row.name}: click to hear it and show its step values, right-click for fill, randomize, copy and more`;
      label.classList.toggle("is-selected", selectedRow()?.track === track.id && selectedRow()?.pitch === row.pitch);
      label.addEventListener("click", () => { audition(track, row.pitch); selectRow(track, row.pitch); });
      label.addEventListener("contextmenu", (e) => rowMenu(e, pattern, track, view, row));
      line.append(rowStrip(pattern, track, view, row), label, stepRow(pattern, track, view, row));
      line.addEventListener("contextmenu", (e) => { if (!e.target.closest(".pat-steps, .pat-strip")) rowMenu(e, pattern, track, view, row); });
      block.append(line);
    }
    const chosen = selectedRow();
    const shown = chosen?.track === track.id ? view.rows.find((r) => r.pitch === chosen.pitch) : null;
    if (shown && rowNotes(part, view, shown).length) block.append(stepValues(pattern, track, view, shown));
    return block;
  };

  // Previous or next pattern (the arrows and the - and = keys).
  const stepPattern = (by) => {
    const list = screen.session?.patterns ?? [];
    if (list.length < 2) return;
    const at = list.findIndex((p) => p.id === selected()?.id);
    choose(list[(at + by + list.length) % list.length].id);
  };

  const splitByTrack = async (pattern) => {
    const result = await daw("splitPattern", { id: pattern.id });
    const made = result?.patterns ?? [];
    if (!made.length) return;
    screen.selectedPattern = made[0];
    toast(`Split into ${made.length} pattern${made.length === 1 ? "" : "s"}, one per track`);
  };

  const patternMenu = (event, pattern) => {
    const box = event.currentTarget.getBoundingClientRect();
    const withNotes = pattern.parts.filter((p) => p.notes?.length).length;
    popMenu(box.left, box.bottom + 2, ({ item, rule, heading }) => {
      heading(pattern.name);
      item("Split by track", () => splitByTrack(pattern), {
        disabled: withNotes < 2, hint: "A pattern for each track that has notes; the blocks on the song are replaced by blocks of the new patterns",
      });
      item("Copy pattern", () => newPattern(pattern.id));
      rule();
      item("Only tracks in this pattern", () => { screen.patternOnlyUsed = !screen.patternOnlyUsed; screen.rememberView?.(); render(true); },
        { checked: Boolean(screen.patternOnlyUsed), hint: "Hide the MIDI tracks that have no notes in this pattern" });
      rule();
      item("Delete pattern", () => daw("removePattern", { id: pattern.id }), { danger: true });
    }, event.currentTarget);
  };

  const controls = (pattern, list) => {
    const bar = html("div", "pat-bar");
    const picker = html("select", "dropdown pat-picker");
    picker.title = "The pattern you edit (- and = for the previous and next)";
    list.forEach((p) => picker.append(new Option(p.name, p.id)));
    picker.value = String(pattern.id);
    picker.addEventListener("change", () => choose(Number(picker.value)));

    const dot = html("button", "pat-colour");
    dot.type = "button";
    dot.title = "Pattern colour (click to change)";
    dot.style.background = patternColour(pattern);
    dot.addEventListener("click", () => {
      const at = COLOURS.indexOf(patternColour(pattern));
      daw("setPattern", { id: pattern.id, colour: COLOURS[(at + 1) % COLOURS.length] });
    });

    const name = html("input", "pat-name");
    Object.assign(name, { type: "text", value: pattern.name, maxLength: 40, title: "Pattern name" });
    name.addEventListener("change", () => { if (name.value.trim()) daw("setPattern", { id: pattern.id, name: name.value.trim() }); });
    name.addEventListener("keydown", (e) => { e.stopPropagation(); if (e.key === "Enter") name.blur(); });

    const small = (text, title, onClick, className = "tool") => {
      const b = html("button", `${className} pat-tool`, text);
      b.type = "button";
      b.title = title;
      b.addEventListener("click", onClick);
      return b;
    };

    const perBar = screen.beatsPerBar || 4;
    const lengths = [1, 2, 3, 4, 8].map((bars) => bars * perBar);
    const length = html("select", "dropdown pat-length");
    length.title = "How long the pattern is (or drag the grip at the end of the steps)";
    if (!lengths.some((l) => Math.abs(l - pattern.length) < 1e-6)) lengths.push(pattern.length);
    lengths.sort((a, b) => a - b).forEach((l) => length.append(new Option(l % perBar === 0 ? `${l / perBar} bar${l / perBar > 1 ? "s" : ""}` : `${+l.toFixed(2)} beats`, l)));
    length.value = String(lengths.find((l) => Math.abs(l - pattern.length) < 1e-6));
    length.addEventListener("change", () => daw("setPattern", { id: pattern.id, length: Number(length.value) }));

    const step = html("select", "dropdown pat-stepsize");
    step.title = "How long one step is";
    STEP_SIZES.forEach(([beats, text]) => step.append(new Option(text, beats)));
    step.value = String(STEP_SIZES.reduce((best, s) => (Math.abs(s[0] - pattern.step) < Math.abs(best[0] - pattern.step) ? s : best))[0]);
    step.addEventListener("change", () => daw("setPattern", { id: pattern.id, step: Number(step.value) }));

    const swing = html("label", "pat-swing");
    swing.title = "Swing: delays every second step (100% is a triplet feel)";
    const swingInput = html("input");
    Object.assign(swingInput, { type: "range", min: 0, max: 100, step: 1, value: Math.round(pattern.swing * 100) });
    const swingText = html("span", "pat-swing-value", `${Math.round(pattern.swing * 100)}%`);
    swingInput.addEventListener("input", () => { swingText.textContent = `${swingInput.value}%`; });
    swingInput.addEventListener("change", () => daw("setPattern", { id: pattern.id, swing: Number(swingInput.value) / 100 }));
    swing.append(html("span", "", "Swing"), swingInput, swingText);

    const recording = Boolean(screen.session?.patternRecording);
    const record = small(recording ? "● Stop" : "● Record", recording ? "Stop recording (the pattern keeps looping)"
      : "Record what you play into this pattern while it loops, pass after pass. Plays on the selected row's track, or on the armed MIDI tracks.",
      () => toggleRecord(pattern), `tool pat-record${recording ? " is-on" : ""}`);
    const snapOn = screen.session?.patternSnap !== false;
    const snap = html("label", "pat-snap");
    snap.title = "Snap recorded notes to the step grid";
    const snapBox = html("input");
    Object.assign(snapBox, { type: "checkbox", checked: snapOn });
    snapBox.addEventListener("change", () => daw("patternRecord", { on: recording, snap: snapBox.checked }));
    snap.append(snapBox, html("span", "", "Snap"));

    const place = small("Place at playhead", "Put this pattern on the song at the playhead (the Patterns lane under the ruler)", () => placeAtPlayhead(pattern), "tool is-accent");

    const prev = small("‹", "Previous pattern (-)", () => stepPattern(-1), "tool pat-arrow");
    const next = small("›", "Next pattern (=)", () => stepPattern(1), "tool pat-arrow");
    prev.disabled = next.disabled = list.length < 2;
    const more = small("⋯", "Pattern options: split by track, show only the tracks in this pattern", (e) => patternMenu(e, pattern));

    bar.append(dot, prev, picker, next, name,
      small("+", "New pattern", () => newPattern()),
      small("Copy", "Duplicate this pattern", () => newPattern(pattern.id)),
      small("Delete", "Delete this pattern and its blocks", () => daw("removePattern", { id: pattern.id }), "tool is-danger"),
      more, html("span", "tb-sep"), length, step, swing, html("span", "pat-spacer"), record, snap, place);
    return bar;
  };

  // Record into the pattern: Pattern mode first (the loop is the pattern), then the looper.
  const toggleRecord = async (pattern) => {
    if (screen.session?.patternRecording) return void daw("patternRecord", { on: false });
    if ((screen.session?.patternMode ?? -1) < 0) await daw("patternMode", { pattern: pattern.id });
    const chosen = selectedRow();
    const track = chosen?.track ?? midiTracks()[0]?.id;
    await daw("patternRecord", { on: true, snap: screen.session?.patternSnap !== false, ...(track != null ? { track } : {}) });
  };

  const placeAtPlayhead = async (pattern) => {
    const beats = screen.tempoMap.toBeats(screen.status?.position ?? 0);
    const perBar = screen.beatsPerBar || 4;
    const start = Math.max(0, Math.floor(beats / perBar + 1e-6) * perBar);
    const id = await daw("addPatternClip", { pattern: pattern.id, start, length: pattern.length });
    if (typeof id === "number") screen.selectedPatternClip = id;
  };

  const empty = () => {
    const box = html("div", "pat-empty");
    box.append(html("h3", "", "Patterns"),
      html("p", "", "Build a loop with step buttons, like a drum machine. Each row is a sound, each button a step. Place the pattern on the song, or switch to Pattern mode (next to the loop button) to loop it while you work."));
    const kit = html("button", "tool is-accent", "Start with a drum kit");
    kit.type = "button";
    kit.addEventListener("click", startWithKit);
    const blank = html("button", "tool", "New empty pattern");
    blank.type = "button";
    blank.addEventListener("click", () => newPattern());
    const actions = html("div", "pat-empty-actions");
    actions.append(kit, blank);
    box.append(actions);
    return box;
  };

  const render = (force = false) => {
    const session = screen.session;
    if (!session) return;
    const pattern = selected();
    if (pattern) screen.selectedPattern = pattern.id;
    const tracks = midiTracks();
    const key = JSON.stringify([pattern, tracks.map((t) => [t.id, t.name, t.colour, t.keyNames, t.kitPans, t.volume, t.pan, t.instrument?.name]),
                                (session.patterns ?? []).map((p) => [p.id, p.name]), screen.beatsPerBar,
                                session.patternRecording, session.patternSnap, screen.patternRow, rollTrack, session.tempo,
                                screen.patternTarget, screen.patternOnlyUsed]);
    if (!force && key === signature) return;
    signature = key;
    columns = [];
    lit = -1;
    const rolled = rollTrack != null && pattern ? tracks.find((t) => t.id === rollTrack) : null;
    if (rolled) {
      element.replaceChildren(rollView(pattern, rolled));
      return;
    }
    if (rollTrack != null) rollTrack = null;
    disposeRoll();
    const scroll = element.querySelector(".pat-rack")?.scrollTop ?? 0;
    element.replaceChildren();
    if (!pattern) {
      element.append(empty());
      return;
    }
    const rack = html("div", "pat-rack is-drop-rack");
    if (tracks.length) rack.append(ruler(pattern));
    visibleTracks(pattern).forEach((track) => rack.append(trackBlock(pattern, track, tracks.indexOf(track))));
    const add = html("div", "pat-foot");
    const addTrack = html("button", "tool", "+ instrument");
    addTrack.type = "button";
    addTrack.title = "Add a MIDI track with a drum kit, synth or plug-in, and a row for it";
    addTrack.addEventListener("click", () => instrumentMenu(addTrack));
    add.append(addTrack);
    if (!tracks.length) add.append(html("span", "pat-hint", "Add an instrument to get rows. Drum instruments get a row per sound."));
    rack.append(add);
    element.append(controls(pattern, session.patterns), rack);
    rack.scrollTop = scroll;
  };

  // A sample dragged from the library onto the rack (browser.js). On a row of a Kit track it
  // goes on that pad; anywhere else in the rack it becomes a new row: the next free pad of a Kit
  // track (the selected one if it is a Kit, else the first, else a new Kit track).
  const isKit = (track) => /kit/i.test(track.instrument?.id ?? "") || /^kit$/i.test(track.instrument?.name ?? "");
  const dropSample = async (file, hit) => {
    const patternId = selected()?.id ?? (await newPattern());
    if (typeof patternId !== "number") return;
    let track = null, pitch = null;
    if (hit?.track != null) {
      track = midiTracks().find((t) => t.id === hit.track) ?? null;
      pitch = hit.pitch ?? null;
      if (track && !isKit(track)) track = null;
    }
    if (!track) {
      track = midiTracks().find((t) => t.id === selectedRow()?.track && isKit(t)) ?? midiTracks().find(isKit) ?? null;
      if (!track) {
        const found = await daw("instruments");
        const kit = (found?.instruments ?? []).find((i) => /kit/i.test(i.id) || i.name === "Kit");
        if (!kit) return void toast("The Kit instrument isn't available.", "is-error");
        const id = await addInstrumentTrack(kit);
        if (typeof id !== "number") return;
        await screen.waitForTrack?.(id);
        track = (screen.session?.tracks ?? []).find((t) => t.id === id) ?? { id, keyNames: {} };
      }
    }
    const target = screen.session?.patterns?.find((p) => p.id === patternId) ?? { id: patternId, parts: [] };
    const part = partOf(target, track.id);
    const shown = trackRows(track, part).rows.map((r) => r.pitch);
    if (pitch == null) {
      // The next pad (notes 36-51) with no sample whose row isn't busy.
      const named = new Set(Object.keys(track.keyNames ?? {}).filter((k) => track.keyNames[k]).map(Number));
      pitch = Array.from({ length: 16 }, (_, i) => 36 + i).find((p) => !named.has(p) && !(part?.notes ?? []).some((n) => n.pitch === p)) ?? 36;
    }
    const result = await daw("kitSample", { track: track.id, note: pitch, file });
    if (result?.ok === false) return;
    if (!shown.includes(pitch))
      await daw("setPart", { pattern: target.id, track: track.id, drum: true, rows: [...shown, pitch] });
    screen.selectedPattern = target.id;
    screen.patternRow = { track: track.id, pitch };
    render(true);
  };

  // The rack's keys, while the pointer last went down in it (DawScreen.onKey asks first):
  // - and = previous / next pattern, Up / Down the selected row, Ctrl+Shift+Left / Right rotate
  // it, Ctrl+C / Ctrl+V copy and paste its steps.
  let active = false;
  document.addEventListener("pointerdown", (e) => { active = element.contains(e.target); }, true);
  const onKey = (event) => {
    if (!active || !element.isConnected || rollTrack != null) return false;
    const pattern = selected();
    if (!pattern) return false;
    const mod = event.ctrlKey || event.metaKey;
    const go = (action) => { event.preventDefault(); action(); return true; };
    if (!mod && !event.altKey && (event.key === "-" || event.key === "_" || event.code === "NumpadSubtract")) return go(() => stepPattern(-1));
    if (!mod && !event.altKey && (event.key === "=" || event.key === "+" || event.code === "NumpadAdd")) return go(() => stepPattern(1));
    const rows = rackRows(pattern);
    const chosen = chosenRow(pattern);
    if (!mod && !event.altKey && !event.shiftKey && (event.key === "ArrowUp" || event.key === "ArrowDown") && rows.length) {
      return go(() => {
        const at = chosen ? rows.indexOf(chosen) : -1;
        const to = rows[clamp(at + (event.key === "ArrowUp" ? -1 : 1), 0, rows.length - 1)];
        selectRow(to.track, to.row.pitch);
        element.querySelector(".pat-label.is-selected")?.scrollIntoView({ block: "nearest" });
      });
    }
    if (!chosen) return false;
    const act = rowActions(pattern, chosen.track, chosen.view, chosen.row);
    if (mod && event.shiftKey && !event.altKey && (event.key === "ArrowLeft" || event.key === "ArrowRight"))
      return go(() => act.rotate(event.key === "ArrowLeft" ? -1 : 1));
    if (mod && !event.shiftKey && !event.altKey && event.key.toLowerCase() === "c") return go(() => act.copy());
    if (mod && !event.shiftKey && !event.altKey && event.key.toLowerCase() === "v" && stepClipboard) return go(() => act.paste());
    return false;
  };

  return {
    element,
    update() { render(); },
    ensurePattern,
    dropSample,
    onKey,
    // The light that runs along the steps while the pattern loops.
    setStatus(status) {
      const pattern = selected();
      if (roll) {
        const beats = pattern ? screen.tempoMap.toBeats(status.position) : 0;
        const inside = pattern ? ((beats % pattern.length) + pattern.length) % pattern.length : 0;
        const live = pattern && status.playing && status.patternMode === pattern.id;
        roll.roll.setStatus({ ...status, position: live ? inside * secondsPerBeat() : -1 });
      }
      let at = -1;
      if (pattern && status.playing && status.patternMode === pattern.id) {
        const beats = screen.tempoMap.toBeats(status.position);
        at = Math.floor(((beats % pattern.length) + pattern.length) % pattern.length / pattern.step + 1e-6);
      }
      if (at === lit) return;
      for (const b of columns[lit] ?? []) b.classList.remove("is-now");
      for (const b of columns[at] ?? []) b.classList.add("is-now");
      lit = at;
    },
  };
}
