// MicFX web UI. Runs inside the JUCE WebView; every control is bound to a processor
// parameter through the relays created in src/PluginEditor.cpp (relay name = parameter ID).
import * as Juce from "./juce/index.js";
import { CATEGORIES, MODULE_KEYS, MODULES, NOTE_NAMES, SCALES, isControlShown } from "./modules.js";
import { createActivity, createFxView } from "./fxviews.js";
import { createPresetPicker, effectPresets, presetParams } from "./fxpresets.js";
import { ARC_END, ARC_START, arcPath, clamp, createFilePicker, formatDuration, formatValue, html, keyText, polar, releaseFocus, svg } from "./widgets.js";
import { deviceKit, initDaw } from "./daw.js";
import { DeviceCard } from "./devices.js";
import { mountLiveMacros } from "./livemacros.js";
import { LOOPER_KEYS, LoopPanel } from "./looper.js";
import { MidiLearn, markLearnable } from "./midilearn.js";
import { PracticeView } from "./practice.js";
import { SCENE_KEYS, SceneBar } from "./scenes.js";
import { SETLIST_KEYS, SetlistPanel } from "./setlist.js";
import { CapturePanel } from "./capture.js";
import { SoundCheckPanel } from "./soundcheck.js";
import { initSetup } from "./setup.js";

// Uncaught errors go to the app's log (VS Code debug console / terminal). In debug
// builds you can also right-click the UI -> Inspect Element for Safari's Web Inspector.
const logError = Juce.getNativeFunction("logError");
window.addEventListener("error", (event) => logError(`${event.message} (${event.filename}:${event.lineno})`));
window.addEventListener("unhandledrejection", (event) => logError(`Unhandled rejection: ${event.reason}`));

const METER_FLOOR_DB = -60;

let parameterDefaults = {};
let parameterInfo = {}; // { param: { min, max, skew, step, label, default, choices } }, for the effect cards

// Parameter kind by ID ("choice" | "toggle"; anything else is a slider), from the catalog.
const parameterKinds = Object.fromEntries(MODULES.flatMap((m) => [[m.power, "toggle"],
  ...m.controls.filter((c) => c.param && (c.type === "choice" || c.type === "toggle")).map((c) => [c.param, c.type])]));

// Scaled value -> [0, 1], mirroring juce::NormalisableRange (range + skew).
function toNormalised(state, scaled) {
  const { start, end, skew } = state.properties;
  return end === start ? 0 : Math.pow(clamp((scaled - start) / (end - start)), skew);
}

function noteName(note) {
  return `${NOTE_NAMES[((note % 12) + 12) % 12]}${Math.floor(note / 12) - 1}`;
}

//==============================================================================
// Rotary control bound to a slider relay.
class Knob {
  constructor(element, { param, label, bipolar = false, onUserChange = () => {} }) {
    this.element = element;
    this.id = param;
    this.bipolar = bipolar;
    this.onUserChange = onUserChange;
    this.state = Juce.getSliderState(param);

    this.arc = svg("path", { class: "value-arc" });
    this.pointer = svg("line", { class: "pointer" });
    const dial = svg("svg", { viewBox: "0 0 100 100" });
    dial.append(
      svg("path", { class: "track", d: arcPath(ARC_START, ARC_END, 44) }),
      this.arc,
      svg("circle", { class: "body", cx: 50, cy: 50, r: 32 }),
      this.pointer,
    );

    this.readout = html("div", "readout");
    element.classList.add("knob");
    element.append(dial, this.readout, html("div", "name", label));

    element.tabIndex = 0;
    element.setAttribute("role", "slider");
    element.setAttribute("aria-label", label);
    markLearnable(element, `param:${param}`, label);
    element.title = "Drag or scroll · Shift for fine control · Double-click to reset";

    this.state.valueChangedEvent.addListener(() => this.render());
    this.state.propertiesChangedEvent.addListener(() => this.render());

    element.addEventListener("pointerdown", (event) => this.onPointerDown(event));
    element.addEventListener("wheel", (event) => this.onWheel(event), { passive: false });
    element.addEventListener("keydown", (event) => this.onKeyDown(event));
    element.addEventListener("dblclick", () => this.resetToDefault());

    this.render();
  }

  get normalised() {
    return clamp(this.state.getNormalisedValue()) || 0;
  }

  setNormalised(value) {
    this.state.setNormalisedValue(clamp(value));
    this.onUserChange();
    this.render(); // The backend echoes the change as well; rendering now keeps drags snappy.
  }

  // Wraps a one-off change in a gesture so hosts record it as a single automation edit.
  change(value) {
    this.state.sliderDragStarted();
    this.setNormalised(value);
    this.state.sliderDragEnded();
  }

  onPointerDown(event) {
    event.preventDefault();
    this.element.focus();
    this.element.setPointerCapture(event.pointerId);
    this.state.sliderDragStarted();

    let lastY = event.clientY;
    let value = this.normalised;

    const move = (moveEvent) => {
      const pixelsForFullRange = moveEvent.shiftKey ? 900 : 220;
      value = clamp(value + (lastY - moveEvent.clientY) / pixelsForFullRange);
      lastY = moveEvent.clientY;
      this.setNormalised(value);
    };

    const end = () => {
      this.state.sliderDragEnded();
      this.element.removeEventListener("pointermove", move);
      this.element.removeEventListener("pointerup", end);
      this.element.removeEventListener("pointercancel", end);
    };

    this.element.addEventListener("pointermove", move);
    this.element.addEventListener("pointerup", end);
    this.element.addEventListener("pointercancel", end);
  }

  onWheel(event) {
    event.preventDefault();
    this.change(this.normalised - event.deltaY * (event.shiftKey ? 0.0004 : 0.002));
  }

  onKeyDown(event) {
    const step = event.shiftKey ? 0.002 : 0.02;
    const keys = {
      ArrowUp: this.normalised + step,
      ArrowRight: this.normalised + step,
      ArrowDown: this.normalised - step,
      ArrowLeft: this.normalised - step,
      Home: 0,
      End: 1,
    };

    if (!(event.key in keys)) return;
    event.preventDefault();
    this.change(keys[event.key]);
  }

  resetToDefault() {
    const defaultValue = parameterDefaults[this.id];
    if (defaultValue !== undefined) this.change(toNormalised(this.state, defaultValue));
  }

  render() {
    const angle = ARC_START + this.normalised * (ARC_END - ARC_START);
    const arcStart = this.bipolar ? 0 : ARC_START;
    this.arc.setAttribute("d", Math.abs(angle - arcStart) < 0.5 ? "" : arcPath(arcStart, angle, 44));

    const [x1, y1] = polar(angle, 12);
    const [x2, y2] = polar(angle, 25);
    Object.entries({ x1, y1, x2, y2 }).forEach(([name, value]) => this.pointer.setAttribute(name, value));

    const text = formatValue(this.state.getScaledValue(), this.state.properties.label);
    this.readout.textContent = text;
    this.element.setAttribute("aria-valuetext", text);
    this.element.dispatchEvent(new CustomEvent("knob-change", { bubbles: true }));
  }
}

//==============================================================================
//==============================================================================
// One-octave keyboard for choosing the autotune key; lights up the scale and the
// note currently being tuned to.
class KeyPicker {
  static WHITE = [0, 2, 4, 5, 7, 9, 11];
  static BLACK = [[1, 0], [3, 1], [6, 3], [8, 4], [10, 5]]; // [semitone, white key to its left]

  constructor(param, scaleParam, onUserChange) {
    this.key = Juce.getComboBoxState(param);
    this.scale = Juce.getComboBoxState(scaleParam);
    this.onUserChange = onUserChange;
    this.target = -1;

    this.element = html("div", "keys-picker");
    const row = html("div", "row");
    this.matchButton = html("button", "key-match", "Match song");
    this.matchButton.title = "Listen to the backing track and set the key and scale to the song's";
    this.matchButton.addEventListener("click", () => this.matchBackingTrack());
    row.append(html("span", "name", "Key"), this.matchButton, (this.keyLabel = html("span", "name")));
    this.piano = html("div", "piano");
    this.element.append(row, this.piano);

    this.keys = new Array(12);

    for (const semitone of KeyPicker.WHITE) this.keys[semitone] = this.addKey(semitone, "white");

    for (const [semitone, whiteIndex] of KeyPicker.BLACK) {
      const key = this.addKey(semitone, "black");
      key.style.left = `${((whiteIndex + 1) / 7) * 100 - 4.5}%`;
      this.keys[semitone] = key;
    }

    for (const state of [this.key, this.scale]) {
      state.valueChangedEvent.addListener(() => this.render());
      state.propertiesChangedEvent.addListener(() => this.render());
    }

    this.render();
  }

  addKey(semitone, colour) {
    const key = html("button", `key ${colour}`, NOTE_NAMES[semitone]);
    key.title = NOTE_NAMES[semitone];
    key.addEventListener("click", () => {
      this.key.setChoiceIndex(semitone);
      this.onUserChange();
      this.render();
    });
    this.piano.append(key);
    return key;
  }

  // The backing track's key: the key changes, and the scale becomes Major or Minor
  // unless it already fits (a minor pentatonic stays for a minor song).
  async matchBackingTrack() {
    const button = this.matchButton;
    button.disabled = true;
    button.textContent = "Listening…";
    try {
      const result = await Juce.getNativeFunction("detectBackingKey")();
      if (!result?.ok) {
        button.textContent = result?.error ? "No key found" : "Match song";
        button.title = result?.error ?? button.title;
        return;
      }
      const fitting = result.minor ? ["Minor", "Harmonic Minor", "Minor Pentatonic", "Blues"] : ["Major", "Major Pentatonic"];
      const choices = this.scale.properties.choices;
      if (!fitting.includes(choices[this.scale.getChoiceIndex()])) {
        const index = choices.indexOf(result.minor ? "Minor" : "Major");
        if (index >= 0) this.scale.setChoiceIndex(index);
      }
      this.key.setChoiceIndex(result.key);
      this.onUserChange();
      this.render();
      button.textContent = "Match song";
      button.title = `${result.track}: ${result.name}${result.confidence < 0.25 ? " (unsure)" : ""}`;
    } catch (error) {
      button.textContent = "Match song";
    } finally {
      button.disabled = false;
    }
  }

  setTarget(note) {
    const pitchClass = note >= 0 ? note % 12 : -1;
    if (pitchClass === this.target) return;
    this.target = pitchClass;
    this.render();
  }

  render() {
    const root = this.key.properties.choices.length ? this.key.getChoiceIndex() : 0;
    const scale = SCALES[this.scale.properties.choices.length ? this.scale.getChoiceIndex() : 0] ?? SCALES[0];
    this.keyLabel.textContent = `${NOTE_NAMES[root]} ${this.scale.properties.choices[this.scale.getChoiceIndex()] ?? ""}`;

    this.keys.forEach((key, semitone) => {
      key.classList.toggle("is-root", semitone === root);
      key.classList.toggle("in-scale", scale.includes((semitone - root + 12) % 12));
      key.classList.toggle("is-target", semitone === this.target);
    });
  }
}

//==============================================================================
// Shows the note autotune is pulling towards and how far off the sung pitch is.
class Tuner {
  constructor() {
    this.element = html("div", "tuner");
    this.note = html("div", "note", "—");
    this.needle = html("div", "needle");
    const scale = html("div", "scale");
    scale.append(this.needle);
    this.detail = html("div", "detail", "Sing to see your pitch");
    this.element.append(this.note, scale, this.detail);
  }

  update(sung, target) {
    const active = sung >= 0 && target >= 0;
    this.element.classList.toggle("is-active", active);

    if (!active) {
      this.note.textContent = "—";
      this.detail.textContent = "Sing to see your pitch";
      return;
    }

    const cents = Math.round((sung - target) * 100);
    const name = noteName(target);
    this.note.innerHTML = `${name.replace(/-?\d+$/, "")}<small>${name.match(/-?\d+$/)[0]}</small>`;
    this.needle.style.left = `${50 + clamp(cents, -50, 50)}%`;
    this.detail.textContent = cents === 0 ? "in tune" : `${cents > 0 ? "+" : ""}${cents}¢ ${cents > 0 ? "sharp" : "flat"}`;
  }
}

//==============================================================================
// The harmonizer: the chord it follows (Chords mode) and the note each voice sings.
class ChordReadout {
  constructor() {
    this.element = html("div", "chord-readout");
    this.chord = html("div", "chord", "—");
    this.voices = html("div", "voices");
    this.detail = html("div", "detail", "");
    this.element.append(this.chord, this.voices, this.detail);
    this.signature = "";
  }

  update(chord, voices) {
    const mode = Juce.getComboBoxState("harmonyMode").getChoiceIndex();
    const following = mode === 2;
    const sung = (voices ?? []).filter((note) => note >= 0);
    const signature = `${mode}|${chord}|${sung.join(",")}`;
    if (signature === this.signature) return;
    this.signature = signature;

    this.element.classList.toggle("is-following", following);
    this.element.classList.toggle("is-active", sung.length > 0);
    this.chord.textContent = following ? (chord || "—") : mode === 1 ? "Key" : "Intervals";

    this.voices.replaceChildren(...sung.map((note) => html("span", "voice", noteName(note))));

    const source = Juce.getComboBoxState("harmonySource").getChoiceIndex();
    this.detail.textContent = !following ? (sung.length ? "" : "Sing to hear the voices")
      : chord ? (sung.length ? "Voices on the chord's notes" : "Sing to hear the voices")
      : ["Load and play a backing track", "Play chords into input 2", "Play chords on a MIDI keyboard"][source] ?? "";
  }
}

//==============================================================================
// Decorative decay envelope that follows the reverb's size, damping and mix.
class ReverbGraphic {
  constructor(card) {
    this.element = svg("svg", { class: "reverb-graphic", viewBox: "0 0 400 60", preserveAspectRatio: "none" });
    card.addEventListener("knob-change", () => this.render());
    this.energy = 1;
    this.render();
  }

  update() { this.render(); }

  // Live: the tail swells with what comes out of the reverb and dies away with it.
  live(reading) {
    const level = reading ? Math.min(1, Math.max(0, (reading.out + 60) / 60)) : 1;
    this.energy = reading ? Math.max(0.08 + level * 0.92, this.energy * 0.93) : 1;
    this.element.style.setProperty("--energy", this.energy.toFixed(3));
  }

  render() {
    const value = (id) => Juce.getSliderState(id).getScaledValue() / 100;
    const size = value("reverbSize");
    const damping = value("reverbDamping");
    const mix = value("reverbMix");
    const bars = 72;
    const barWidth = 400 / bars;
    const decay = 0.05 + size * 0.45;

    this.element.replaceChildren(...Array.from({ length: bars }, (_, i) => {
      const t = i / bars;
      const envelope = Math.exp(-t / decay);
      const sparkle = 0.55 + 0.45 * Math.abs(Math.sin(i * 12.9898));
      const height = Math.max(1.5, 54 * envelope * sparkle * (1 - damping * t * 0.8) * (0.2 + 0.8 * mix));

      return svg("rect", {
        x: i * barWidth + 1, y: 58 - height, width: barWidth - 2, height, rx: 1.5,
        opacity: (0.3 + 0.7 * envelope).toFixed(3),
      });
    }));
  }
}

//==============================================================================
// Peak meter with instant attack, smooth fall-off and a held peak marker.
class Meter {
  constructor(element) {
    this.element = element;
    this.cover = html("div", "cover");
    this.peakLine = html("div", "peak");
    this.readout = html("div", "readout", "−∞");

    const track = html("div", "track");
    track.append(this.cover, this.peakLine);
    element.append(track, this.readout);

    this.level = METER_FLOOR_DB;
    this.peak = METER_FLOOR_DB;
    this.peakHeldAt = 0;
  }

  push(db) {
    this.level = Math.max(this.level, db);

    if (db >= this.peak) {
      this.peak = db;
      this.peakHeldAt = performance.now();
    }
  }

  tick(now, elapsedMs) {
    this.level = Math.max(METER_FLOOR_DB, this.level - elapsedMs * 0.03);

    if (now - this.peakHeldAt > 1200)
      this.peak = Math.max(METER_FLOOR_DB, this.peak - elapsedMs * 0.02);

    const toPercent = (db) => clamp((db - METER_FLOOR_DB) / -METER_FLOOR_DB) * 100;
    this.cover.style.height = `${100 - toPercent(this.level)}%`;
    this.peakLine.style.bottom = `calc(${toPercent(this.peak)}% - 2px)`;
    this.readout.textContent = this.peak <= METER_FLOOR_DB ? "−∞" : this.peak.toFixed(1);
    this.element.classList.toggle("is-hot", this.peak > -1);
  }
}

//==============================================================================
class PresetBar {
  constructor(element) {
    this.element = element;
    this.menu = element.querySelector(".preset-menu");
    this.button = element.querySelector(".preset-button");
    this.nameLabel = element.querySelector(".preset-name");
    this.presets = [];
    this.current = -1;
    this.modified = false;

    this.loadPreset = Juce.getNativeFunction("loadPreset");
    this.library = Juce.getNativeFunction("library");
    this.sounds = [];     // your saved sounds (library chains): [{ name, live }]
    this.factoryChains = []; // factory vocal chains (src/FactoryChains.h): [{ name, folder, description, live }]
    this.library("factoryChains").then((chains) => {
      this.factoryChains = (Array.isArray(chains) ? chains : []).filter((chain) => chain.live);
      if (this.presets.length) this.buildMenu();
    }).catch(() => {});
    this.soundName = "";  // the one playing, when it's one of yours
    this.refreshSounds();

    this.button.addEventListener("click", (event) => {
      event.stopPropagation();
      this.menu.hidden ? this.open() : this.close();
    });

    element.querySelectorAll("[data-preset-step]").forEach((button) =>
      button.addEventListener("click", () => this.step(Number(button.dataset.presetStep))));

    document.addEventListener("click", (event) => {
      if (!this.menu.contains(event.target)) this.close();
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") this.close();
    });

    // Favourites (preset names, in the order they were starred), kept in the app settings.
    this.favourites = [];
    this.uiPref = Juce.getNativeFunction("uiPref");

    Promise.all([Juce.getNativeFunction("getPresets")(), this.uiPref("favouritePresets").catch(() => "")]).then(([presets, saved]) => {
      this.presets = presets ?? [];
      try { this.favourites = (JSON.parse(saved || "[]") ?? []).filter((name) => this.presets.some((p) => p.name === name)); } catch { this.favourites = []; }
      this.buildMenu();
      this.render();
    });
  }

  refreshSounds() {
    return this.library("chains").then((chains) => {
      this.sounds = Array.isArray(chains) ? chains : [];
      if (!this.menu.hidden || this.presets.length) this.buildMenu();
    }).catch(() => {});
  }

  // The Live chain as it sounds now, saved under a name (typed in the menu).
  saveSound(name) {
    name = name.trim();
    if (!name) return;
    this.library("saveSound", name).then((result) => {
      if (result?.ok === false) { recorderBar.setStatus(result.error, "is-error"); return; }
      this.soundName = name;
      this.current = -1;
      this.modified = false;
      recorderBar.setStatus(`Saved “${name}” in your sounds.`);
      this.refreshSounds().then(() => this.render());
    });
  }

  loadSound(name) {
    this.close();
    this.library("loadSound", name).then((result) => {
      if (result?.ok === false) recorderBar.setStatus(result.error, "is-error");
      this.soundName = name;
      this.current = -1;
      this.modified = false;
      this.render();
    });
  }

  loadFactorySound(name) {
    this.close();
    this.library("loadFactorySound", name).then((result) => {
      if (result?.ok === false) recorderBar.setStatus(result.error, "is-error");
      this.soundName = name;
      this.current = -1;
      this.modified = false;
      this.render();
    });
  }

  deleteSound(name) {
    this.library("deleteChain", name).then(() => {
      if (this.soundName === name) this.soundName = "";
      this.refreshSounds().then(() => this.render());
    });
  }

  // Indices of the favourite presets, in favourite order.
  get favouriteIndices() {
    return this.favourites.map((name) => this.presets.findIndex((p) => p.name === name)).filter((i) => i >= 0);
  }

  toggleFavourite(index) {
    const name = this.presets[index]?.name;
    if (!name) return;
    this.favourites = this.favourites.includes(name) ? this.favourites.filter((n) => n !== name) : [...this.favourites, name];
    this.uiPref("favouritePresets", JSON.stringify(this.favourites));
    this.buildMenu();
    this.render();
  }

  buildMenu() {
    const query = this.search?.value ?? "";
    this.menu.replaceChildren();

    this.search = html("input", "preset-search");
    Object.assign(this.search, { type: "search", placeholder: "Search presets…", value: query, spellcheck: false });
    this.search.addEventListener("input", () => this.filter());
    this.search.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        const first = this.menu.querySelector(".preset-item:not([hidden]) [data-index]");
        if (first) this.load(Number(first.dataset.index));
      }
      if (event.key === "Escape") this.close();
      event.stopPropagation();
    });
    this.search.addEventListener("click", (event) => event.stopPropagation());
    const top = html("div", "preset-search-row");
    top.append(this.search, html("span", "preset-hint", "☆ adds a favourite · keys 1–9 load favourites"));
    this.menu.append(top);

    const item = (index, number) => {
      const preset = this.presets[index];
      const row = html("div", "preset-item");
      row.dataset.search = `${preset.name} ${preset.category}`.toLowerCase();
      const load = html("button", "preset-load", number ? `${number}  ${preset.name}` : preset.name);
      load.dataset.index = index;
      load.addEventListener("click", () => this.load(index));
      const starred = this.favourites.includes(preset.name);
      const star = html("button", `preset-star${starred ? " is-on" : ""}`, starred ? "★" : "☆");
      star.title = starred ? "Remove from favourites" : "Add to favourites";
      star.addEventListener("click", (event) => { event.stopPropagation(); this.toggleFavourite(index); });
      row.append(load, star);
      return row;
    };

    // Your sounds: save the current one, load or delete the saved ones.
    const yours = html("div", "preset-group is-yours");
    yours.append(html("h3", "", "Your sounds"));
    const saveRow = html("div", "preset-item preset-save-row");
    saveRow.dataset.search = "";
    const saveButton = html("button", "preset-load preset-save", "＋ Save this sound as…");
    saveButton.title = "Keep the effects that are on, their settings and their order under a name";
    saveButton.addEventListener("click", (event) => {
      event.stopPropagation();
      const input = html("input", "preset-search preset-save-name");
      Object.assign(input, { placeholder: "Name for this sound", maxLength: 60, spellcheck: false, value: this.soundName });
      input.addEventListener("click", (e) => e.stopPropagation());
      input.addEventListener("keydown", (e) => {
        e.stopPropagation();
        if (e.key === "Enter") { this.saveSound(input.value); this.close(); }
        else if (e.key === "Escape") this.buildMenu();
      });
      saveRow.replaceChildren(input);
      input.focus();
      input.select();
    });
    saveRow.append(saveButton);
    yours.append(saveRow);
    for (const sound of this.sounds) {
      const row = html("div", "preset-item");
      row.dataset.search = `${sound.name} your sounds`.toLowerCase();
      const load = html("button", "preset-load", sound.name);
      load.dataset.sound = sound.name;
      load.addEventListener("click", () => this.loadSound(sound.name));
      if (!sound.live) load.title = "Made in the DAW: Live plays one of each built-in effect, so some of it is left out";
      const remove = html("button", "preset-star preset-delete", "×");
      remove.title = `Delete “${sound.name}”`;
      remove.addEventListener("click", (event) => { event.stopPropagation(); this.deleteSound(sound.name); });
      row.append(load, remove);
      yours.append(row);
    }
    this.menu.append(yours);

    // Vocal chains: whole-voice sounds by style, level-matched for a voice at about -18 LUFS.
    if (this.factoryChains.length) {
      const chains = html("div", "preset-group is-chains");
      chains.append(html("h3", "", "Vocal chains"));
      for (const chain of this.factoryChains) {
        const row = html("div", "preset-item");
        row.dataset.search = `${chain.name} vocal chains ${chain.description}`.toLowerCase();
        const load = html("button", "preset-load", chain.name);
        load.title = chain.description;
        load.addEventListener("click", () => this.loadFactorySound(chain.name));
        row.append(load);
        chains.append(row);
      }
      this.menu.append(chains);
    }

    const favourites = this.favouriteIndices;
    if (favourites.length) {
      const group = html("div", "preset-group is-favourites");
      group.append(html("h3", "", "★ Favourites"));
      favourites.forEach((index, i) => group.append(item(index, i < 9 ? i + 1 : 0)));
      this.menu.append(group);
    }

    const groups = new Map();
    this.presets.forEach((preset, index) => {
      if (!groups.has(preset.category)) {
        const group = html("div", "preset-group");
        group.append(html("h3", "", preset.category));
        groups.set(preset.category, group);
        this.menu.append(group);
      }
      groups.get(preset.category).append(item(index, 0));
    });

    this.filter();
  }

  filter() {
    const words = (this.search?.value ?? "").toLowerCase().split(/\s+/).filter(Boolean);
    for (const group of this.menu.querySelectorAll(".preset-group")) {
      let visible = 0;
      for (const row of group.querySelectorAll(".preset-item")) {
        row.hidden = !row.classList.contains("preset-save-row") && !words.every((w) => row.dataset.search.includes(w));
        if (!row.hidden) visible++;
      }
      group.hidden = visible === 0;
    }
  }

  open() {
    this.menu.hidden = false;
    this.button.setAttribute("aria-expanded", "true");
    if (this.search) {
      this.search.value = "";
      this.filter();
      this.search.focus();
    }
  }

  // Stage keys 1–9: the favourites in order.
  loadFavourite(number) {
    const index = this.favouriteIndices[number - 1];
    if (index === undefined) return false;
    this.load(index);
    return true;
  }

  close() {
    this.menu.hidden = true;
    this.button.setAttribute("aria-expanded", "false");
  }

  load(index) {
    const count = this.presets.length;
    if (count === 0) return;

    const wrapped = ((index % count) + count) % count;
    this.close();
    this.loadPreset(wrapped).then((loaded) => {
      this.current = loaded;
      this.hostPreset = loaded;
      this.soundName = "";
      this.modified = false;
      this.render();
    });
  }

  step(delta) {
    this.load(Math.max(0, this.current) + delta);
  }

  // The host (or a restored session) may change the preset behind our back.
  syncFromHost(index) {
    // Only a change counts (one of your sounds leaves the host's program where it was).
    if (index === this.hostPreset) return;
    this.hostPreset = index;
    if (index === this.current) return;
    this.current = index;
    this.soundName = "";
    this.modified = false;
    this.render();
  }

  markModified() {
    if (this.modified) return;
    this.modified = true;
    this.render();
  }

  render() {
    this.nameLabel.textContent = this.soundName || (this.presets[this.current]?.name ?? "—");
    this.element.classList.toggle("is-modified", this.modified);
    this.menu.querySelectorAll("[data-index]").forEach((button) =>
      button.classList.toggle("is-current", Number(button.dataset.index) === this.current));
    this.menu.querySelectorAll("[data-sound]").forEach((button) =>
      button.classList.toggle("is-current", button.dataset.sound === this.soundName));
  }
}

//==============================================================================
// Builds the sidebar (every effect, grouped) and the chain (cards for effects that
// are switched on, in processing order). The order is the singer's: drag a card by its
// number (or focus the number and press ←/→) to move it; an effect switched on from
// the list goes to the end, as in Live.
class Rack {
  constructor({ sidebar, grid, empty, count, onUserChange }) {
    this.grid = grid;
    this.empty = empty;
    this.count = count;
    this.onUserChange = onUserChange;
    this.entries = [];
    this.chainOrder = Juce.getNativeFunction("chainOrder");

    const groups = {};
    const liveModules = MODULES.filter((m) => !m.dawOnly); // mastering effects are DAW-only
    for (const [key, category] of Object.entries(CATEGORIES)) {
      if (!liveModules.some((m) => m.category === key)) continue;
      const group = html("div", "sidebar-group");
      group.style.setProperty("--cat", category.color);
      group.append(html("h3", "", category.name));
      sidebar.append(group);
      groups[key] = group;
    }

    for (const module of liveModules) {
      const power = Juce.getToggleState(module.power);
      const card = this.buildCard(module, power);
      const item = this.buildSidebarItem(module, power, card);

      groups[module.category].append(this.buildSidebarRow(module, item, card));
      grid.append(card);
      this.entries.push({ module, power, card, item });

      power.valueChangedEvent.addListener(() => this.refresh());
    }

    this.refresh();
    this.buildSearch(sidebar);

    // A preset, a setlist song or the host can reorder the chain too.
    this.chainOrder("get").then((keys) => this.applyOrder(keys)).catch(() => {});
    window.__JUCE__.backend.addEventListener("chainOrder", (keys) => this.applyOrder(String(keys).split(",")));
  }

  // Puts the cards in the chain's order (moving only the ones out of place, so the rest
  // don't replay their entrance).
  // Autotune, pitch shift and harmonizer are one effect in the chain ("pitch"), so they
  // move together.
  applyOrder(keys) {
    if (!Array.isArray(keys)) return;
    const ordered = keys.flatMap((key) => this.entries.filter((entry) => MODULE_KEYS[entry.module.id] === key));
    for (const entry of this.entries) if (!ordered.includes(entry)) ordered.push(entry);
    this.entries = ordered;

    ordered.forEach(({ card }, index) => {
      if (this.grid.children[index] !== card) this.grid.insertBefore(card, this.grid.children[index] ?? null);
    });
    this.refresh();
  }

  // Moves an effect to a place among all the effects (on or off), then shows the order
  // the processor took.
  moveTo(module, index) {
    this.chainOrder("move", MODULE_KEYS[module.id], index).then((keys) => {
      this.applyOrder(keys);
      this.onUserChange();
    });
  }

  // Moves an effect before (or after) another card.
  moveNextTo(module, target, after) {
    const moving = MODULE_KEYS[module.id];
    const keys = [...new Set(this.entries.map((entry) => MODULE_KEYS[entry.module.id]))].filter((key) => key !== moving);
    const index = keys.indexOf(MODULE_KEYS[target.id]);
    if (index >= 0) this.moveTo(module, index + (after ? 1 : 0));
  }

  // ←/→ on a card's number: past the next card that's on (and not part of the same effect).
  nudge(module, direction) {
    const key = MODULE_KEYS[module.id];
    const visible = this.entries.filter((entry) => !entry.card.hidden);
    const at = visible.findIndex((entry) => entry.module === module);
    let next = at + direction;
    while (visible[next] && MODULE_KEYS[visible[next].module.id] === key) next += direction;
    const target = visible[next];
    if (target) this.moveNextTo(module, target.module, direction > 0);
  }

  // Dragging a card by its number onto another card (pointer events: WebKitGTK's
  // HTML drag and drop is unreliable).
  enableReorder(module, card, handle) {
    handle.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      const startX = event.clientX, startY = event.clientY;
      let dragging = false, target = null, after = false;
      handle.setPointerCapture(event.pointerId);

      const clear = () => this.grid.querySelectorAll(".drop-before, .drop-after").forEach((c) => c.classList.remove("drop-before", "drop-after"));
      const move = (moveEvent) => {
        if (!dragging && Math.hypot(moveEvent.clientX - startX, moveEvent.clientY - startY) < 5) return;
        dragging = true;
        card.classList.add("is-dragging");
        const over = document.elementFromPoint(moveEvent.clientX, moveEvent.clientY)?.closest(".live-card");
        const entry = this.entries.find((e) => e.card === over && !e.card.hidden && MODULE_KEYS[e.module.id] !== MODULE_KEYS[module.id]);
        clear();
        target = entry?.module ?? null;
        if (!entry) return;
        const box = over.getBoundingClientRect();
        after = moveEvent.clientX > box.left + box.width / 2;
        over.classList.add(after ? "drop-after" : "drop-before");
      };
      const up = () => {
        handle.removeEventListener("pointermove", move);
        handle.removeEventListener("pointerup", up);
        handle.removeEventListener("pointercancel", up);
        card.classList.remove("is-dragging");
        clear();
        if (dragging && target) this.moveNextTo(module, target, after);
      };
      handle.addEventListener("pointermove", move);
      handle.addEventListener("pointerup", up);
      handle.addEventListener("pointercancel", up);
    });
    handle.addEventListener("click", () => handle.focus()); // WebKit doesn't focus the number on click
    handle.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") return;
      event.preventDefault();
      event.stopPropagation(); // ←/→ elsewhere step through presets
      this.nudge(module, event.key === "ArrowRight" ? 1 : -1);
      requestAnimationFrame(() => handle.focus());
    });
  }

  // One effect's card: the same device card as the DAW's (ui/devices.js), reading and
  // writing the parameters through the relays. Live keeps its extras: the number to
  // drag it by, reset, echo tap, the tuner, chord readout and key picker, MIDI learn.
  buildCard(module, power) {
    const key = MODULE_KEYS[module.id];
    const insert = {
      id: module.id, type: key, module: module.id, name: "",
      values: new Proxy({}, { get: (_, param) => (typeof param === "string" ? this.getParameter(param) : undefined) }),
    };
    const access = {
      insert,
      get: (param) => this.getParameter(param),
      set: (param, value, first) => this.setLive(param, value, first),
      info: (param) => parameterInfo[param],
    };
    const verbs = {
      remove: () => {
        power.setValue(false);
        this.onUserChange();
        this.refresh();
      },
      openMenu: (x, y, anchor) => this.openCardMenu(module, power, device, x, y, anchor),
      switchAB: () => {},
    };
    const device = new DeviceCard(insert, access, this.cardKit(module), verbs);
    const card = device.element;
    card.dataset.module = module.id;
    card.classList.add("live-card");
    card.hidden = true;
    card.device = device;
    card.addEventListener("animationend", () => card.classList.add("is-settled"));
    card.addEventListener("contextmenu", (event) => {
      if (event.target.closest("input, select")) return;
      event.preventDefault();
      this.openCardMenu(module, power, device, event.clientX, event.clientY);
    });

    // The host, MIDI, presets, scenes or the other screen change values too: redraw
    // (once a frame) when any of this card's parameters moves.
    let pending = false;
    const redraw = () => {
      if (pending) return;
      pending = true;
      requestAnimationFrame(() => {
        pending = false;
        if (!card.hidden) device.update(insert);
        card.dispatchEvent(new Event("knob-change"));
      });
    };
    const params = new Set([`${key}InGain`, `${key}OutGain`]);
    for (const control of module.controls) for (const param of [control.param, control.scaleParam]) if (param) params.add(param);
    for (const param of params) this.relay(param).valueChangedEvent.addListener(redraw);
    card.redraw = redraw;
    return card;
  }

  // The device card's pieces, with the Live screen's extras.
  cardKit(module) {
    const rack = this;
    return {
      ...deviceKit(),
      live: true,
      activator: false,
      ab: false,
      ownPicture: (type) => type === "reverb", // its decay picture is one of its controls
      createExtra: (control, device) => rack.buildExtra(control, device),
      decorateControl: (control, element) => {
        if (control.param) markLearnable(element, `param:${control.param}`, control.label);
      },
      decorate: (device) => {
        const head = device.head;
        head.title = "";
        const step = html("span", "step");
        step.tabIndex = 0;
        step.title = "Drag to move this effect in the chain (or press ← / →)";
        step.setAttribute("role", "button");
        step.setAttribute("aria-label", `Move ${module.name} in the chain`);
        rack.enableReorder(module, device.element, step);
        head.prepend(step);

        head.querySelector(".insert-more").title = "More: move, trims, reset";
        const remove = head.querySelector(".insert-remove");
        remove.title = "Take it out of the chain (it stays in the list)";
        remove.setAttribute("aria-label", `Turn ${module.name} off`);
        markLearnable(remove, `param:${module.power}`, `${module.name} on/off`);

        // Reset is in the ⋯ menu (and a double-click resets one control).
        if (module.id === "echo") {
          const tap = html("button", "mini card-tap", "Tap");
          tap.title = "Tap the beat a few times to set the echo time (with Sync on, the tempo the echo follows)";
          tap.addEventListener("click", () => rack.tapEcho(tap));
          head.querySelector(".insert-trim-switch").before(tap);
        }
        device.body.prepend(html("p", "desc live-desc", module.description));
      },
    };
  }

  // Live-only controls the device card doesn't draw itself (undefined: the card does).
  buildExtra(control, device) {
    switch (control.type) {
      case "file": {
        const moduleFile = Juce.getNativeFunction("moduleFile");
        const picker = createFilePicker({ label: control.label, load: () => moduleFile(control.module, "choose"),
                                          clear: () => moduleFile(control.module, "clear") });
        moduleFile(control.module, "get").then((result) => picker.update(result?.file)).catch(() => {});
        return picker.element;
      }
      case "tuner": return (this.tuner = new Tuner()).element;
      case "chord": return (this.chordReadout = new ChordReadout()).element;
      case "keys": return (this.keyPicker = new KeyPicker(control.param, control.scaleParam, this.onUserChange)).element;
      case "reverb-graphic": {
        const graphic = new ReverbGraphic(device.element);
        device.view = graphic;
        return graphic.element;
      }
      default: return undefined;
    }
  }

  // ⋯ or right-click on a card.
  openCardMenu(module, power, device, x, y, anchor = null) {
    const { popMenu } = deviceKit();
    const visible = this.entries.filter((entry) => !entry.card.hidden);
    const at = visible.findIndex((entry) => entry.module === module);
    popMenu(x, y, ({ item, rule }) => {
      item("Move earlier", () => this.nudge(module, -1), { disabled: at <= 0, shortcut: "←" , hint: "Or drag it by its number" });
      item("Move later", () => this.nudge(module, 1), { disabled: at >= visible.length - 1, shortcut: "→" });
      rule();
      item("Show trims", () => device.setTrimsShown(!device.trimsShown), { checked: !device.gains.hidden });
      item("Reset to default settings", () => this.resetModule(module));
      rule();
      item("Take out of the chain", () => { power.setValue(false); this.onUserChange(); this.refresh(); }, { danger: true });
    }, anchor);
  }

  relay(param) {
    const kind = parameterKinds[param] ?? "slider";
    return kind === "choice" ? Juce.getComboBoxState(param) : kind === "toggle" ? Juce.getToggleState(param) : Juce.getSliderState(param);
  }

  // A value from a card, as a host gesture: a drag is one undo step (it ends a moment
  // after the last move).
  setLive(param, value, first) {
    const kind = parameterKinds[param] ?? "slider";
    if (kind === "toggle") Juce.getToggleState(param).setValue(value >= 0.5);
    else if (kind === "choice") Juce.getComboBoxState(param).setChoiceIndex(Math.round(value));
    else {
      const state = Juce.getSliderState(param);
      if (first || !this.gestureTimers?.[param]) state.sliderDragStarted();
      state.setNormalisedValue(toNormalised(state, value));
      clearTimeout(this.gestureTimers?.[param]);
      (this.gestureTimers ??= {})[param] = setTimeout(() => { state.sliderDragEnded(); delete this.gestureTimers[param]; }, 300);
    }
    this.onUserChange();
  }

  // From the "meters" event: each effect's live reading by module key ({ in, out,
  // spectrum? }), for the cards that are showing.
  live(readings) {
    for (const { module, card } of this.entries) {
      if (card.hidden) continue;
      card.device.live(readings?.[MODULE_KEYS[module.id]] ?? null);
    }
  }

  // Every control of the module, and its in/out gain, back to its default (the power stays on).
  resetModule(module) {
    liveHistory("label", `Reset ${module.name}`);
    const key = MODULE_KEYS[module.id];
    for (const control of module.controls) {
      const params = [control.param, control.scaleParam].filter(Boolean);
      for (const param of params) this.setParameter(param, parameterDefaults[param]);
    }
    for (const param of [`${key}InGain`, `${key}OutGain`]) this.setParameter(param, parameterDefaults[param]);
    this.onUserChange();
  }

  // Sets a parameter in real units (as a host gesture, so it's one undo step).
  setParameter(param, value) {
    if (value === undefined) return;
    const kind = parameterKinds[param] ?? "slider";
    if (kind === "toggle") Juce.getToggleState(param).setValue(Boolean(value));
    else if (kind === "choice") Juce.getComboBoxState(param).setChoiceIndex(Math.round(value));
    else {
      const state = Juce.getSliderState(param);
      state.sliderDragStarted();
      state.setNormalisedValue(toNormalised(state, value));
      state.sliderDragEnded();
    }
  }

  getParameter(param) {
    const kind = parameterKinds[param] ?? "slider";
    if (kind === "toggle") return Juce.getToggleState(param).getValue() ? 1 : 0;
    if (kind === "choice") return Juce.getComboBoxState(param).getChoiceIndex();
    return Juce.getSliderState(param).getScaledValue();
  }

  // Tap tempo for the echo: the average gap between taps becomes the echo time.
  tapEcho(button) {
    const now = performance.now();
    if (this.echoTaps?.length && now - this.echoTaps[this.echoTaps.length - 1] > 2000) this.echoTaps = [];
    this.echoTaps = [...(this.echoTaps ?? []), now].slice(-6);
    button.classList.remove("is-tapped");
    void button.offsetWidth;
    button.classList.add("is-tapped");
    if (this.echoTaps.length < 2) return;
    const gaps = this.echoTaps.slice(1).map((t, i) => t - this.echoTaps[i]);
    const ms = clamp(gaps.reduce((a, b) => a + b, 0) / gaps.length, 20, 1500);
    // The tapped beat is also the tempo a synced echo follows.
    const bpm = Math.round((60000 / ms) * 10) / 10;
    Juce.getNativeFunction("echoTapTempo")(bpm).catch(() => {});
    const synced = Math.round(Juce.getComboBoxState("delaySync")?.getChoiceIndex?.() ?? 0) > 0;
    if (synced) {
      button.textContent = `${Math.round(bpm)} bpm`;
    } else {
      const state = Juce.getSliderState("delayTime");
      state.sliderDragStarted();
      state.setNormalisedValue(toNormalised(state, ms));
      state.sliderDragEnded();
      button.textContent = `${Math.round(ms)} ms`;
    }
    clearTimeout(this.tapLabelTimer);
    this.tapLabelTimer = setTimeout(() => { button.textContent = "Tap"; }, 2500);
    this.onUserChange();
  }

  // The library in Live: each effect's presets fold out under it (a click loads one and
  // switches the effect on), and a search box finds effects and presets by name.
  buildSidebarRow(module, item, card) {
    const row = html("div", "effect-row");
    row.dataset.name = module.name.toLowerCase();
    const unfold = html("button", "effect-unfold", "▸");
    Object.assign(unfold, { type: "button", title: `Show ${module.name}'s presets` });
    const list = html("div", "effect-presets");
    list.hidden = true;
    const fill = () => effectPresets(Juce.getNativeFunction, module.id).then((presets) => {
      list.replaceChildren(...presets.map((preset) => {
        const b = html("button", `effect-preset${preset.factory ? "" : " is-yours"}`, preset.name);
        b.type = "button";
        b.title = `${module.name} with the "${preset.name}" preset`;
        b.addEventListener("click", () => this.loadPreset(module, card, preset));
        return b;
      }));
      if (!presets.length) list.append(html("div", "effect-preset-none", "No presets yet"));
      row.dataset.presets = presets.map((p) => p.name.toLowerCase()).join("\n");
    });
    row.open = (open) => {
      list.hidden = !open;
      unfold.textContent = open ? "▾" : "▸";
      unfold.setAttribute("aria-expanded", String(open));
    };
    unfold.addEventListener("click", () => { const open = list.hidden; row.open(open); if (open) fill(); });
    row.append(item, unfold, list);
    fill();
    return row;
  }

  loadPreset(module, card, preset) {
    const entry = this.entries.find((e) => e.module === module);
    if (entry && !entry.power.getValue()) entry.item.click(); // on, at the end of the chain
    card.device?.presetPicker?.load(preset, preset.factory);
  }

  buildSearch(sidebar) {
    const search = html("input", "sidebar-search");
    Object.assign(search, { type: "search", placeholder: "Find an effect or preset", spellcheck: false });
    search.addEventListener("keydown", (event) => {
      event.stopPropagation(); // typing mustn't fire the stage keys
      if (event.key === "Escape") { search.value = ""; search.dispatchEvent(new Event("input")); search.blur(); }
    });
    search.addEventListener("input", () => {
      const query = search.value.trim().toLowerCase();
      for (const row of sidebar.querySelectorAll(".effect-row")) {
        const presetHit = query.length > 1 && (row.dataset.presets ?? "").split("\n").some((name) => name.includes(query));
        row.hidden = Boolean(query) && !row.dataset.name.includes(query) && !presetHit;
        row.open(presetHit && !row.dataset.name.includes(query));
        for (const b of row.querySelectorAll(".effect-preset")) b.hidden = presetHit && !row.dataset.name.includes(query) && !b.textContent.toLowerCase().includes(query);
      }
      for (const group of sidebar.querySelectorAll(".sidebar-group")) group.hidden = ![...group.querySelectorAll(".effect-row")].some((row) => !row.hidden);
    });
    sidebar.before(search);
  }

  buildSidebarItem(module, power, card) {
    const item = html("button", "effect-item");
    item.style.setProperty("--cat", CATEGORIES[module.category].color);
    item.title = module.description;
    item.append(html("span", "led"), html("span", "", module.name));
    markLearnable(item, `param:${module.power}`, `${module.name} on/off`);

    item.addEventListener("click", () => {
      const on = !power.getValue();
      power.setValue(on);
      this.onUserChange();
      this.refresh();

      if (on) {
        // Switched on from the list: it goes to the end of the chain (unless another part
        // of the same effect, like the harmonizer for autotune, is already playing).
        const key = MODULE_KEYS[module.id];
        const playing = this.entries.some((entry) => entry.module !== module && MODULE_KEYS[entry.module.id] === key && !entry.card.hidden);
        if (!playing) this.chainOrder("last", key).then((keys) => this.applyOrder(keys)).catch(() => {});
        card.scrollIntoView({ behavior: "smooth", block: "nearest" });
        card.classList.remove("is-flashing");
        void card.offsetWidth; // restart the animation
        card.classList.add("is-flashing");
      }
    });

    return item;
  }

  refresh() {
    let active = 0;

    for (const { power, card, item } of this.entries) {
      const on = Boolean(power.getValue());
      if (!on) card.classList.remove("is-settled"); // shown again: it makes its entrance
      if (on && card.hidden) card.device.update(card.device.insert); // it didn't redraw while hidden
      card.hidden = !on;
      item.classList.toggle("is-on", on);
      item.setAttribute("aria-pressed", String(on));

      if (on) card.querySelector(".step").textContent = String(++active).padStart(2, "0");
    }

    this.count.textContent = `${active} on`;
    this.empty.hidden = active > 0;
  }
}

//==============================================================================
const presetBar = new PresetBar(document.querySelector(".preset-bar"));

//==============================================================================
// Undo for Live mode (src/LiveHistory): every change to the sound is one named step.
// Two buttons beside the preset bar, Ctrl+Z / Ctrl+Shift+Z (Cmd on a Mac).
const liveHistory = Juce.getNativeFunction("liveHistory");
const liveUndo = (() => {
  const group = html("div", "live-undo");
  const make = (action, glyph) => {
    const b = html("button", "icon-button", glyph);
    b.type = "button";
    b.disabled = true;
    b.addEventListener("click", () => run(action));
    group.append(b);
    return b;
  };
  const undoButton = make("undo", "↶");
  const redoButton = make("redo", "↷");
  document.querySelector(".preset-bar").append(group);
  let last = { undo: "", redo: "" };
  const show = (state) => {
    if (!state || typeof state !== "object") return;
    const loaded = state.undo && state.undo !== last.undo && state.undo.startsWith("Load preset") && !state.redo;
    last = state;
    undoButton.disabled = !state.undo;
    redoButton.disabled = !state.redo;
    undoButton.title = state.undo ? `Undo ${state.undo} (${keyText("Ctrl+Z")})` : "Nothing to undo";
    redoButton.title = state.redo ? `Redo ${state.redo} (${keyText("Ctrl+Shift+Z")})` : "Nothing to redo";
    undoButton.setAttribute("aria-label", undoButton.title);
    redoButton.setAttribute("aria-label", redoButton.title);
    if (loaded) recorderBar?.setStatus(`${state.undo.replace("Load preset", "Loaded")}. ${keyText("Ctrl+Z")} puts the sound before it back.`);
    if (typeof state.preset === "number") presetBar.syncFromHost(state.preset);
  };
  function run(action) {
    return liveHistory(action).then((state) => {
      show(state);
      const name = action === "undo" ? state?.redo : state?.undo;
      if (name) recorderBar?.setStatus(`${action === "undo" ? "Undid" : "Redid"} ${name}.`);
    });
  }
  window.__JUCE__.backend.addEventListener("liveHistory", show);
  liveHistory("state").then(show);
  return { run };
})();
const markModified = () => presetBar.markModified();

// The effect cards need the parameters' ranges, so the rack waits for them.
let rack = null;
Juce.getNativeFunction("getParameterInfo")().catch(() => ({})).then((info) => {
  parameterInfo = info ?? {};
  rack = new Rack({
    sidebar: document.querySelector(".sidebar-groups"),
    grid: document.querySelector(".chain-grid"),
    empty: document.querySelector(".chain-empty"),
    count: document.querySelector(".active-count"),
    onUserChange: markModified,
  });
  const grid = document.querySelector(".chain-grid");
  mountLiveMacros({ parent: grid.parentElement, before: grid, grid, kit: deviceKit(), parameterInfo, setLive: (param, value, first) => rack.setLive(param, value, first) });
});

// Input/output gain belong to the mic setup rather than the preset.
document.querySelectorAll(".levels .knob").forEach((element) =>
  new Knob(element, { param: element.dataset.param, label: element.dataset.label, bipolar: element.hasAttribute("data-bipolar") }));

//==============================================================================
// Record / stop, the dry-file option and where takes are saved. The C++ side does the
// recording; this only drives it and shows the elapsed time from the "meters" event.
class RecorderBar {
  constructor(element) {
    this.element = element;
    this.button = element.querySelector(".record-button");
    this.label = element.querySelector(".record-label");
    this.time = element.querySelector(".record-time");
    this.dry = element.querySelector(".record-dry");
    this.path = element.querySelector(".record-path");
    this.change = element.querySelector(".record-change");
    this.status = element.querySelector(".record-status");
    this.recording = false;
    this.busy = false;

    this.start = Juce.getNativeFunction("startRecording");
    this.stop = Juce.getNativeFunction("stopRecording");
    this.reveal = Juce.getNativeFunction("revealRecording");
    this.setDry = Juce.getNativeFunction("setRecordDry");
    this.chooseFolder = Juce.getNativeFunction("chooseRecordingFolder");

    this.button.addEventListener("click", () => this.toggle());
    this.dry.addEventListener("click", () => {
      const on = this.dry.getAttribute("aria-pressed") !== "true";
      this.dry.setAttribute("aria-pressed", String(on));
      this.setDry(on);
    });
    this.change.addEventListener("click", () => this.chooseFolder().then((settings) => this.applySettings(settings)));

    document.addEventListener("keydown", (event) => {
      if ((event.key === "r" || event.key === "R") && !isTyping(event) && !event.metaKey && !event.ctrlKey && !event.altKey) {
        event.preventDefault();
        this.toggle();
      }
    });

    Juce.getNativeFunction("getRecorderSettings")().then((settings) => this.applySettings(settings));
  }

  applySettings(settings) {
    if (!settings) return;
    // Shorten the home folder to ~ so the interesting end of the path stays visible.
    this.path.textContent = settings.folder.replace(/^(\/Users\/[^/]+|\/home\/[^/]+|[A-Z]:\\Users\\[^\\]+)/, "~");
    this.path.title = settings.folder;
    this.dry.setAttribute("aria-pressed", String(Boolean(settings.includeDry)));
  }

  async toggle() {
    if (this.busy) return;
    this.busy = true;
    this.button.disabled = true;

    try {
      if (this.recording) {
        this.showTake(await this.stop());
        this.setRecording(false);
      } else {
        const result = await this.start(this.dry.getAttribute("aria-pressed") === "true");
        if (result?.ok) {
          this.setStatus("");
          this.setRecording(true);
        } else {
          this.setStatus(result?.error || "Couldn't start recording", "is-error");
        }
      }
    } finally {
      this.busy = false;
      this.button.disabled = false;
    }
  }

  setRecording(recording) {
    this.recording = recording;
    this.element.classList.toggle("is-recording", recording);
    this.button.setAttribute("aria-pressed", String(recording));
    this.label.textContent = recording ? "Stop" : "Record";
    this.dry.disabled = recording;
    this.change.disabled = recording;
  }

  // From the "meters" event: seconds recorded, or -1 when not recording.
  update(seconds) {
    if (seconds === undefined || this.busy) return;

    if (seconds >= 0) {
      if (!this.recording) this.setRecording(true);
    } else if (this.recording) {
      // Stopped from the C++ side (e.g. the audio device changed): fetch the saved take.
      this.setRecording(false);
      this.stop().then((take) => this.showTake(take));
    }
  }

  showTake(take) {
    if (!take?.file) return;

    // The take also appears as a new track row; the full path is in the tooltip.
    this.status.replaceChildren(
      Object.assign(html("span", "", `Saved ${formatDuration(take.seconds)} take`), { title: take.file }),
    );

    if (take.dropped) this.status.append(html("span", "warning", "· some audio was dropped"));

    const show = html("button", "text-button", "Show");
    show.addEventListener("click", () => this.reveal());
    this.status.append(show);
    this.status.className = `record-status${take.dropped ? " is-warning" : ""}`;
  }

  setStatus(text, className = "") {
    this.status.textContent = text;
    this.status.className = `record-status ${className}`;
  }
}

// Typing in a field, or the Studio screen is showing (it has its own shortcuts).
function isTyping(event) {
  if (document.body.classList.contains("is-daw")) return true;
  return Boolean(event.target.closest?.("input:not([type=range]), select, textarea"));
}

//==============================================================================
// The multi-track player: transport buttons, a row per track (the backing track and
// every recorded take) with its waveform, and a playhead you can click to move.
class Tracks {
  constructor(studio) {
    this.studio = studio;
    this.container = studio.querySelector(".tracks");
    this.rowsElement = studio.querySelector(".tracks-rows");
    this.playhead = studio.querySelector(".playhead");
    this.time = studio.querySelector(".record-time");
    this.lengthLabel = studio.querySelector(".transport-length");
    this.toggleButton = studio.querySelector(".tracks-toggle");

    this.transport = Juce.getNativeFunction("transport");
    this.getTracks = Juce.getNativeFunction("getTracks");
    this.setTrack = Juce.getNativeFunction("setTrack");
    this.removeTrack = Juce.getNativeFunction("removeTrack");
    this.loadBacking = Juce.getNativeFunction("loadBackingTrack");

    this.rows = new Map(); // id -> { element, canvas, track }
    this.state = { position: 0, length: 0, playing: false, loading: 0 };

    studio.querySelectorAll("[data-transport]").forEach((button) =>
      button.addEventListener("click", () => {
        const action = button.dataset.transport;
        this.transport(action === "play" && this.state.playing ? "pause" : action);
      }));

    studio.querySelectorAll(".add-track, .add-track-inline").forEach((button) =>
      button.addEventListener("click", () => this.loadBacking()));
    this.openInDaw = studio.querySelector(".open-in-daw");
    this.openInDaw.addEventListener("click", () => this.sendToDaw());

    this.toggleButton.addEventListener("click", () => {
      const collapsed = studio.classList.toggle("is-collapsed");
      this.toggleButton.setAttribute("aria-expanded", String(!collapsed));
    });

    document.addEventListener("keydown", (event) => {
      if (event.key === " " && !isTyping(event)) { // Space always plays, whatever has focus
        releaseFocus(event);
        this.transport(this.state.playing ? "pause" : "play");
      }
    });

    new ResizeObserver(() => this.redraw()).observe(this.container);

    window.__JUCE__.backend.addEventListener("tracks", () => this.refresh());
    window.__JUCE__.backend.addEventListener("trackError", (message) => recorderBar.setStatus(message, "is-error"));
    this.refresh();
  }

  async refresh() {
    const tracks = (await this.getTracks()) ?? [];
    const seen = new Set();
    let takeNumber = 0;

    for (const track of tracks) {
      seen.add(track.id);
      const label = track.isTake ? `Recording ${++takeNumber}` : track.name;
      const row = this.rows.get(track.id) ?? this.createRow(track);
      row.track = track;
      row.name.textContent = label;
      row.name.title = track.file;
      row.mute.setAttribute("aria-pressed", String(track.muted));
      row.element.classList.toggle("is-muted", track.muted);
      row.volume.value = String(Math.round(Math.sqrt(track.gain) * 100));
      this.rowsElement.append(row.element); // keeps rows in track order
    }

    for (const [id, row] of this.rows) {
      if (!seen.has(id)) {
        row.element.remove();
        this.rows.delete(id);
      }
    }

    this.container.classList.toggle("has-tracks", this.rows.size > 0);
    this.openInDaw.hidden = this.rows.size === 0 || document.querySelector(".topbar .mode-switch").hidden;
    this.redraw();
  }

  // The backing track and every recording become DAW tracks, then the DAW opens.
  async sendToDaw() {
    const result = await Juce.getNativeFunction("singAlongToDaw")();
    if (!result?.ok) return recorderBar.setStatus(result?.error || "Couldn't open the session in the DAW.", "is-error");
    const count = result.tracks?.length ?? 0;
    recorderBar.setStatus(`Added ${count} track${count === 1 ? "" : "s"} to the DAW project.`);
    document.querySelector('.topbar .mode-switch [data-mode="daw"]')?.click();
  }

  createRow(track) {
    const element = html("div", `track ${track.isTake ? "track--take" : "track--backing"}`);
    const header = html("div", "track-header");
    const name = html("span", "track-name");
    const mute = Object.assign(html("button", "track-mute", "M"), { title: "Mute" });
    const volume = Object.assign(document.createElement("input"),
                                 { type: "range", min: 0, max: 150, className: "track-volume", title: "Volume" });
    const remove = Object.assign(html("button", "track-remove", "×"), { title: "Remove from the session (the file stays on disk)" });
    const lane = html("div", "track-lane");
    const canvas = document.createElement("canvas");

    header.append(name, mute, volume, remove);
    lane.append(canvas);
    element.append(header, lane);

    const row = { element, name, mute, volume, canvas, lane, track };

    mute.addEventListener("click", () => {
      const muted = mute.getAttribute("aria-pressed") !== "true";
      mute.setAttribute("aria-pressed", String(muted));
      element.classList.toggle("is-muted", muted);
      this.setTrack(row.track.id, { muted });
    });

    // Slider position is the square root of the gain, so the middle of the travel is
    // about -6 dB and the top is +3.5 dB.
    volume.addEventListener("input", () => {
      const position = Number(volume.value) / 100;
      this.setTrack(row.track.id, { gain: position * position });
    });
    volume.addEventListener("dblclick", () => {
      volume.value = "100";
      this.setTrack(row.track.id, { gain: 1 });
    });

    remove.addEventListener("click", () => this.removeTrack(row.track.id));

    lane.addEventListener("pointerdown", (event) => {
      const box = lane.getBoundingClientRect();
      this.transport("seek", clamp((event.clientX - box.left) / box.width) * this.viewLength());
    });

    this.rows.set(track.id, row);
    return row;
  }

  // Seconds shown across the lanes: the whole session plus a little room to record past the end.
  viewLength() {
    const end = Math.max(0, ...[...this.rows.values()].map(({ track }) => track.start + track.length));
    return Math.max(30, end * 1.1, this.state.position * 1.1);
  }

  redraw() {
    const view = this.viewLength();
    for (const row of this.rows.values()) this.drawWaveform(row, view);
    this.movePlayhead();
  }

  drawWaveform({ canvas, lane, track }, view) {
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(lane.clientWidth * ratio));
    const height = Math.max(1, Math.round(lane.clientHeight * ratio));
    if (canvas.width !== width) canvas.width = width;
    if (canvas.height !== height) canvas.height = height;

    const context = canvas.getContext("2d");
    context.clearRect(0, 0, width, height);

    const color = getComputedStyle(lane).getPropertyValue("--track-color").trim() || "#8c7bff";
    const pixelsPerSecond = width / view;
    const x0 = track.start * pixelsPerSecond;
    const x1 = (track.start + track.length) * pixelsPerSecond;

    // Clip background, then a min/max column per pixel.
    context.fillStyle = color;
    context.globalAlpha = 0.12;
    context.fillRect(x0, 0, x1 - x0, height);
    context.globalAlpha = 0.9;

    const peaks = track.peaks ?? [];
    const peaksPerPixel = 1 / (pixelsPerSecond * track.peakSeconds);
    const middle = height / 2;

    for (let x = Math.max(0, Math.floor(x0)); x < Math.min(width, x1); ++x) {
      const first = Math.floor((x - x0) * peaksPerPixel);
      const last = Math.max(first + 1, Math.floor((x + 1 - x0) * peaksPerPixel));
      let peak = 0;
      for (let i = Math.max(0, first); i < Math.min(peaks.length, last); ++i) peak = Math.max(peak, peaks[i]);
      const half = Math.max(0.5 * ratio, Math.min(1, peak) * (middle - 2 * ratio));
      context.fillRect(x, middle - half, 1, half * 2);
    }

    context.globalAlpha = 1;
  }

  movePlayhead() {
    const lane = this.rows.values().next().value?.lane;
    this.playhead.hidden = !lane;
    if (!lane) return;

    const offset = lane.offsetLeft;
    const x = offset + clamp(this.state.position / this.viewLength()) * lane.clientWidth;
    this.playhead.style.left = `${x}px`;
  }

  // From the "meters" event.
  update(transport) {
    if (!transport) return;
    const viewBefore = this.viewLength();
    this.state = transport;

    this.studio.classList.toggle("is-playing", transport.playing);
    this.time.textContent = formatDuration(transport.position);
    this.lengthLabel.textContent = transport.length > 0 ? `/ ${formatDuration(transport.length)}` : "";

    // Recording past the end widens the view, so the waveforms have to be redrawn.
    if (Math.abs(this.viewLength() - viewBefore) > 1e-6) this.redraw();
    else this.movePlayhead();
  }
}

const recorderBar = new RecorderBar(document.querySelector(".recorder"));
const capturePanel = new CapturePanel(document.querySelector(".record-folder"), (text, className) => recorderBar.setStatus(text, className));
const tracks = new Tracks(document.querySelector(".studio"));

const practiceView = new PracticeView({ section: document.querySelector(".practice"),
                                        toggle: document.querySelector(".practice-toggle"),
                                        workspace: document.querySelector(".workspace") });

const loopPanel = new LoopPanel(document.querySelector(".looper"), document.querySelector(".loop-toggle"));

// MIDI learn: the stage buttons can be mapped too.
markLearnable(document.querySelector('[data-preset-step="-1"]'), "action:preset.previous", "Previous preset");
markLearnable(document.querySelector('[data-preset-step="1"]'), "action:preset.next", "Next preset");
markLearnable(document.querySelector(".panic-button"), "action:mute", "Mute (panic)");
markLearnable(document.querySelector(".recorder .record-button"), "action:record", "Record");
const sceneBar = new SceneBar(document.querySelector(".scene-bar"));
const midiLearn = new MidiLearn(document.querySelector(".midi-button"));
const setlistPanel = new SetlistPanel(document.querySelector(".setlist-button"), (text) => recorderBar.setStatus(text));
window.__JUCE__.backend.addEventListener("midiAction", (action) => {
  if (action === "record") recorderBar.toggle();
});

const meters = Object.fromEntries(
  [...document.querySelectorAll("[data-meter]")].map((element) => [element.dataset.meter, new Meter(element)]),
);

window.__JUCE__.backend.addEventListener("meters", (status) => {
  meters.input.push(status.input);
  if (micCheck) micCheck.peak = Math.max(micCheck.peak, status.input);
  meters.output.push(status.output);
  rack?.tuner?.update(status.note, status.target);
  rack?.chordReadout?.update(status.chord, status.voices);
  if (!document.hidden) rack?.live(status.fx);
  rack?.keyPicker?.setTarget(status.target);
  presetBar.syncFromHost(status.preset);
  recorderBar.update(status.recording);
  capturePanel.setAvailable(status.capture);
  tracks.update(status.transport);
  setlistPanel.followPosition(status.transport?.position);
  if (status.looper) loopPanel.update(status.looper);
  midiLearn.update(status.midi);
  if (status.scenes) sceneBar.update(status.scenes);
  if (typeof status.muted === "boolean" && status.muted !== liveState.muted) applyLiveState({ muted: status.muted });
});

let lastFrame = performance.now();
requestAnimationFrame(function frame(now) {
  Object.values(meters).forEach((meter) => meter.tick(now, now - lastFrame));
  lastFrame = now;
  requestAnimationFrame(frame);
});

//==============================================================================
// Performance controls: panic mute, hold-for-dry, mic check and stage keys.
const liveControl = Juce.getNativeFunction("liveControl");
const panicButton = document.querySelector(".panic-button");
const dryButton = document.querySelector(".dry-button");
const micCheckButton = document.querySelector(".mic-check");
const liveState = { muted: false, dry: false };

function applyLiveState(state) {
  if (!state) return;
  Object.assign(liveState, state);
  panicButton.setAttribute("aria-pressed", String(liveState.muted));
  panicButton.textContent = liveState.muted ? "Muted" : "Mute";
  document.body.classList.toggle("is-muted", liveState.muted);
  dryButton.setAttribute("aria-pressed", String(liveState.dry));
  document.body.classList.toggle("is-dry", liveState.dry);
}

const setMuted = (on) => liveControl("mute", on).then(applyLiveState);
const setDry = (on) => { if (on !== liveState.dry) { liveState.dry = on; liveControl("dry", on).then(applyLiveState); } };
panicButton.addEventListener("click", () => setMuted(!liveState.muted));

dryButton.addEventListener("pointerdown", (event) => { dryButton.setPointerCapture?.(event.pointerId); setDry(true); });
for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) dryButton.addEventListener(type, () => setDry(false));
window.addEventListener("blur", () => setDry(false));

// Mic check: listens for 5 s, then moves the input gain so the loudest peak sits at -12 dB.
let micCheck = null;
const MIC_CHECK_TARGET = -12;
micCheckButton.addEventListener("click", () => {
  if (micCheck) return;
  micCheck = { peak: -100, until: performance.now() + 5000 };
  micCheckButton.classList.add("is-busy");
  const tick = () => {
    const left = Math.ceil((micCheck.until - performance.now()) / 1000);
    if (left > 0) {
      micCheckButton.textContent = `Sing… ${left}`;
      return setTimeout(tick, 200);
    }
    const { peak } = micCheck;
    micCheck = null;
    micCheckButton.classList.remove("is-busy");
    const state = Juce.getSliderState("inputGain");
    const current = state.getScaledValue();
    if (peak < -60) {
      micCheckButton.textContent = "No sound";
    } else {
      const gain = clamp(current + (MIC_CHECK_TARGET - peak), state.properties.start, state.properties.end);
      liveHistory("label", "Mic check");
      state.sliderDragStarted();
      state.setNormalisedValue(toNormalised(state, gain));
      state.sliderDragEnded();
      micCheckButton.textContent = `${gain >= 0 ? "+" : ""}${gain.toFixed(1)} dB`;
    }
    setTimeout(() => { micCheckButton.textContent = "Mic check"; }, 3000);
  };
  tick();
});

// Sound check: sing for 10-20 s and the chain is set for the voice, mic and room (ui/soundcheck.js).
new SoundCheckPanel(document.querySelector(".sound-check"));

// The "?" overlay with the Live keys.
const LIVE_KEYS = [["Space", "Play / pause the backing track"], ["R", "Record"], ["Ctrl+← / →", "Previous / next preset"],
                   ["1 – 9", "Load favourite preset 1 to 9 (star presets in the preset menu)"],
                   ["Ctrl+Z / Ctrl+Shift+Z", "Undo / redo any change to the sound (presets, knobs, scenes, resets)"], ["M", "Mute output (panic)"],
                   ["Hold D", "Hear your voice without effects"], ...LOOPER_KEYS, ...SCENE_KEYS, ...SETLIST_KEYS, ["?", "This list"], ["F1", "The user manual"], ["Ctrl+,", "Settings"], ["Esc", "Close"]];
function toggleLiveKeys(show) {
  let overlay = document.querySelector(".live-keys");
  if (!overlay) {
    overlay = html("div", "live-keys");
    overlay.hidden = true;
    const panel = html("div", "live-keys-panel");
    const close = html("button", "live-keys-close", "×");
    close.setAttribute("aria-label", "Close");
    close.addEventListener("click", () => toggleLiveKeys(false));
    panel.append(html("h2", "", "Stage keys"), close);
    for (const [keys, action] of LIVE_KEYS) {
      const row = html("div", "live-key");
      row.append(html("kbd", "", keyText(keys)), html("span", "", action));
      panel.append(row);
    }
    overlay.append(panel);
    overlay.addEventListener("pointerdown", (event) => { if (event.target === overlay) toggleLiveKeys(false); });
    document.body.append(overlay);
  }
  overlay.hidden = !(show ?? overlay.hidden);
}
document.querySelector(".keys-button").addEventListener("click", () => toggleLiveKeys());
document.addEventListener("micfx:liveKeys", () => toggleLiveKeys(true));

document.addEventListener("keydown", (event) => {
  if (isTyping(event) || event.altKey) return;
  // A whole-sound change never sits on a bare arrow key: Ctrl+← / → (Cmd on a Mac) steps presets.
  if (event.metaKey || event.ctrlKey) {
    const letter = event.key.toLowerCase();
    if (event.key === "ArrowLeft" || event.key === "ArrowRight") { event.preventDefault(); presetBar.step(event.key === "ArrowLeft" ? -1 : 1); }
    else if (letter === "z" || letter === "y") { event.preventDefault(); liveUndo.run(letter === "y" || event.shiftKey ? "redo" : "undo"); }
    return;
  }
  if (event.target.closest?.("[role=slider]") && event.key.startsWith("Arrow")) return;
  const key = event.key.toLowerCase();
  if (key === "m" && !event.repeat) { event.preventDefault(); setMuted(!liveState.muted); }
  else if (key === "d") { event.preventDefault(); setDry(true); }
  else if (/^[1-9]$/.test(event.key)) {
    event.preventDefault();
    if (!presetBar.loadFavourite(Number(event.key))) recorderBar.setStatus(`No favourite ${event.key} yet: star presets in the preset menu.`);
  }
  else if (event.key === "?" || event.key === "/") { event.preventDefault(); toggleLiveKeys(); }
  else if (event.key === "Escape") toggleLiveKeys(false);
});
document.addEventListener("keyup", (event) => { if (event.key.toLowerCase() === "d") setDry(false); });

Juce.getNativeFunction("getParameterDefaults")().then((defaults) => {
  parameterDefaults = defaults ?? {};
});

initDaw();
initSetup(); // after initDaw: the status strip goes into both hint lines
