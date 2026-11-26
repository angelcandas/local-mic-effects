// The practice guide in the DAW screen: a lane under the ruler with the notes of a song's melody
// (PROJECT `guide`, see src/daw/Edit.h), so you can record a voice with a visual reference instead
// of only the sound. It's sent from Practice with the song's tracks ("Open in DAW"); it makes no
// sound. The lane scrolls with the arrangement, so the notes pass the playhead as it plays.
//
//   right-click    move the whole guide a little earlier or later, or remove it
import { html } from "./widgets.js";

export const GUIDE_ROW = 46; // px

const NAMES = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"];
export const noteName = (midi) => `${NAMES[((Math.round(midi) % 12) + 12) % 12]}${Math.floor(Math.round(midi) / 12) - 1}`;

// options: { daw (command, args) -> Promise, popMenu (x, y, fill) }
export function createGuideLane(screen, { daw, popMenu }) {
  const element = html("div", "guide-lane");
  element.title = "Practice guide: the notes to sing, from the song opened in Practice · right-click to move or remove it";
  let notes = [], width = 0, key = "";

  const draw = () => {
    element.replaceChildren();
    element.style.width = `${width}px`;
    if (!notes.length) return;
    let low = Infinity, high = -Infinity;
    for (const n of notes) { low = Math.min(low, n.pitch); high = Math.max(high, n.pitch); }
    const span = Math.max(8, high - low + 2);
    const bottom = (low + high) / 2 - span / 2;
    const inner = GUIDE_ROW - 8;
    for (const n of notes) {
      const bar = html("div", "guide-note");
      bar.style.left = `${n.start * screen.zoom}px`;
      bar.style.width = `${Math.max(3, (n.end - n.start) * screen.zoom - 1)}px`;
      bar.style.top = `${4 + inner - ((n.pitch - bottom) / span) * inner - 3}px`;
      bar.title = `${noteName(n.pitch)} · bar ${screen.barsBeats(n.start)}`;
      element.append(bar);
    }
  };

  element.addEventListener("pointerdown", (event) => event.stopPropagation());
  element.addEventListener("dblclick", (event) => event.stopPropagation());
  element.addEventListener("contextmenu", (event) => {
    event.preventDefault();
    event.stopPropagation();
    popMenu(event.clientX, event.clientY, ({ item, rule }) => {
      item("Move 50 ms earlier", () => daw("guide", { action: "shift", by: -0.05 }));
      item("Move 50 ms later", () => daw("guide", { action: "shift", by: 0.05 }));
      rule();
      item("Remove the guide", () => daw("guide", { action: "clear" }));
    });
  });

  return {
    element,
    // `session.guide`: { name, notes: [start, end, pitch, ...] } or null.
    update(session, laneWidth) {
      const guide = session.guide;
      notes = [];
      for (let i = 0; guide && i + 2 < guide.notes.length; i += 3) notes.push({ start: guide.notes[i], end: guide.notes[i + 1], pitch: guide.notes[i + 2] });
      const next = `${laneWidth}:${screen.zoom}:${guide?.notes?.length ?? 0}:${guide?.notes?.[0] ?? ""}:${guide?.notes?.[guide?.notes?.length - 1] ?? ""}`;
      width = laneWidth;
      if (next !== key) { key = next; draw(); }
      else element.style.width = `${width}px`;
    },
    // The note being sung at `time` (seconds), for the corner: "A4", or "" between notes.
    nameAt(time) {
      const n = notes.find((x) => time >= x.start && time < x.end);
      return n ? noteName(n.pitch) : "";
    },
  };
}
