// Capture (Live): the app keeps the last few minutes of the voice (processed and dry) so a
// take nobody pressed Record for can still be saved. "Capture" saves the newest part as a
// take on a new track row, like a recording. The memory (3 minutes by default, or off) is a
// setting. Engine: src/CaptureRing.*, native function "capture", "capture" in the "meters" event.
import * as Juce from "./juce/index.js";
import { formatDuration, html } from "./widgets.js";

const capture = Juce.getNativeFunction("capture");
const AMOUNTS = [[30, "30 s"], [60, "1 min"], [180, "3 min"], [0, "All"]];
const KEEP = [[0, "Off"], [1, "1 min"], [3, "3 min"], [5, "5 min"]];

const select = (options, value, label) => {
  const element = html("select", "capture-select");
  element.setAttribute("aria-label", label);
  for (const [number, text] of options) {
    const option = html("option", "", text);
    option.value = String(number);
    element.append(option);
  }
  element.value = String(value);
  return element;
};

export class CapturePanel {
  // `anchor`: the element it goes in front of; `setStatus(text, className)`: the recorder's status line.
  constructor(anchor, setStatus) {
    this.setStatus = setStatus;
    this.busy = false;
    this.element = html("div", "capture");

    this.button = html("button", "pill capture-button", "Capture");
    this.button.type = "button";
    this.button.title = "Save the last minutes you sang as a take, even if you weren't recording";
    this.amount = select(AMOUNTS, 180, "How much to capture");
    this.keep = select(KEEP, 3, "How long Capture keeps");
    this.keep.title = "How much Capture keeps in memory (about 35 MB per minute at 48 kHz)";
    this.readout = html("span", "capture-readout");
    const keepLabel = html("span", "record-caption", "Keep");
    this.element.append(this.button, this.amount, keepLabel, this.keep, this.readout);
    anchor.before(this.element);

    this.button.addEventListener("click", () => this.save());
    this.keep.addEventListener("change", async () => this.apply(await capture("settings", { minutes: Number(this.keep.value) })));

    capture("status").then((state) => this.apply(state));
  }

  apply(state) {
    if (!state) return;
    this.keep.value = String(KEEP.some(([m]) => m === state.minutes) ? state.minutes : 3);
    this.setAvailable(state.on ? state.seconds : -1);
  }

  // From the "meters" event: seconds kept, or -1 when Capture is off.
  setAvailable(seconds) {
    const on = seconds !== undefined && seconds >= 0;
    this.element.classList.toggle("is-off", !on);
    this.button.disabled = !on || this.busy;
    this.amount.disabled = !on;
    this.readout.textContent = on ? `${formatDuration(seconds)} kept` : "off";
  }

  async save() {
    if (this.busy) return;
    this.busy = true;
    this.button.disabled = true;
    try {
      const seconds = Number(this.amount.value);
      const result = await capture("save", seconds > 0 ? { seconds } : {});
      if (result?.ok) this.setStatus(`Captured ${formatDuration(result.seconds)} on a new track`, "");
      else this.setStatus(result?.error || "Couldn't save the capture", "is-error");
    } finally {
      this.busy = false;
      this.button.disabled = this.element.classList.contains("is-off");
    }
  }
}
