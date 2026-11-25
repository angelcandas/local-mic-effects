// MIDI effects on a MIDI track (market item 13): an arpeggiator, a chord maker and a scale
// lock, played in order before the instrument. They sit at the left of the track's effects
// panel, before the instrument and its audio effects. The engine side is src/daw/MidiEffects.*;
// the bridge commands are addMidiFx {track, type, index?}, setMidiFx {id, ...settings},
// removeMidiFx {id} and moveMidiFx {id, index}; the session lists them as track.midiFx.

import { html } from "./widgets.js";

export const MIDI_FX_TYPES = [
  { type: "arp", name: "Arpeggiator", hint: "Plays the held notes one after another on the beat: up, down, as played..." },
  { type: "chord", name: "Chord", hint: "Turns each note into a chord: the song key's own chord on it, or a fixed shape" },
  { type: "scale", name: "Scale lock", hint: "Keeps every note in the song key (or a scale of its own): wrong notes move to the nearest right one" },
];

const RATES = [
  [1, "1/4"], [0.5, "1/8"], [1 / 3, "1/8 triplet"], [0.25, "1/16"], [1 / 6, "1/16 triplet"], [0.125, "1/32"], [2, "1/2"],
];
const PATTERNS = [
  ["up", "Up"], ["down", "Down"], ["upDown", "Up and down"], ["played", "As played"], ["random", "Random"], ["together", "All together"],
];
// Keys as MidiEffects.cpp getShapes / getScales.
const SHAPES = [
  ["key", "Triad in key"], ["key7", "Seventh in key"], ["major", "Major"], ["minor", "Minor"], ["power", "Power (5th + octave)"],
  ["sus2", "Sus2"], ["sus4", "Sus4"], ["seventh", "7th"], ["major7", "Major 7th"], ["minor7", "Minor 7th"], ["add9", "Add 9"],
  ["octave", "Octave"], ["octaves", "Two octaves"], ["fifth", "Fifth"],
];
const SCALES = [
  ["song", "Song key"], ["major", "Major"], ["minor", "Minor"], ["harmonicMinor", "Harmonic minor"], ["dorian", "Dorian"],
  ["mixolydian", "Mixolydian"], ["pentatonic", "Pentatonic major"], ["pentatonicMinor", "Pentatonic minor"], ["blues", "Blues"],
];
const ROOTS = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"];

const typeName = (type) => MIDI_FX_TYPES.find((t) => t.type === type)?.name ?? type;

// The column of MIDI effect cards. `tools`: { daw, button, popMenu, toast }.
export function createMidiFxPanel(track, tools) {
  const { daw, button, popMenu } = tools;
  const element = html("div", "midifx-panel");
  const head = html("div", "midifx-head");
  const addButton = button("tool midifx-add", "+ Add", "Add a MIDI effect (arpeggiator, chord, scale lock)", (event) => {
    const box = event.currentTarget.getBoundingClientRect();
    popMenu(box.left, box.bottom + 4, ({ item }) => {
      for (const fx of MIDI_FX_TYPES)
        item(fx.name, () => daw("addMidiFx", { track: current.id, type: fx.type }), { hint: fx.hint, disabled: (current.midiFx ?? []).length >= 8 });
    });
  });
  const label = html("span", "chain-gain-title", "MIDI FX");
  label.title = "MIDI effects: the notes go through these, left to right, before the instrument plays them";
  head.append(label, addButton);
  const list = html("div", "midifx-list");
  element.append(head, list);

  let current = track;
  let layout = null;
  let cards = [];

  const select = (options, value, title, onChange) => {
    const box = html("select", "dropdown midifx-select");
    box.title = title;
    for (const [v, label] of options) box.append(new Option(label, String(v)));
    box.value = String(value);
    box.addEventListener("change", () => onChange(box.value));
    return box;
  };
  const row = (label, control) => {
    const line = html("label", "midifx-row");
    line.append(html("span", "midifx-label", label), control);
    return line;
  };
  // A 0..100 % slider; sends while dragging as one undo step.
  const slider = (value, title, onChange) => {
    const input = html("input", "midifx-slider");
    Object.assign(input, { type: "range", min: 5, max: 100, step: 1, value: Math.round(value * 100), title });
    let first = true;
    input.addEventListener("input", () => { onChange(Number(input.value) / 100, first); first = false; });
    input.addEventListener("change", () => { first = true; });
    return input;
  };

  const set = (fx, values, first = true) => daw("setMidiFx", { id: fx.id, ...values, continue: !first });

  const makeCard = (fx, index, count) => {
    const card = html("div", `midifx-card is-${fx.type}`);
    const top = html("div", "midifx-card-head");
    const power = button("midifx-power", "", "On or off (off: the notes pass as they are)", () => set(fx, { on: !fx.on }));
    const title = html("strong", "midifx-title", typeName(fx.type));
    const more = button("midifx-more", "⋯", "Move or remove", (event) => {
      const box = event.currentTarget.getBoundingClientRect();
      popMenu(box.left, box.bottom + 4, ({ item }) => {
        item("Move left", () => daw("moveMidiFx", { id: fx.id, index: index - 1 }), { disabled: index === 0, hint: "Earlier in the chain: it gets the notes first" });
        item("Move right", () => daw("moveMidiFx", { id: fx.id, index: index + 1 }), { disabled: index >= count - 1 });
        item("Remove", () => daw("removeMidiFx", { id: fx.id }));
      });
    });
    top.append(power, title, more);
    card.append(top);

    const controls = {};
    if (fx.type === "arp") {
      controls.rate = select(RATES.map(([v, l]) => [v, l]), nearestRate(fx.rate), "How fast: the length of each step", (v) => set(fx, { rate: Number(v) }));
      controls.pattern = select(PATTERNS, fx.pattern, "The order the held notes play in", (v) => set(fx, { pattern: v }));
      controls.octaves = select([[1, "1 octave"], [2, "2 octaves"], [3, "3 octaves"], [4, "4 octaves"]], fx.octaves, "How many octaves up the pattern climbs", (v) => set(fx, { octaves: Number(v) }));
      controls.gate = slider(fx.gate, "Gate: how long each note lasts, as a part of its step (100% = legato)", (v, first) => set(fx, { gate: v }, first));
      controls.latch = html("input", "midifx-check");
      Object.assign(controls.latch, { type: "checkbox", checked: fx.latch, title: "Latch: keep playing after you let go, until you play new notes or stop" });
      controls.latch.addEventListener("change", () => set(fx, { latch: controls.latch.checked }));
      card.append(row("Rate", controls.rate), row("Order", controls.pattern), row("Range", controls.octaves), row("Gate", controls.gate), row("Latch", controls.latch));
    } else if (fx.type === "chord") {
      controls.shape = select(SHAPES, fx.shape, "The chord each note makes (in key: the song key's chord on that note)", (v) => set(fx, { shape: v }));
      controls.level = slider(fx.level, "The added notes' loudness, as a part of the played note's", (v, first) => set(fx, { level: v }, first));
      card.append(row("Chord", controls.shape), row("Level", controls.level));
    } else if (fx.type === "scale") {
      controls.scale = select(SCALES, fx.scale, "The scale the notes are kept in (Song key: the key set for the song)", (v) => set(fx, { scale: v }));
      controls.root = select(ROOTS.map((n, i) => [i, n]), fx.root, "The scale's root note", (v) => set(fx, { root: Number(v) }));
      controls.snap = select([["nearest", "Move to nearest"], ["drop", "Leave out"]], fx.snap, "What happens to a note outside the scale", (v) => set(fx, { snap: v }));
      card.append(row("Scale", controls.scale), row("Root", controls.root), row("Others", controls.snap));
    }

    const update = (next) => {
      fx = next;
      card.classList.toggle("is-off", !fx.on);
      power.setAttribute("aria-pressed", String(Boolean(fx.on)));
      const idle = (control) => control && document.activeElement !== control;
      if (idle(controls.rate)) controls.rate.value = String(nearestRate(fx.rate));
      if (idle(controls.pattern)) controls.pattern.value = fx.pattern;
      if (idle(controls.octaves)) controls.octaves.value = String(fx.octaves);
      if (idle(controls.gate)) controls.gate.value = String(Math.round(fx.gate * 100));
      if (controls.latch) controls.latch.checked = Boolean(fx.latch);
      if (idle(controls.shape)) controls.shape.value = fx.shape;
      if (idle(controls.level)) controls.level.value = String(Math.round(fx.level * 100));
      if (idle(controls.scale)) controls.scale.value = fx.scale;
      if (controls.root) {
        controls.root.disabled = fx.scale === "song";
        if (idle(controls.root)) controls.root.value = String(fx.root);
      }
      if (idle(controls.snap)) controls.snap.value = fx.snap;
    };
    update(fx);
    return { element: card, update };
  };

  const update = (next) => {
    current = next;
    const effects = next.midiFx ?? [];
    const key = effects.map((fx) => `${fx.id}:${fx.type}`).join(",");
    if (key !== layout) {
      layout = key;
      cards = effects.map((fx, i) => makeCard(fx, i, effects.length));
      list.replaceChildren(...cards.map((c) => c.element));
      if (!effects.length)
        list.append(html("p", "midifx-empty", "None. Add an arpeggiator, chords or a scale lock."));
    }
    effects.forEach((fx, i) => cards[i]?.update(fx));
  };
  update(track);
  return { id: track.id, element, update };
}

function nearestRate(rate) {
  let best = RATES[0][0];
  for (const [v] of RATES) if (Math.abs(v - rate) < Math.abs(best - rate)) best = v;
  return best;
}
