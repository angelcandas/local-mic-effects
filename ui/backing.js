// "Build backing vocals" (the clip menu): harmony and double voices from a sung lead, as
// real clips on new tracks in a folded group (see src/analysis/Harmony.h and the bridge's
// "backingVocals" command). Each voice is a copy of the lead clip with note edits, so the
// note pitch editor can fix any of its notes afterwards.

import { html } from "./widgets.js";

// [key, label], as analysis::harmony::getPartKeys() / getPartName().
export const BACKING_PARTS = [
  ["thirdAbove", "3rd above"], ["thirdBelow", "3rd below"], ["fifthAbove", "5th above"], ["fifthBelow", "5th below"],
  ["sixthBelow", "6th below"], ["octaveAbove", "Octave above"], ["octaveBelow", "Octave below"],
  ["chordAbove", "Chord notes above"], ["chordBelow", "Chord notes below"], ["doubleTrack", "Double (same notes)"],
];
// Voices 1 and 2 pan as a pair (close), 3 and 4 as a wider pair.
const DEFAULT_VOICES = ["thirdAbove", "fifthBelow", "doubleTrack", ""];
const NOTE_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"];

const PREF = "micfx.backing";
const loadPrefs = () => {
  try { return JSON.parse(localStorage.getItem(PREF) || "null") || {}; } catch { return {}; }
};
const savePrefs = (prefs) => {
  try { localStorage.setItem(PREF, JSON.stringify(prefs)); } catch { /* private mode */ }
};

export class BackingVocals {
  // screen: the DAW screen (session, setJob/endJob, other panels); after: the element the panel goes after.
  constructor(screen, after, { daw, toast }) {
    this.screen = screen;
    this.daw = daw;
    this.toast = toast;
    this.clip = null;
    this.running = false;

    const panel = (this.panel = html("div", "backing-panel"));
    panel.hidden = true;
    const prefs = loadPrefs();
    const voices = Array.isArray(prefs.voices) && prefs.voices.length === 4 ? prefs.voices : DEFAULT_VOICES;
    const option = (value, label, selected) => {
      const o = html("option", "", label);
      o.value = value;
      o.selected = selected;
      return o;
    };
    const select = (name, options, value) => {
      const s = html("select", "dropdown");
      s.name = name;
      for (const [key, label] of options) s.append(option(key, label, key === value));
      return s;
    };
    const row = (label, title, control) => {
      const l = html("label", "", label);
      l.title = title;
      l.append(control);
      return l;
    };

    const head = html("div", "export-head");
    this.title = html("strong", "backing-title", "Build backing vocals");
    const close = html("button", "mini backing-close", "×");
    close.title = "Close";
    close.setAttribute("aria-label", "Close");
    close.addEventListener("click", () => this.toggle(false));
    head.append(this.title, close);

    const options = html("div", "export-options");
    const choices = [["", "Off"], ...BACKING_PARTS];
    this.voiceSelects = voices.map((value, i) =>
      select(`voice${i + 1}`, choices, choices.some(([key]) => key === value) ? value : ""));
    this.voiceSelects.forEach((s, i) => {
      options.append(row(`Voice ${i + 1}`, i < 2 ? "Voices 1 and 2 are panned as a pair, a little left and right"
                                             : "Voices 3 and 4 are panned as a wider pair", s));
      s.addEventListener("change", () => this.update());
    });
    this.feel = select("feel", [["tight", "Tight (a careful studio take)"], ["loose", "Loose (a live group)"]], prefs.feel || "tight");
    this.follow = select("follow", [["chords", "The chords of the other tracks"], ["key", "The song key only"]], prefs.follow || "chords");
    options.append(row("Feel", "How far each voice wanders from the lead in time, tuning and vibrato", this.feel),
                   row("Harmony", "What the harmony notes are chosen from", this.follow));
    this.follow.addEventListener("change", () => this.update());

    this.note = html("p", "ai-note backing-note");
    this.bar = html("div", "export-bar");
    this.bar.append(html("div"));
    const footer = html("div", "export-footer");
    this.status = html("span", "export-result backing-status");
    this.cancel = html("button", "tool backing-cancel", "Cancel");
    this.cancel.addEventListener("click", () => this.daw("backingVocals", { action: "cancel" }));
    this.go = html("button", "tool is-accent backing-go", "Build");
    this.go.addEventListener("click", () => this.start());
    footer.append(this.status, this.cancel, this.go);

    panel.append(head, options, this.note, this.bar, footer);
    after.after(panel);
    window.__JUCE__.backend.addEventListener("dawBackingVocals", (event) => this.onEvent(event));
  }

  parts() {
    return this.voiceSelects.map((s) => s.value).filter(Boolean);
  }

  toggle(show, clip = null) {
    this.panel.hidden = !show;
    if (!show) return;
    const s = this.screen;
    for (const other of [s.exportPanel, s.stemsPanel, s.autoMixPanel, s.toMidiPanel, s.rebalancePanel, s.root.querySelector(".mixcheck-panel")])
      if (other) other.hidden = true;
    if (!this.running) {
      this.clip = clip;
      this.title.textContent = clip ? `Backing vocals for “${clip.name}”` : "Build backing vocals";
      this.setStatus("");
      this.update();
    }
  }

  update() {
    const key = this.screen.session?.key;
    const keyed = (key?.key ?? -1) >= 0;
    const keyName = keyed ? `${NOTE_NAMES[key.key]} ${key.minor ? "minor" : "major"}` : "";
    const count = this.parts().length;
    this.go.disabled = count === 0;
    this.go.textContent = count > 1 ? `Build ${count} voices` : "Build";
    const from = this.follow.value === "chords"
      ? `Harmonies follow the chords the other tracks play${keyed ? `, in ${keyName}` : ""}.`
      : keyed ? `Harmonies follow the scale of ${keyName}.` : "Harmonies follow the scale of the key the lead sings in.";
    this.note.textContent = `${from} Each voice is a copy of this clip on its own track, in a folded “Backing vocals” group, ` +
      "sung a little differently so they sound like separate takes. Fix any note afterwards in the clip's Notes view.";
  }

  setStatus(text, isError = false) {
    this.status.replaceChildren(html("span", isError ? "is-error" : "", text));
  }

  async start() {
    if (!this.clip) return;
    const parts = this.parts();
    if (!parts.length) return this.setStatus("Pick at least one voice.", true);
    savePrefs({ voices: this.voiceSelects.map((s) => s.value), feel: this.feel.value, follow: this.follow.value });
    this.setStatus("Starting…");
    const result = await this.daw("backingVocals", { action: "start", id: this.clip.id, parts, feel: this.feel.value, follow: this.follow.value });
    if (result?.ok === false) this.setStatus(result.error, true);
  }

  onEvent(event) {
    const running = event.state === "analysing";
    this.running = running;
    this.panel.classList.toggle("is-running", running);
    const s = this.screen;
    if (running) {
      const progress = Number(event.progress) || 0;
      this.bar.style.setProperty("--progress", String(progress));
      s.setJob("backing", { label: "Backing vocals", progress, cancel: () => this.daw("backingVocals", { action: "cancel" }),
                            open: () => { this.panel.hidden = false; } });
      this.setStatus(progress < 0.7 && this.follow.value === "chords" ? "Listening to the chords…" : "Listening to the lead…");
      return;
    }
    s.endJob("backing");
    if (event.state === "error") return this.setStatus(event.error, true);
    if (event.state === "cancelled") return this.setStatus("Cancelled.");
    if (event.state === "done") {
      this.panel.hidden = true;
      const count = Number(event.count) || 0;
      const chords = event.listened && !event.chords ? " (no chords were heard, so they follow the key)" : "";
      this.toast(`Added ${count} backing voice${count === 1 ? "" : "s"} in a folded group${event.key ? `, in ${event.key}` : ""}${chords}.`, "", "undo");
    }
  }
}
