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

// Does the part hold notes that the one row of steps can't show (other pitches or off the grid)?
function isMelody(part, pattern, rows) {
  if (!part?.notes?.length) return false;
  const step = pattern.step;
  return part.notes.some((n) => n.pitch !== rows[0].pitch || Math.abs(n.time / step - Math.round(n.time / step)) > 0.1);
}

const stepCount = (pattern) => Math.max(1, Math.round(pattern.length / pattern.step - 1e-6));
const noteIndex = (note, pattern) => Math.round(note.time / pattern.step);

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
    const id = await daw("addPattern", from != null ? { from } : { name: `Pattern ${list.length + 1}` });
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

  const setSteps = (pattern, track, pitch, steps) => daw("setSteps", { pattern: pattern.id, track: track.id, pitch, steps });

  const rowMenu = (event, pattern, track, view, row) => {
    event.preventDefault();
    const part = partOf(pattern, track.id);
    const count = stepCount(pattern);
    const step = pattern.step;
    const edit = (change) => daw("editPattern", { pattern: pattern.id, track: track.id, change });
    const fill = (every) => setSteps(pattern, track, row.pitch, Array.from({ length: count }, (_, i) => [i, i % every === 0 ? STEP_VELOCITY : 0]));
    const shift = (by) => edit(notesAt(part, row.pitch).map((n) => ({
      id: n.id, time: ((((n.time + by * step) % pattern.length) + pattern.length) % pattern.length),
    })));
    popMenu(event.clientX, event.clientY, ({ item, rule, heading }) => {
      heading(row.name);
      item("Fill every 2 steps", () => fill(2));
      item("Fill every 4 steps", () => fill(4));
      item("Fill every 8 steps", () => fill(8));
      item("Shift left", () => shift(-1));
      item("Shift right", () => shift(1));
      item("Clear", () => daw("editPattern", { pattern: pattern.id, track: track.id, remove: notesAt(part, row.pitch).map((n) => n.id) }));
      rule();
      if (!view.drum || !view.named) {
        // The row's note: "rows per note" for a plug-in, or the one note of a track row.
        const move = async (delta) => {
          const to = clamp(row.pitch + delta, 0, 127);
          if (view.drum) {
            const rows = view.rows.map((r) => (r.pitch === row.pitch ? to : r.pitch));
            await daw("setPart", { pattern: pattern.id, track: track.id, drum: true, rows });
          } else await daw("setPart", { pattern: pattern.id, track: track.id, stepNote: to });
          await edit(notesAt(part, row.pitch).map((n) => ({ id: n.id, pitch: to })));
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
    popMenu(event.clientX, event.clientY, ({ item, rule, heading }) => {
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

  //---------------------------------------------------------------- selected row and velocity bars
  const selectedRow = () => screen.patternRow ?? null; // { track, pitch }
  const selectRow = (track, pitch) => {
    screen.patternRow = { track: track.id, pitch };
    render(true);
  };

  // The velocity bars of one row, under the track's steps: one bar per step that has a note.
  // Drag on a bar to set it; drag across several to paint a ramp.
  const velocityBars = (pattern, track, row) => {
    const part = partOf(pattern, track.id);
    const count = stepCount(pattern);
    const byIndex = new Map(notesAt(part, row.pitch).map((n) => [noteIndex(n, pattern), n]));
    const line = html("div", "pat-row pat-vel-row");
    line.append(html("span", "pat-label is-fixed", `${row.name} velocity`));
    const strip = html("div", "pat-vel");
    strip.style.setProperty("--steps", count);
    strip.title = "Velocity of each step: drag up or down to set it, drag across steps to draw a ramp";
    const bars = [];
    for (let i = 0; i < count; ++i) {
      const cell = html("div", `pat-vel-cell${byIndex.has(i) ? " has-note" : ""}`);
      const bar = html("i");
      const v = byIndex.get(i)?.velocity ?? 0;
      bar.style.height = `${(v / 127) * 100}%`;
      if (byIndex.has(i)) cell.title = `Step ${i + 1}: velocity ${v}`;
      cell.append(bar);
      strip.append(cell);
      bars.push({ cell, bar });
    }
    strip.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      const changes = new Map();
      const set = (ev) => {
        const box = strip.getBoundingClientRect();
        const i = clamp(Math.floor((ev.clientX - box.left) / (STEP_W + STEP_GAP)), 0, count - 1);
        const note = byIndex.get(i);
        if (!note) return;
        const v = clamp(Math.round((1 - (ev.clientY - box.top) / box.height) * 127), 1, 127);
        changes.set(note.id, v);
        bars[i].bar.style.height = `${(v / 127) * 100}%`;
        bars[i].cell.title = `Step ${i + 1}: velocity ${v}`;
      };
      set(e);
      screen.dragging = true;
      strip.setPointerCapture?.(e.pointerId);
      const up = () => {
        strip.removeEventListener("pointermove", set);
        strip.removeEventListener("pointerup", up);
        strip.removeEventListener("pointercancel", up);
        screen.dragging = false;
        if (changes.size) daw("editPattern", { pattern: pattern.id, track: track.id, change: [...changes].map(([id, velocity]) => ({ id, velocity })) });
        screen.flushSession?.();
      };
      strip.addEventListener("pointermove", set);
      strip.addEventListener("pointerup", up);
      strip.addEventListener("pointercancel", up);
    });
    line.append(strip);
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
    const on = new Map();
    for (const n of notesAt(part, row.pitch)) on.set(noteIndex(n, pattern), n.velocity);
    const strip = html("div", "pat-steps");
    strip.style.setProperty("--steps", count);
    const buttons = [];
    for (let i = 0; i < count; ++i) {
      const b = html("button", `pat-step${(i >> 2) % 2 ? " is-alt" : ""}${on.has(i) ? " is-on" : ""}`);
      b.type = "button";
      b.tabIndex = -1;
      b.setAttribute("aria-label", `${row.name} step ${i + 1}`);
      if (on.has(i)) b.style.setProperty("--vel", String(clamp(on.get(i) / 127, 0.25, 1)));
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
        if (changes.size) setSteps(pattern, track, row.pitch, [...changes]);
        screen.flushSession?.();
      };
      strip.addEventListener("pointermove", move);
      strip.addEventListener("pointerup", up);
      strip.addEventListener("pointercancel", up);
    });
    return strip;
  };

  // A part with notes that the steps can't show: a small preview of them (edit them in the piano roll).
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
    box.title = `${notes.length} notes: they don't fit the step grid. Clear the track in the pattern to use steps.`;
    return box;
  };

  const trackBlock = (pattern, track, index) => {
    const part = partOf(pattern, track.id);
    const view = trackRows(track, part);
    const block = html("div", `pat-track${part?.mute ? " is-muted" : ""}`);
    block.style.setProperty("--track", track.colour || COLOURS[(index + 3) % COLOURS.length]);
    block.dataset.track = track.id;

    const head = html("div", "pat-head-row");
    const mute = html("button", `pat-mute${part?.mute ? " is-on" : ""}`, "M");
    mute.type = "button";
    mute.title = "Mute this track in the pattern";
    mute.addEventListener("click", () => daw("setPart", { pattern: pattern.id, track: track.id, mute: !part?.mute }));
    const name = html("button", "pat-track-name", track.name);
    name.type = "button";
    name.title = `${track.name}${track.instrument?.name ? ` · ${track.instrument.name}` : ""}. Click to select the track, right-click for more.`;
    name.addEventListener("click", () => screen.select?.(track.id, null));
    name.addEventListener("contextmenu", (e) => trackMenu(e, pattern, track, view));
    head.append(html("span", "pat-dot"), name, mute);
    if (view.drum) {
      const add = html("button", "pat-add-sound", "+ sound");
      add.type = "button";
      add.title = "Add a row for another sound";
      add.addEventListener("click", (e) => addSoundMenu(e, pattern, track, view));
      head.append(add);
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

    if (!view.drum && isMelody(part, pattern, view.rows)) {
      const row = html("div", "pat-row");
      row.append(html("span", "pat-label is-fixed", "Notes"), preview(pattern, part));
      block.append(row);
      return block;
    }
    for (const row of view.rows) {
      const line = html("div", "pat-row");
      const label = html("button", "pat-label", view.drum ? row.name : noteName(row.pitch));
      label.type = "button";
      label.title = `${row.name}: click to hear it and show its velocity bars, right-click for fill, shift and more`;
      label.classList.toggle("is-selected", selectedRow()?.track === track.id && selectedRow()?.pitch === row.pitch);
      label.addEventListener("click", () => { audition(track, row.pitch); selectRow(track, row.pitch); });
      label.addEventListener("contextmenu", (e) => rowMenu(e, pattern, track, view, row));
      line.append(label, stepRow(pattern, track, view, row));
      line.addEventListener("contextmenu", (e) => { if (!e.target.closest(".pat-steps")) rowMenu(e, pattern, track, view, row); });
      block.append(line);
    }
    const chosen = selectedRow();
    const shown = chosen?.track === track.id ? view.rows.find((r) => r.pitch === chosen.pitch) : null;
    if (shown && notesAt(part, shown.pitch).length) block.append(velocityBars(pattern, track, shown));
    return block;
  };

  const controls = (pattern, list) => {
    const bar = html("div", "pat-bar");
    const picker = html("select", "dropdown pat-picker");
    picker.title = "The pattern you edit";
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
    length.title = "How long the pattern is";
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

    bar.append(dot, picker, name,
      small("+", "New pattern", () => newPattern()),
      small("Copy", "Duplicate this pattern", () => newPattern(pattern.id)),
      small("Delete", "Delete this pattern and its blocks", () => daw("removePattern", { id: pattern.id }), "tool is-danger"),
      html("span", "tb-sep"), length, step, swing, html("span", "pat-spacer"), record, snap, place);
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
    const key = JSON.stringify([pattern, tracks.map((t) => [t.id, t.name, t.colour, t.keyNames, t.instrument?.name]),
                                (session.patterns ?? []).map((p) => [p.id, p.name]), screen.beatsPerBar,
                                session.patternRecording, session.patternSnap, screen.patternRow, rollTrack, session.tempo]);
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
    const rack = html("div", "pat-rack");
    tracks.forEach((track, i) => rack.append(trackBlock(pattern, track, i)));
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

  return {
    element,
    update() { render(); },
    ensurePattern,
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

//==================================================================================== the lane
const ROW_H = 30;

// The Patterns lane under the ruler. screen: zoom, snap, snapTime, tempoMap, beatsPerBar, session,
// dragging, selectedPattern.
export function createPatternLane(screen, daw, { popMenu, openPatterns }) {
  const element = html("div", "pattern-lane");
  element.title = "Patterns: click an empty spot to place the selected pattern · drag to move · drag the right edge to repeat";
  element.tabIndex = -1;
  let width = 0, clips = [], patterns = [];
  let selected = null;

  const patternOf = (id) => patterns.find((p) => p.id === id);
  const barsOf = (beats) => Math.floor(beats / (screen.beatsPerBar || 4) + 1e-6) * (screen.beatsPerBar || 4);
  const rowCount = () => Math.max(1, ...clips.map((c) => c.row + 1));

  // The first row where [start, end) is free (so overlapping blocks stack).
  const freeRow = (start, length, skip) => {
    for (let row = 0; row < 4; ++row)
      if (!clips.some((c) => c.id !== skip && c.row === row && c.start < start + length - 1e-6 && c.start + c.length > start + 1e-6)) return row;
    return 3;
  };

  const preview = (clip, pattern, box) => {
    const canvas = html("canvas", "pl-preview");
    const w = Math.max(1, Math.round(box.width)), h = ROW_H - 6;
    canvas.width = w;
    canvas.height = h;
    const g = canvas.getContext("2d");
    const notes = pattern.parts.flatMap((p) => (p.mute ? [] : p.notes));
    if (!notes.length) return canvas;
    const lo = Math.min(...notes.map((n) => n.pitch)), hi = Math.max(...notes.map((n) => n.pitch));
    const map = screen.tempoMap, origin = map.toSeconds(clip.start);
    g.fillStyle = "rgba(255,255,255,0.7)";
    for (let rep = 0; rep * pattern.length < clip.length - 1e-6; ++rep)
      for (const n of notes) {
        const at = rep * pattern.length + n.time;
        if (at >= clip.length) continue;
        const x = (map.toSeconds(clip.start + at) - origin) * screen.zoom;
        const y = 4 + (hi === lo ? 0.5 : 1 - (n.pitch - lo) / (hi - lo)) * (h - 10);
        g.fillRect(Math.round(x), Math.round(y), Math.max(1, Math.round(2)), 2);
      }
    return canvas;
  };

  const draw = () => {
    const map = screen.tempoMap, zoom = screen.zoom;
    element.replaceChildren();
    element.style.width = `${width}px`;
    element.style.height = `${rowCount() * ROW_H + 4}px`;
    for (const clip of clips) {
      const pattern = patternOf(clip.pattern);
      if (!pattern) continue;
      const left = clip.startSeconds * zoom, size = Math.max(6, (clip.endSeconds - clip.startSeconds) * zoom);
      const block = html("div", `pl-block${clip.mute ? " is-muted" : ""}${selected === clip.id ? " is-selected" : ""}`);
      block.dataset.id = clip.id;
      block.tabIndex = 0;
      block.style.cssText = `left:${left}px;width:${size}px;top:${clip.row * ROW_H + 2}px;height:${ROW_H - 2}px;--pat:${patternColour(pattern)}`;
      block.append(preview(clip, pattern, { width: size }), html("span", "pl-name", pattern.name), html("span", "pl-edge"));
      // The seams between repeats of the pattern.
      for (let rep = 1; rep * pattern.length < clip.length - 1e-6; ++rep) {
        const seam = html("i", "pl-seam");
        seam.style.left = `${(map.toSeconds(clip.start + rep * pattern.length) - map.toSeconds(clip.start)) * zoom}px`;
        block.append(seam);
      }
      element.append(block);
    }
  };

  const select = (id) => {
    selected = id;
    screen.selectedPatternClip = id;
    element.querySelectorAll(".pl-block").forEach((b) => b.classList.toggle("is-selected", Number(b.dataset.id) === id));
  };

  const clipOf = (id) => clips.find((c) => c.id === id);

  const duplicate = async (clip) => {
    const row = freeRow(clip.start + clip.length, clip.length, -1);
    const id = await daw("addPatternClip", { pattern: clip.pattern, start: clip.start + clip.length, length: clip.length, row });
    if (typeof id === "number") select(id);
  };

  const menu = (event, clip) => {
    event.preventDefault();
    select(clip.id);
    popMenu(event.clientX, event.clientY, ({ item, rule, heading }) => {
      heading(patternOf(clip.pattern)?.name ?? "Pattern");
      item("Edit pattern", () => { screen.selectedPattern = clip.pattern; openPatterns(); });
      item("Make unique", async () => { const id = await daw("makePatternUnique", { id: clip.id }); if (typeof id === "number") screen.selectedPattern = id; },
        { hint: "Copy the pattern for this block only, so editing it doesn't change the other blocks" });
      item("Turn into MIDI clips", () => daw("patternToClips", { id: clip.id }), { hint: "Write the block out as ordinary MIDI clips on the tracks" });
      item(clip.mute ? "Unmute" : "Mute", () => daw("setPatternClip", { id: clip.id, mute: !clip.mute }));
      item("Duplicate", () => duplicate(clip), { shortcut: "Ctrl+D" });
      rule();
      item("Delete", () => daw("removePatternClip", { id: clip.id }), { danger: true, shortcut: "Delete" });
    });
  };

  const local = (event) => event.clientX - element.getBoundingClientRect().left;

  element.addEventListener("contextmenu", (event) => {
    const block = event.target.closest(".pl-block");
    if (block) menu(event, clipOf(Number(block.dataset.id)));
    else event.preventDefault();
  });

  element.addEventListener("dblclick", (event) => {
    const block = event.target.closest(".pl-block");
    const clip = block && clipOf(Number(block.dataset.id));
    if (!clip) return;
    screen.selectedPattern = clip.pattern;
    openPatterns();
  });

  element.addEventListener("keydown", (event) => {
    const clip = clipOf(selected);
    if (!clip) return;
    const mod = event.ctrlKey || event.metaKey;
    if (event.key === "Delete" || event.key === "Backspace") {
      event.preventDefault();
      event.stopPropagation();
      select(null);
      daw("removePatternClip", { id: clip.id });
    } else if (mod && event.key.toLowerCase() === "d") {
      event.preventDefault();
      event.stopPropagation();
      duplicate(clip);
    }
  });

  element.addEventListener("pointerdown", async (event) => {
    if (event.button !== 0) return;
    const blockElement = event.target.closest(".pl-block");
    const map = screen.tempoMap;

    if (!blockElement) {
      // An empty spot: place the selected pattern at the bar.
      const pattern = patterns.find((p) => p.id === screen.selectedPattern) ?? patterns[0];
      if (!pattern) return;
      const start = barsOf(map.toBeats(local(event) / screen.zoom));
      const id = await daw("addPatternClip", { pattern: pattern.id, start, length: pattern.length, row: freeRow(start, pattern.length, -1) });
      if (typeof id === "number") select(id);
      return;
    }

    const clip = clipOf(Number(blockElement.dataset.id));
    if (!clip) return;
    select(clip.id);
    if (event.detail >= 2) return;
    const pattern = patternOf(clip.pattern);
    const resize = event.target.closest(".pl-edge") !== null;
    const startX = local(event);
    const startSeconds = clip.startSeconds, endSeconds = clip.endSeconds;
    let start = clip.start, length = clip.length, moved = false;
    screen.dragging = true;
    blockElement.setPointerCapture?.(event.pointerId);

    const move = (e) => {
      const dx = local(e) - startX;
      if (Math.abs(dx) > 2) moved = true;
      if (!moved) return;
      if (resize) {
        // Whole repeats of the pattern (Alt: free).
        const seconds = endSeconds + dx / screen.zoom;
        const beats = map.toBeats(Math.max(startSeconds, seconds)) - clip.start;
        length = e.altKey ? Math.max(1 / 16, beats) : Math.max(pattern.length, Math.round(beats / pattern.length) * pattern.length);
        blockElement.style.width = `${Math.max(6, (map.toSeconds(clip.start + length) - startSeconds) * screen.zoom)}px`;
      } else {
        start = Math.max(0, map.toBeats(screen.snapTime(startSeconds + dx / screen.zoom, e)));
        blockElement.style.left = `${map.toSeconds(start) * screen.zoom}px`;
      }
    };
    const up = () => {
      blockElement.removeEventListener("pointermove", move);
      blockElement.removeEventListener("pointerup", up);
      blockElement.removeEventListener("pointercancel", up);
      screen.dragging = false;
      if (moved) daw("setPatternClip", { id: clip.id, start, length, row: freeRow(start, length, clip.id) });
      screen.flushSession?.();
    };
    blockElement.addEventListener("pointermove", move);
    blockElement.addEventListener("pointerup", up);
    blockElement.addEventListener("pointercancel", up);
  });

  return {
    element,
    update(session, laneWidth) {
      patterns = session.patterns ?? [];
      clips = session.patternClips ?? [];
      width = laneWidth;
      if (selected != null && !clips.some((c) => c.id === selected)) selected = null;
      if (screen.selectedPatternClip != null && clips.some((c) => c.id === screen.selectedPatternClip)) selected = screen.selectedPatternClip;
      draw();
    },
    rows: () => rowCount(),
  };
}
