// MicFX web UI. Runs inside the JUCE WebView; every control is bound to a processor
// parameter through the relays created in src/PluginEditor.cpp (relay name = parameter ID).
import * as Juce from "./juce/index.js";
import { CATEGORIES, MODULES, NOTE_NAMES, SCALES } from "./modules.js";
import { ARC_END, ARC_START, arcPath, clamp, formatDuration, formatValue, html, polar, svg } from "./widgets.js";
import { initDaw } from "./daw.js";

// Uncaught errors go to the app's log (VS Code debug console / terminal). In debug
// builds you can also right-click the UI -> Inspect Element for Safari's Web Inspector.
const logError = Juce.getNativeFunction("logError");
window.addEventListener("error", (event) => logError(`${event.message} (${event.filename}:${event.lineno})`));
window.addEventListener("unhandledrejection", (event) => logError(`Unhandled rejection: ${event.reason}`));

const METER_FLOOR_DB = -60;

let parameterDefaults = {};

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
// Choice parameter: segmented buttons for a few options, a dropdown for more.
class Choice {
  constructor(param, label, onUserChange) {
    this.state = Juce.getComboBoxState(param);
    this.onUserChange = onUserChange;
    this.element = html("div", "choice");
    this.element.append(html("div", "name", label));
    this.signature = null; // choices arrive from C++ after construction; build once now, rebuild then

    this.state.valueChangedEvent.addListener(() => this.render());
    this.state.propertiesChangedEvent.addListener(() => this.render());
    this.render();
  }

  select(index) {
    this.state.setChoiceIndex(index);
    this.onUserChange();
    this.render();
  }

  build(choices) {
    this.control?.remove();

    if (choices.length <= 3) {
      this.control = html("div", "segmented");
      choices.forEach((choice, index) => {
        const button = html("button", "", choice);
        button.addEventListener("click", () => this.select(index));
        this.control.append(button);
      });
    } else {
      this.control = html("select", "dropdown");
      choices.forEach((choice) => this.control.append(new Option(choice)));
      this.control.addEventListener("change", () => this.select(this.control.selectedIndex));
    }

    this.element.append(this.control);
  }

  render() {
    const choices = this.state.properties.choices ?? [];
    if (choices.join("|") !== this.signature) {
      this.signature = choices.join("|");
      this.build(choices);
    }

    const index = choices.length > 0 ? this.state.getChoiceIndex() : 0;

    if (this.control.tagName === "SELECT") this.control.selectedIndex = index;
    else [...this.control.children].forEach((button, i) => button.classList.toggle("is-selected", i === index));
  }
}

//==============================================================================
// On/off switch for a bool parameter that isn't a module's power button.
class Pill {
  constructor(param, label, onUserChange) {
    this.state = Juce.getToggleState(param);
    this.element = html("button", "pill");
    this.element.append(html("span", "switch"), html("span", "", label));
    this.element.addEventListener("click", () => {
      this.state.setValue(!this.state.getValue());
      onUserChange();
      this.render();
    });

    this.state.valueChangedEvent.addListener(() => this.render());
    this.render();
  }

  render() {
    this.element.setAttribute("aria-pressed", String(Boolean(this.state.getValue())));
  }
}

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
    row.append(html("span", "name", "Key"), (this.keyLabel = html("span", "name")));
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
// Decorative decay envelope that follows the reverb's size, damping and mix.
class ReverbGraphic {
  constructor(card) {
    this.element = svg("svg", { class: "reverb-graphic", viewBox: "0 0 400 60", preserveAspectRatio: "none" });
    card.addEventListener("knob-change", () => this.render());
    this.render();
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

    Juce.getNativeFunction("getPresets")().then((presets) => {
      this.presets = presets ?? [];
      this.buildMenu();
      this.render();
    });
  }

  buildMenu() {
    const groups = new Map();

    this.presets.forEach((preset, index) => {
      if (!groups.has(preset.category)) {
        const group = html("div", "preset-group");
        group.append(html("h3", "", preset.category));
        groups.set(preset.category, group);
        this.menu.append(group);
      }

      const button = html("button", "", preset.name);
      button.dataset.index = index;
      button.addEventListener("click", () => this.load(index));
      groups.get(preset.category).append(button);
    });
  }

  open() {
    this.menu.hidden = false;
    this.button.setAttribute("aria-expanded", "true");
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
      this.modified = false;
      this.render();
    });
  }

  step(delta) {
    this.load(Math.max(0, this.current) + delta);
  }

  // The host (or a restored session) may change the preset behind our back.
  syncFromHost(index) {
    if (index === this.current) return;
    this.current = index;
    this.modified = false;
    this.render();
  }

  markModified() {
    if (this.modified) return;
    this.modified = true;
    this.render();
  }

  render() {
    this.nameLabel.textContent = this.presets[this.current]?.name ?? "—";
    this.element.classList.toggle("is-modified", this.modified);
    this.menu.querySelectorAll("button").forEach((button) =>
      button.classList.toggle("is-current", Number(button.dataset.index) === this.current));
  }
}

//==============================================================================
// Builds the sidebar (every effect, grouped) and the chain (cards for effects that
// are switched on, in processing order).
class Rack {
  constructor({ sidebar, grid, empty, count, onUserChange }) {
    this.grid = grid;
    this.empty = empty;
    this.count = count;
    this.onUserChange = onUserChange;
    this.entries = [];

    const groups = {};
    for (const [key, category] of Object.entries(CATEGORIES)) {
      const group = html("div", "sidebar-group");
      group.style.setProperty("--cat", category.color);
      group.append(html("h3", "", category.name));
      sidebar.append(group);
      groups[key] = group;
    }

    for (const module of MODULES) {
      const power = Juce.getToggleState(module.power);
      const card = this.buildCard(module, power);
      const item = this.buildSidebarItem(module, power, card);

      groups[module.category].append(item);
      grid.append(card);
      this.entries.push({ module, power, card, item });

      power.valueChangedEvent.addListener(() => this.refresh());
    }

    this.refresh();
  }

  buildCard(module, power) {
    const card = html("section", `card${module.wide ? " wide" : ""}`);
    card.dataset.module = module.id;
    card.style.setProperty("--cat", CATEGORIES[module.category].color);
    card.hidden = true;

    const header = html("header");
    const powerButton = html("button", "power");
    powerButton.setAttribute("aria-label", `Turn ${module.name} off`);
    powerButton.title = "Turn off";
    powerButton.addEventListener("click", () => {
      power.setValue(false);
      this.onUserChange();
      this.refresh();
    });

    header.append(html("span", "step"), html("h2", "", module.name), powerButton);

    const controls = html("div", "controls");
    for (const control of module.controls)
      controls.append(this.buildControl(control, card));

    card.append(header, html("p", "desc", module.description), controls);
    return card;
  }

  buildControl(control, card) {
    switch (control.type) {
      case "knob": {
        const element = html("div");
        new Knob(element, { ...control, onUserChange: this.onUserChange });
        return element;
      }
      case "choice": return new Choice(control.param, control.label, this.onUserChange).element;
      case "toggle": return new Pill(control.param, control.label, this.onUserChange).element;
      case "tuner": return (this.tuner = new Tuner()).element;
      case "keys": return (this.keyPicker = new KeyPicker(control.param, control.scaleParam, this.onUserChange)).element;
      case "reverb-graphic": return new ReverbGraphic(card).element;
      default: throw new Error(`Unknown control type ${control.type}`);
    }
  }

  buildSidebarItem(module, power, card) {
    const item = html("button", "effect-item");
    item.style.setProperty("--cat", CATEGORIES[module.category].color);
    item.title = module.description;
    item.append(html("span", "led"), html("span", "", module.name));

    item.addEventListener("click", () => {
      const on = !power.getValue();
      power.setValue(on);
      this.onUserChange();
      this.refresh();

      if (on) {
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
const markModified = () => presetBar.markModified();

const rack = new Rack({
  sidebar: document.querySelector(".sidebar-groups"),
  grid: document.querySelector(".chain-grid"),
  empty: document.querySelector(".chain-empty"),
  count: document.querySelector(".active-count"),
  onUserChange: markModified,
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

    this.toggleButton.addEventListener("click", () => {
      const collapsed = studio.classList.toggle("is-collapsed");
      this.toggleButton.setAttribute("aria-expanded", String(!collapsed));
    });

    document.addEventListener("keydown", (event) => {
      if (event.key === " " && !isTyping(event) && !event.target.closest?.("button, [role=slider]")) {
        event.preventDefault();
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
      const label = track.isTake ? `Take ${++takeNumber}` : track.name;
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
    this.redraw();
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
const tracks = new Tracks(document.querySelector(".studio"));

const meters = Object.fromEntries(
  [...document.querySelectorAll("[data-meter]")].map((element) => [element.dataset.meter, new Meter(element)]),
);

window.__JUCE__.backend.addEventListener("meters", (status) => {
  meters.input.push(status.input);
  meters.output.push(status.output);
  rack.tuner?.update(status.note, status.target);
  rack.keyPicker?.setTarget(status.target);
  presetBar.syncFromHost(status.preset);
  recorderBar.update(status.recording);
  tracks.update(status.transport);
});

let lastFrame = performance.now();
requestAnimationFrame(function frame(now) {
  Object.values(meters).forEach((meter) => meter.tick(now, now - lastFrame));
  lastFrame = now;
  requestAnimationFrame(frame);
});

Juce.getNativeFunction("getParameterDefaults")().then((defaults) => {
  parameterDefaults = defaults ?? {};
});

initDaw();
