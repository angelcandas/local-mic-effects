// The chord track in the DAW screen: a lane under the ruler with the song's chords
// (PROJECT `chords`, see src/daw/Edit.h). Each chord lasts until the next one.
//
//   click an empty spot     type a chord there (on the beat under the pointer)
//   double-click a chord    type over it
//   drag a chord            move it to another beat
//   right-click             edit, delete, detect, clear
//   Delete / Backspace      delete the selected chord
//
// The chords are kept in beats (the tempo map counts them), so they stay on the music when
// the tempo changes; the session sends each one's time in seconds too. Harmonizer inserts
// in Chords mode follow the track, and the piano roll tints the chord's notes.
import { html } from "./widgets.js";

export const CHORD_ROW = 26; // px

// Pitch classes (bit 0 = C) of the chord sounding at a time (seconds), 0 when none.
export function chordMaskAt(chords, time) {
  let mask = 0;
  for (const chord of chords ?? []) {
    if (chord.time > time + 1e-6) break;
    mask = chord.chord ? chord.mask : 0;
  }
  return mask;
}

// options: { daw (command, args) -> Promise, popMenu (x, y, fill), toast (text, className) }
export function createChordLane(screen, { daw, popMenu, toast }) {
  const element = html("div", "chord-lane");
  element.title = "Chord track: click to add a chord · double-click to change it · drag to move · right-click for more";
  element.tabIndex = 0;
  let chords = [], width = 0, selected = null, editing = null;

  const beatAt = (x, event) => {
    const seconds = screen.snapTime(Math.max(0, x) / screen.zoom, event);
    // Chords sit on beats (half beats when the grid is finer and snapping is on).
    const step = screen.snap && !event?.altKey && screen.gridBeats < 1 ? 0.5 : 1;
    return Math.max(0, Math.round(screen.tempoMap.toBeats(seconds) / step) * step);
  };
  const xOfBeat = (beat) => screen.tempoMap.toSeconds(beat) * screen.zoom;
  const local = (event) => event.clientX - element.getBoundingClientRect().left;
  const same = (a, b) => Math.abs(a - b) < 1e-3;

  const put = (beat, name, from = null) => {
    const args = { action: "put", beat, name };
    if (from != null && !same(from, beat)) args.from = from;
    else if (from != null) delete args.from;
    return daw("chords", args);
  };
  const remove = (beat) => daw("chords", { action: "remove", beat });

  const draw = () => {
    element.replaceChildren();
    element.style.width = `${width}px`;
    chords.forEach((chord, i) => {
      const x0 = xOfBeat(chord.beat);
      const x1 = i + 1 < chords.length ? xOfBeat(chords[i + 1].beat) : Math.max(x0 + 60, width);
      const block = html("div", `chord-block${chord.chord ? "" : " is-none"}${selected != null && same(selected, chord.beat) ? " is-selected" : ""}`);
      block.style.left = `${x0}px`;
      block.style.width = `${Math.max(4, x1 - x0)}px`;
      block.dataset.beat = chord.beat;
      block.append(html("span", "chord-name", chord.name));
      block.title = `${chord.name} · bar ${screen.barsBeats(chord.time)}`;
      element.append(block);
    });
    if (editing) element.append(editing.input);
    if (!chords.length && !editing)
      element.append(html("span", "chord-empty", "Click to type a chord, or Detect to hear them from the song"));
  };

  // An inline field at a beat; Enter or leaving it writes the chord, Escape forgets it.
  const edit = (beat, current = "", from = null) => {
    if (editing) editing.input.blur();
    const input = html("input", "chord-input");
    Object.assign(input, { value: current, spellcheck: false, placeholder: "Am7", title: "C, Am, G7, Fmaj7, Dm7, Bdim, Dsus4, D/F#, N.C." });
    input.style.left = `${xOfBeat(beat)}px`;
    let done = false;
    const finish = async (write) => {
      if (done) return;
      done = true;
      editing = null;
      const name = input.value.trim();
      if (write && name && name !== current) {
        const result = await put(beat, name, from);
        if (result?.ok !== false) selected = beat;
      } else if (write && !name && from != null) await remove(from);
      draw();
    };
    input.addEventListener("keydown", (event) => {
      event.stopPropagation(); // not the screen's key commands
      if (event.key === "Enter") { event.preventDefault(); finish(true); element.focus(); }
      else if (event.key === "Escape") { event.preventDefault(); finish(false); element.focus(); }
      else if (event.key === "Tab") {
        // Tab: write it and type the next chord a bar later.
        event.preventDefault();
        finish(true).then(() => edit(beat + screen.beatsPerBar));
      }
    });
    input.addEventListener("blur", () => finish(true));
    editing = { input, beat };
    draw();
    input.focus();
    input.select();
  };

  const chordOf = (event) => {
    const block = event.target.closest?.(".chord-block");
    return block ? chords.find((c) => same(c.beat, Number(block.dataset.beat))) : null;
  };

  // The lane's clicks are its own (not the arrange's: a double-click there opens a clip).
  element.addEventListener("dblclick", (event) => event.stopPropagation());
  element.addEventListener("pointerdown", (event) => {
    if (event.target.closest(".chord-input")) return event.stopPropagation();
    if (event.button !== 0) return;
    event.stopPropagation();
    event.preventDefault(); // the field opened below keeps the focus
    const chord = chordOf(event);
    const startX = local(event);
    // The field opens once the click is over, so the click doesn't take its focus.
    const later = (...args) => setTimeout(() => edit(...args), 0);
    if (!chord) {
      if (event.detail === 1) later(beatAt(startX, event));
      return;
    }
    if (event.detail >= 2) return later(chord.beat, chord.name, chord.beat);

    // A chord: select it, and drag it along the beats.
    selected = chord.beat;
    element.focus({ preventScroll: true });
    const block = event.target.closest(".chord-block");
    const startLeft = xOfBeat(chord.beat);
    let target = chord.beat, moved = false;
    screen.dragging = true;
    element.setPointerCapture?.(event.pointerId);
    const move = (e) => {
      const dx = local(e) - startX;
      if (Math.abs(dx) > 3) moved = true;
      if (!moved) return;
      target = beatAt(startLeft + dx, e);
      block.style.left = `${xOfBeat(target)}px`;
      block.classList.add("is-dragging");
    };
    const up = () => {
      element.removeEventListener("pointermove", move);
      element.removeEventListener("pointerup", up);
      element.removeEventListener("pointercancel", up);
      screen.dragging = false;
      if (moved && !same(target, chord.beat)) {
        selected = target;
        put(target, chord.name, chord.beat);
      } else draw();
    };
    element.addEventListener("pointermove", move);
    element.addEventListener("pointerup", up);
    element.addEventListener("pointercancel", up);
    draw();
  });

  element.addEventListener("contextmenu", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const chord = chordOf(event);
    const beat = chord ? chord.beat : beatAt(local(event), event);
    popMenu(event.clientX, event.clientY, ({ item, rule }) => {
      if (chord) {
        item(`Change ${chord.name}…`, () => edit(chord.beat, chord.name, chord.beat));
        item("Delete chord", () => remove(chord.beat));
        rule();
      } else item("Add a chord here…", () => edit(beat));
      item("Detect chords from the song", () => screen.detectChords());
      item("Clear the chord track", () => daw("chords", { action: "set", chords: [] }), { disabled: !chords.length });
    });
  });

  element.addEventListener("keydown", (event) => {
    if (selected == null || editing) return;
    if (event.key === "Delete" || event.key === "Backspace") {
      event.preventDefault();
      event.stopPropagation();
      const beat = selected;
      selected = null;
      remove(beat);
    } else if (event.key === "Enter") {
      event.preventDefault();
      event.stopPropagation();
      const chord = chords.find((c) => same(c.beat, selected));
      if (chord) edit(chord.beat, chord.name, chord.beat);
    }
  });
  element.addEventListener("blur", () => {
    if (selected != null && !editing) { selected = null; draw(); }
  });

  return {
    element,
    update(session, laneWidth) {
      chords = session.chords ?? [];
      width = laneWidth;
      if (selected != null && !chords.some((c) => same(c.beat, selected))) selected = null;
      if (!editing) draw();
      else element.style.width = `${width}px`;
    },
    // The chord under the playhead, for the corner.
    nameAt(time) {
      let name = "";
      for (const chord of chords) {
        if (chord.time > time + 1e-6) break;
        name = chord.name;
      }
      return name;
    },
    toast,
  };
}
