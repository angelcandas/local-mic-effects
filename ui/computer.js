// Computer audio (Live, standalone app): the sound of the other apps (a video, a song) mixed
// with the voice, to hear and to record together. A switch, a level, a meter and options
// (where it comes from, whether the takes get it, muting the apps on the speakers).
// Engine: src/ComputerAudio.*, native function "computerAudio", "computer" in the "meters" event.
import * as Juce from "./juce/index.js";
import { html } from "./widgets.js";

const computerAudio = Juce.getNativeFunction("computerAudio");

const formatDb = (db) => (db <= -60 ? "off" : `${db > 0 ? "+" : ""}${Math.round(db)} dB`);

export class ComputerAudioPanel {
  // `anchor`: the element it goes in front of; `setStatus(text, className)`: the recorder's status line.
  constructor(anchor, setStatus) {
    this.setStatus = setStatus;
    this.state = null;
    this.element = html("div", "computer-audio");
    this.element.hidden = true; // until the app says it has it

    this.toggle = html("button", "pill computer-toggle");
    this.toggle.type = "button";
    this.toggle.setAttribute("aria-pressed", "false");
    this.toggle.title = "Mix the computer's sound (a video, any app) with your voice, to hear and record them together";
    this.toggle.append(html("span", "switch"), html("span", "", "Computer audio"));

    this.level = html("input", "computer-level");
    Object.assign(this.level, { type: "range", min: "-60", max: "12", step: "1", value: "-6" });
    this.level.setAttribute("aria-label", "Computer audio level");
    this.levelReadout = html("span", "computer-readout", "-6 dB");

    this.meter = html("span", "computer-meter");
    this.meterFill = html("span", "computer-meter-fill");
    this.meter.append(this.meterFill);
    this.meter.title = "The computer audio's level";

    this.optionsButton = html("button", "icon-button computer-options-button", "▾");
    this.optionsButton.type = "button";
    this.optionsButton.title = "Where the computer audio comes from, and options";
    this.optionsButton.setAttribute("aria-label", "Computer audio options");
    this.optionsButton.setAttribute("aria-expanded", "false");

    this.options = html("div", "computer-options");
    this.options.hidden = true;
    const sourceLabel = html("label", "computer-option");
    sourceLabel.append(html("span", "record-caption", "From"));
    this.source = html("select", "capture-select computer-source");
    this.source.setAttribute("aria-label", "Where the computer audio comes from");
    sourceLabel.append(this.source);

    const checkbox = (text, title) => {
      const label = html("label", "check computer-option");
      const input = html("input");
      input.type = "checkbox";
      label.title = title;
      label.append(input, ` ${text}`);
      return [label, input];
    };
    const [recordLabel, record] = checkbox("In recordings", "Recordings and Capture get your voice with the computer audio under it, lined up with what you heard");
    const [muteLabel, mute] = checkbox("Mute the apps on the speakers",
      "Hear the apps once, through MicFX, in time with your voice (they come back when you turn this off)");
    this.record = record;
    this.mute = mute;
    this.muteLabel = muteLabel;
    this.hint = html("p", "computer-hint");
    this.options.append(sourceLabel, recordLabel, muteLabel, this.hint);

    this.element.append(this.toggle, this.level, this.levelReadout, this.meter, this.optionsButton, this.options);
    anchor.before(this.element);

    this.toggle.addEventListener("click", () => this.set({ on: this.toggle.getAttribute("aria-pressed") !== "true" }));
    this.level.addEventListener("input", () => {
      this.levelReadout.textContent = formatDb(Number(this.level.value));
      clearTimeout(this.levelTimer);
      this.levelTimer = setTimeout(() => computerAudio("set", { level: Number(this.level.value) }), 30);
    });
    this.level.addEventListener("dblclick", () => {
      this.level.value = "-6";
      this.level.dispatchEvent(new Event("input"));
    });
    this.source.addEventListener("change", () => this.set({ source: this.source.value }));
    this.record.addEventListener("change", () => this.set({ inRecordings: this.record.checked }));
    this.mute.addEventListener("change", () => this.set({ muteOnSpeakers: this.mute.checked }));
    this.optionsButton.addEventListener("click", () => this.showOptions(this.options.hidden));
    document.addEventListener("pointerdown", (event) => {
      if (!this.options.hidden && !this.element.contains(event.target)) this.showOptions(false);
    });

    computerAudio("status").then((state) => this.apply(state));
  }

  showOptions(show) {
    // The device list can change (a loopback device plugged in): ask again on opening.
    if (show) computerAudio("status").then((state) => this.apply(state));
    this.options.hidden = !show;
    this.optionsButton.setAttribute("aria-expanded", String(show));
  }

  async set(changes) {
    const state = await computerAudio("set", changes);
    this.apply(state);
    if (state?.error) this.setStatus(state.error, "is-error");
    else if (changes.on === true) this.setStatus(`Computer audio on: ${this.sourceName(state)}`, "");
    else if (changes.on === false) this.setStatus("Computer audio off", "");
  }

  sourceName(state) {
    return state?.sources?.find((source) => source.id === state.source)?.name ?? "";
  }

  apply(state) {
    if (!state) return;
    this.state = state;
    this.element.hidden = !state.available;
    this.toggle.setAttribute("aria-pressed", String(Boolean(state.on)));
    this.element.classList.toggle("is-on", Boolean(state.on));

    if (document.activeElement !== this.level) {
      this.level.value = String(Math.round(state.level));
      this.levelReadout.textContent = formatDb(state.level);
    }

    this.source.replaceChildren();
    for (const source of state.sources ?? []) {
      const option = html("option", "", source.name);
      option.value = source.id;
      this.source.append(option);
    }
    if (!(state.sources ?? []).some((source) => source.id === state.source)) {
      const option = html("option", "", state.source ? `${state.source.replace(/^device:/, "")} (not found)` : "Pick a source");
      option.value = state.source ?? "";
      option.disabled = true;
      this.source.prepend(option);
    }
    this.source.value = state.source ?? "";

    this.record.checked = Boolean(state.inRecordings);
    this.mute.checked = Boolean(state.muteOnSpeakers);
    this.muteLabel.hidden = state.source !== "system";

    this.hint.textContent = state.system
      ? "If you hear nothing, allow MicFX in System Settings › Privacy & Security › Screen & System Audio Recording."
      : "Route the computer's sound to a loopback device (BlackHole or Loopback on a Mac, Stereo Mix on Windows, "
        + "a monitor source on Linux) and pick it here. MicFX itself must play somewhere else.";
  }

  // From the "meters" event: { peak, receiving } while it runs, undefined when it's off.
  setReading(reading) {
    const on = Boolean(reading);
    this.element.classList.toggle("is-waiting", on && !reading.receiving);
    const db = on && reading.peak > 0 ? 20 * Math.log10(reading.peak) : -60;
    this.meterFill.style.width = `${Math.max(0, Math.min(100, ((db + 60) / 60) * 100))}%`;
    this.meterFill.classList.toggle("is-hot", db > -1);

    // Turned off elsewhere (DAW mode pauses it and the toggle stays on).
    if (this.state?.on && !on && this.toggle.getAttribute("aria-pressed") === "true") this.meter.title = "Paused";
    else this.meter.title = "The computer audio's level";
  }
}
