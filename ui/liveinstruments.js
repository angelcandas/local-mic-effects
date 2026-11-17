// Live instruments: a MIDI keyboard and a MIDI drum kit (or two of anything) playing the MicFX
// instruments next to the voice, heard, looped and recorded with it. One row per slot: on,
// instrument and sound, which MIDI input and channel, level, meter, and the instrument's window.
// Engine: src/LiveInstruments.*, native function "liveInstruments", "instruments" in the "meters" event.
import * as Juce from "./juce/index.js";
import { html } from "./widgets.js";

const liveInstruments = Juce.getNativeFunction("liveInstruments");
const formatDb = (db) => (db <= -60 ? "off" : `${db > 0 ? "+" : ""}${Math.round(db)} dB`);
const SLOT_NAMES = ["Keys", "Drums"];

const option = (value, text) => Object.assign(html("option", "", text), { value: String(value) });

export class LiveInstrumentsPanel {
  constructor(element, toggleButton) {
    this.element = element;
    this.toggleButton = toggleButton;
    this.state = null;
    this.rows = [];

    const head = html("div", "inst-head");
    head.append(html("h2", "", "Instruments"),
      html("span", "inst-hint", "Play a MIDI keyboard or drum kit with your voice: heard, looped and recorded together"));
    this.list = html("div", "inst-rows");
    this.element.append(head, this.list);

    toggleButton.addEventListener("click", () => this.show(this.element.hidden));
    Juce.getNativeFunction("uiPref")("instrumentsShown").then((shown) => this.show(shown === "1", false)).catch(() => {});
    window.__JUCE__.backend.addEventListener("liveInstruments", (state) => this.apply(state));
    liveInstruments("status").then((state) => this.apply(state));
  }

  show(on, remember = true) {
    this.element.hidden = !on;
    this.toggleButton.setAttribute("aria-pressed", String(on));
    if (remember) Juce.getNativeFunction("uiPref")("instrumentsShown", on ? "1" : "0");
    // MIDI devices come and go: ask again on opening.
    if (on) liveInstruments("status").then((state) => this.apply(state));
  }

  send(action, options) {
    return liveInstruments(action, options).then((state) => this.apply(state));
  }

  buildRow(index) {
    const row = { element: html("div", "inst-row") };
    row.toggle = html("button", "pill inst-toggle");
    row.toggle.type = "button";
    row.toggle.setAttribute("aria-pressed", "false");
    row.toggle.append(html("span", "switch"), html("span", "inst-name", SLOT_NAMES[index] ?? `Slot ${index + 1}`));
    row.toggle.title = "Turn this instrument on or off";

    row.instrument = html("select", "capture-select inst-instrument");
    row.instrument.setAttribute("aria-label", "Instrument");
    row.preset = html("select", "capture-select inst-preset");
    row.preset.setAttribute("aria-label", "Sound");
    row.edit = html("button", "text-button inst-edit", "Edit sound…");
    row.edit.type = "button";
    row.edit.title = "Open the instrument's own window";

    row.input = html("select", "capture-select inst-input");
    row.input.setAttribute("aria-label", "MIDI input");
    row.input.title = "Which MIDI device plays it. Any: every device that no other row has picked";
    row.channel = html("select", "capture-select inst-channel");
    row.channel.setAttribute("aria-label", "MIDI channel");
    row.channel.title = "Which MIDI channel plays it (drum kits usually send on 10)";
    row.channel.append(option(0, "All ch."));
    for (let channel = 1; channel <= 16; ++channel) row.channel.append(option(channel, `Ch. ${channel}`));

    row.level = html("input", "computer-level inst-level");
    Object.assign(row.level, { type: "range", min: "-60", max: "12", step: "1", value: "0" });
    row.level.setAttribute("aria-label", "Level");
    row.levelReadout = html("span", "computer-readout", "0 dB");
    row.meter = html("span", "computer-meter");
    row.meterFill = html("span", "computer-meter-fill");
    row.meter.append(row.meterFill);
    row.activity = html("span", "inst-activity");
    row.activity.title = "Lights when notes come in";

    row.element.append(row.toggle, row.activity, row.instrument, row.preset, row.edit, html("span", "record-caption", "MIDI"),
      row.input, row.channel, row.level, row.levelReadout, row.meter);

    const slot = index;
    row.toggle.addEventListener("click", () => this.send("set", { slot, on: row.toggle.getAttribute("aria-pressed") !== "true" }));
    row.instrument.addEventListener("change", () => this.send("set", { slot, key: row.instrument.value }));
    row.preset.addEventListener("change", () => this.send("preset", { slot, name: row.preset.value }));
    row.edit.addEventListener("click", () => this.send("edit", { slot }));
    row.input.addEventListener("change", () => this.send("set", { slot, input: row.input.value }));
    row.channel.addEventListener("change", () => this.send("set", { slot, channel: Number(row.channel.value) }));
    row.level.addEventListener("input", () => {
      row.levelReadout.textContent = formatDb(Number(row.level.value));
      clearTimeout(row.levelTimer);
      row.levelTimer = setTimeout(() => liveInstruments("set", { slot, level: Number(row.level.value) }), 30);
    });
    row.level.addEventListener("dblclick", () => {
      row.level.value = "0";
      row.level.dispatchEvent(new Event("input"));
    });
    return row;
  }

  apply(state) {
    if (!state) return;
    this.state = state;
    state.slots.forEach((slot, index) => {
      if (!this.rows[index]) {
        this.rows[index] = this.buildRow(index);
        this.list.append(this.rows[index].element);
      }
      const row = this.rows[index];
      row.toggle.setAttribute("aria-pressed", String(Boolean(slot.on)));
      row.element.classList.toggle("is-on", Boolean(slot.on));

      row.instrument.replaceChildren(...state.instruments.map((instrument) => option(instrument.key, instrument.name)));
      row.instrument.value = slot.key;

      const presets = state.instruments.find((instrument) => instrument.key === slot.key)?.presets ?? [];
      row.preset.replaceChildren(...presets.map((name) => option(name, name)));
      if (slot.preset && !presets.includes(slot.preset)) row.preset.prepend(option(slot.preset, slot.preset));
      if (!slot.preset) row.preset.prepend(option("", slot.on ? "Edited" : "First sound"));
      row.preset.value = slot.preset ?? "";
      row.preset.disabled = !slot.on;
      row.edit.disabled = !slot.on;

      const inputs = [option("any", "Any input"), ...(state.inputs ?? []).map((input) => option(input.id, input.name))];
      if (slot.input !== "any" && !(state.inputs ?? []).some((input) => input.id === slot.input))
        inputs.push(option(slot.input, "Not connected"));
      row.input.replaceChildren(...inputs);
      row.input.value = slot.input;
      row.input.hidden = !state.deviceInputs;
      row.channel.value = String(slot.channel);

      if (document.activeElement !== row.level) {
        row.level.value = String(Math.round(slot.level));
        row.levelReadout.textContent = formatDb(slot.level);
      }
    });
  }

  // From the "meters" event: [{ peak, active }] per slot.
  setReadings(readings) {
    (readings ?? []).forEach((reading, index) => {
      const row = this.rows[index];
      if (!row) return;
      const db = reading.peak > 0 ? 20 * Math.log10(reading.peak) : -60;
      row.meterFill.style.width = `${Math.max(0, Math.min(100, ((db + 60) / 60) * 100))}%`;
      row.meterFill.classList.toggle("is-hot", db > -1);
      if (reading.active) {
        row.activity.classList.add("is-lit");
        clearTimeout(row.activityTimer);
        row.activityTimer = setTimeout(() => row.activity.classList.remove("is-lit"), 150);
      }
    });
  }
}
