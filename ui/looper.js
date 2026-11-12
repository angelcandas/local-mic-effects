// The Live mode looper panel: up to four stacked layers of your processed voice.
// The engine is src/Looper.*; the page talks to it with the "looper" native function and
// reads its state from the "meters" event (status.looper).
import * as Juce from "./juce/index.js";
import { clamp, html, svg } from "./widgets.js";
import { markLearnable } from "./midilearn.js";

const looper = Juce.getNativeFunction("looper");

// The stage keys the looper adds (shown in the "?" overlay).
export const LOOPER_KEYS = [["L", "Loop: record, overdub, end the overdub"], ["K", "Stop / play the loop"],
                            ["U / Shift+U", "Undo / redo the last loop layer"]];

const MAIN_LABELS = {
  empty: ["Record loop", "Start recording the first layer (L)"],
  recording: ["Close loop", "Close the loop here; it plays back at once (L)"],
  closing: ["Closing…", "Recording to the end of the bar, then the loop plays"],
  playing: ["Overdub", "Record another layer on top (L)"],
  overdubbing: ["End overdub", "Stop recording this layer; the loop plays on (L)"],
  stopped: ["Play loop", "Play the loop from the top (L)"],
};

const isTyping = (event) => event.target.closest?.("input, textarea, select, [contenteditable=true]") != null;

export class LoopPanel {
  constructor(element, toggleButton) {
    this.element = element;
    this.toggleButton = toggleButton;
    this.status = { state: "empty", layers: 0, redo: 0, maxLayers: 4, length: 0, position: 0, closing: 0,
                    bpm: 120, beatsPerBar: 4, locked: false, volume: 1, gains: [1, 1, 1, 1], mutes: [false, false, false, false] };
    this.overviews = [];
    this.taps = [];
    this.build();

    toggleButton.addEventListener("click", () => this.show(this.element.hidden));
    Juce.getNativeFunction("uiPref")("looperShown").then((shown) => this.show(shown === "1", false)).catch(() => {});
    looper("status").then((status) => this.update(status));

    document.addEventListener("keydown", (event) => this.onKey(event));
  }

  show(on, remember = true) {
    this.element.hidden = !on;
    this.toggleButton.setAttribute("aria-pressed", String(on));
    if (remember) Juce.getNativeFunction("uiPref")("looperShown", on ? "1" : "0");
    if (on) this.drawLayers();
  }

  send(action, args) {
    return looper(action, args).then((status) => this.update(status));
  }

  build() {
    const controls = html("div", "loop-controls");
    this.main = html("button", "loop-main");
    this.main.type = "button";
    this.main.append(html("span", "loop-main-dot"), (this.mainLabel = html("span", "loop-main-label")));
    this.main.addEventListener("click", () => this.send("main"));

    const tool = (className, text, title, onClick) => {
      const button = html("button", `tool ${className}`, text);
      button.type = "button";
      button.title = title;
      button.addEventListener("click", onClick);
      return button;
    };
    this.stop = tool("icon loop-stop", "■", "Stop the loop (K)", () => this.send(this.status.state === "stopped" ? "play" : "stop"));
    this.undo = tool("icon loop-undo", "↶", "Undo the last layer (U)", () => this.send("undo"));
    this.redo = tool("icon loop-redo", "↷", "Redo the layer (Shift+U)", () => this.send("redo"));
    this.clear = tool("loop-clear is-danger", "Clear", "Throw the whole loop away", () => this.onClear());
    controls.append(this.main, this.stop, this.undo, this.redo, this.clear);
    markLearnable(this.main, "action:looper.main", "Loop (record / overdub / play)");
    markLearnable(this.stop, "action:looper.stop", "Stop loop");
    markLearnable(this.undo, "action:looper.undo", "Undo loop layer");
    markLearnable(this.redo, "action:looper.redo", "Redo loop layer");
    markLearnable(this.clear, "action:looper.clear", "Clear loop");

    // The loop's progress: a ring with the time (or bar and beat) inside, beat dots under it.
    const clock = html("div", "loop-clock");
    const ring = svg("svg", { viewBox: "0 0 40 40", class: "loop-ring", "aria-hidden": "true" });
    ring.append(svg("circle", { class: "loop-ring-track", cx: 20, cy: 20, r: 17 }),
                (this.ringArc = svg("circle", { class: "loop-ring-arc", cx: 20, cy: 20, r: 17, pathLength: 100 })));
    this.clockText = html("span", "loop-clock-text");
    this.clockSub = html("span", "loop-clock-sub");
    const readout = html("div", "loop-clock-readout");
    readout.append(this.clockText, this.clockSub);
    this.beats = html("div", "loop-beats");
    clock.append(ring, readout);
    const clockColumn = html("div", "loop-clock-column");
    clockColumn.append(clock, this.beats);

    // Four layer lanes: waveform, mute, level.
    this.lanes = html("div", "loop-layers");
    this.layerRows = [];
    for (let i = 0; i < 4; i++) {
      const row = html("div", "loop-layer");
      const number = html("span", "loop-layer-number", String(i + 1));
      const canvas = html("canvas", "loop-layer-wave");
      const mute = html("button", "loop-layer-mute", "M");
      mute.type = "button";
      mute.title = `Mute layer ${i + 1}`;
      mute.addEventListener("click", () => this.send("layer", { layer: i, muted: !this.status.mutes[i] }));
      const gain = Object.assign(document.createElement("input"), { type: "range", min: 0, max: 150, step: 1, value: 100, className: "loop-layer-gain" });
      gain.title = `Layer ${i + 1} level`;
      gain.addEventListener("input", () => looper("layer", { layer: i, gain: Number(gain.value) / 100 }));
      gain.addEventListener("dblclick", () => { gain.value = 100; looper("layer", { layer: i, gain: 1 }); });
      row.append(number, canvas, mute, gain);
      this.lanes.append(row);
      this.layerRows.push({ row, canvas, mute, gain });
    }

    // Tempo: free length, or whole bars at a tempo you type or tap.
    const tempo = html("div", "loop-tempo");
    this.lock = html("div", "segmented loop-lock");
    this.lock.title = "Free: the loop is exactly as long as you record. Bars: its length snaps to whole bars at the tempo.";
    for (const [value, label] of [[false, "Free"], [true, "Bars"]]) {
      const button = html("button", "", label);
      button.type = "button";
      button.dataset.locked = String(value);
      button.addEventListener("click", () => this.send("settings", { locked: value }));
      this.lock.append(button);
    }
    this.bpm = Object.assign(document.createElement("input"), { type: "number", min: 30, max: 300, step: 0.5, className: "loop-bpm" });
    this.bpm.title = "Tempo (beats per minute)";
    this.bpm.addEventListener("change", () => this.send("settings", { bpm: clamp(Number(this.bpm.value) || 120, 30, 300) }));
    const bpmLabel = html("label", "loop-field");
    bpmLabel.append(this.bpm, html("span", "", "BPM"));
    this.tap = html("button", "tool loop-tap", "Tap");
    this.tap.type = "button";
    this.tap.title = "Tap the beat a few times to set the tempo";
    this.tap.addEventListener("click", () => this.onTap());
    this.meter = document.createElement("select");
    this.meter.className = "dropdown loop-meter";
    this.meter.title = "Beats in a bar";
    for (const beats of [2, 3, 4, 5, 6, 7, 8]) this.meter.append(Object.assign(document.createElement("option"), { value: beats, textContent: `${beats}/4` }));
    this.meter.addEventListener("change", () => this.send("settings", { beatsPerBar: Number(this.meter.value) }));
    this.volume = Object.assign(document.createElement("input"), { type: "range", min: 0, max: 150, step: 1, value: 100, className: "loop-volume" });
    this.volume.title = "Loop volume (double-click: 100%)";
    this.volume.addEventListener("input", () => looper("settings", { volume: Number(this.volume.value) / 100 }));
    this.volume.addEventListener("dblclick", () => { this.volume.value = 100; looper("settings", { volume: 1 }); });
    const volumeLabel = html("label", "loop-field loop-volume-field");
    volumeLabel.append(html("span", "", "Loop"), this.volume);
    tempo.append(this.lock, bpmLabel, this.tap, this.meter, volumeLabel);

    // Keep the loop: WAV files in the recordings folder, or tracks in the DAW project.
    const actions = html("div", "loop-actions");
    this.saveButton = tool("loop-save", "Save WAV", "Save each layer as a WAV file in the recordings folder", () => this.onSave(false));
    this.repeats = document.createElement("select");
    this.repeats.className = "dropdown loop-repeats";
    this.repeats.title = "How many times each layer repeats on its DAW track";
    for (const n of [1, 2, 4, 8, 16]) this.repeats.append(Object.assign(document.createElement("option"), { value: n, textContent: `×${n}` }));
    this.repeats.value = "8";
    this.sendButton = tool("loop-send is-accent", "Send to DAW", "Put each layer on its own track in the DAW project, from bar 1 (with the loop's tempo in Bars mode)", () => this.onSave(true));
    this.openDaw = tool("loop-open-daw", "Open DAW", "Switch to the DAW to see the loop tracks", () => Juce.getNativeFunction("daw")("mode", { daw: true }));
    this.openDaw.hidden = true;
    actions.append(this.saveButton, this.repeats, this.sendButton, this.openDaw);

    const head = html("div", "loop-head");
    head.append(html("h2", "", "Looper"), (this.hint = html("span", "loop-hint")), actions);

    const body = html("div", "loop-body");
    body.append(controls, clockColumn, this.lanes, tempo);
    this.element.append(head, body);
  }

  onSave(toDaw) {
    const busy = toDaw ? this.sendButton : this.saveButton;
    busy.disabled = true;
    looper(toDaw ? "sendToDaw" : "save", { repeats: Number(this.repeats.value) }).then((result) => {
      busy.disabled = false;
      if (!result?.ok) { this.flash(result?.error || "Couldn't save the loop."); return; }
      const count = result.files?.length ?? 0;
      const layers = `${count} layer${count === 1 ? "" : "s"}`;
      this.flash(toDaw ? `Sent ${layers} to the DAW project as tracks Loop 1–${count}.` : `Saved ${layers} in ${result.folder}.`);
      this.openDaw.hidden = !toDaw;
    });
  }

  // A message in place of the hint for a few seconds.
  flash(text) {
    this.message = text;
    this.hint.textContent = text;
    clearTimeout(this.messageTimer);
    this.messageTimer = setTimeout(() => { this.message = ""; this.openDaw.hidden = true; this.renderClock(); }, 6000);
  }

  onClear() {
    // Two clicks, so a stray click on stage can't throw a loop away.
    if (!this.clearArmed) {
      this.clearArmed = true;
      this.clear.textContent = "Sure?";
      this.clear.classList.add("is-armed");
      clearTimeout(this.clearTimer);
      this.clearTimer = setTimeout(() => this.disarmClear(), 2500);
      return;
    }
    this.disarmClear();
    this.send("clear");
  }

  disarmClear() {
    this.clearArmed = false;
    this.clear.textContent = "Clear";
    this.clear.classList.remove("is-armed");
  }

  onTap() {
    const now = performance.now();
    if (this.taps.length && now - this.taps[this.taps.length - 1] > 2000) this.taps = [];
    this.taps = [...this.taps, now].slice(-8);
    this.tap.classList.remove("is-tapped");
    void this.tap.offsetWidth;
    this.tap.classList.add("is-tapped");
    if (this.taps.length < 3) return;
    const gaps = this.taps.slice(1).map((t, i) => t - this.taps[i]);
    const bpm = Math.round(clamp(60000 / (gaps.reduce((a, b) => a + b, 0) / gaps.length), 30, 300) * 2) / 2;
    this.send("settings", { bpm, locked: true });
  }

  onKey(event) {
    if (document.body.classList.contains("is-daw") || isTyping(event) || event.metaKey || event.ctrlKey || event.altKey || event.repeat) return;
    const key = event.key.toLowerCase();
    if (key === "l") { event.preventDefault(); this.send("main"); }
    else if (key === "k") { event.preventDefault(); this.send(this.status.state === "stopped" ? "play" : "stop"); }
    else if (key === "u") { event.preventDefault(); this.send(event.shiftKey ? "redo" : "undo"); }
    else return;
    // A stage key shows the panel, so you see what it did.
    if (this.element.hidden) this.show(true);
  }

  update(status) {
    if (!status || typeof status !== "object") return;
    const layersChanged = status.layers !== this.status.layers || status.state !== this.status.state;
    this.status = { ...this.status, ...status };
    if (Array.isArray(status.overviews)) this.overviews = status.overviews;
    else if (layersChanged && status.layers < this.overviews.length) this.overviews = this.overviews.slice(0, status.layers);

    const s = this.status;
    this.element.dataset.state = s.state;
    this.toggleButton.dataset.state = s.state;
    const full = s.state === "playing" && s.layers >= s.maxLayers;
    const [label, title] = full ? ["4 of 4 layers", "Every layer is used: undo one or clear the loop to overdub again"] : MAIN_LABELS[s.state] ?? MAIN_LABELS.empty;
    this.mainLabel.textContent = label;
    this.main.title = title;
    this.main.disabled = full || s.state === "closing";

    const stopped = s.state === "stopped";
    this.stop.textContent = stopped ? "▶" : "■";
    this.stop.title = stopped ? "Play the loop from the top (K)" : "Stop the loop (K)";
    this.stop.disabled = s.state === "empty";
    this.undo.disabled = !(["recording", "closing", "overdubbing"].includes(s.state) || s.layers > 1);
    this.redo.disabled = !(s.redo > 0 && (s.state === "playing" || stopped));
    this.clear.disabled = s.state === "empty";
    if (this.clear.disabled) this.disarmClear();
    const closed = s.layers > 0 && (s.state === "playing" || s.state === "stopped");
    this.saveButton.disabled = !closed;
    this.sendButton.disabled = !closed;
    this.repeats.disabled = !closed;
    const dawAvailable = !document.querySelector(".mode-switch")?.hidden;
    this.sendButton.hidden = !dawAvailable;
    this.repeats.hidden = !dawAvailable;

    for (const button of this.lock.children) button.classList.toggle("is-selected", button.dataset.locked === String(s.locked));
    if (document.activeElement !== this.bpm) this.bpm.value = String(s.bpm);
    this.bpm.disabled = !s.locked;
    this.meter.value = String(s.beatsPerBar);
    this.meter.disabled = !s.locked;
    if (document.activeElement !== this.volume) this.volume.value = String(Math.round(s.volume * 100));

    this.layerRows.forEach(({ row, mute, gain }, i) => {
      const used = i < s.layers;
      row.classList.toggle("is-used", used);
      row.classList.toggle("is-recording", used && i === s.layers - 1 && (s.state === "overdubbing" || s.state === "recording" || s.state === "closing"));
      row.classList.toggle("is-redo", !used && i < s.layers + s.redo);
      mute.setAttribute("aria-pressed", String(!!s.mutes[i]));
      mute.disabled = !used;
      gain.disabled = !used;
      if (document.activeElement !== gain) gain.value = String(Math.round((s.gains[i] ?? 1) * 100));
    });

    this.renderClock();
    if (!this.element.hidden) this.drawLayers();
  }

  renderClock() {
    const s = this.status;
    const recordingFirst = s.state === "recording" || s.state === "closing";
    const beat = 60 / s.bpm;
    const bar = beat * s.beatsPerBar;
    let progress = 0;

    if (s.state === "empty") {
      this.clockText.textContent = "—";
      this.clockSub.textContent = s.locked ? `${s.bpm} BPM` : "free";
      this.hint.textContent = s.locked ? `The loop will be whole bars of ${s.beatsPerBar}/4 at ${s.bpm} BPM.`
                                       : "Press Record loop (or L), sing, and press again to close the loop.";
    } else if (recordingFirst) {
      progress = s.closing > 0 ? s.position / s.closing : (s.locked ? (s.position % bar) / bar : (s.position % 4) / 4);
      this.clockText.textContent = `${s.position.toFixed(1)} s`;
      this.clockSub.textContent = s.locked ? `bar ${Math.floor(s.position / bar) + 1}` : "recording";
      this.hint.textContent = s.state === "closing" ? "Closing the loop at the bar line…" : `Recording the first layer (up to ${s.maxSeconds} s).`;
    } else {
      progress = s.length > 0 ? s.position / s.length : 0;
      const bars = s.length / bar;
      const wholeBars = s.locked && Math.abs(bars - Math.round(bars)) < 0.01;
      this.clockText.textContent = `${s.position.toFixed(1)} s`;
      this.clockSub.textContent = wholeBars ? `${Math.round(bars)} bar${Math.round(bars) === 1 ? "" : "s"}` : `of ${s.length.toFixed(1)} s`;
      this.hint.textContent = s.state === "overdubbing" ? `Overdubbing layer ${s.layers}. Each pass adds to it.`
                            : s.state === "stopped" ? "Stopped. Play starts the loop from the top."
                            : `${s.layers} of ${s.maxLayers} layers.`;
    }

    if (this.message) this.hint.textContent = this.message;
    this.ringArc.style.strokeDasharray = `${clamp(progress) * 100} 100`;

    // One dot per beat of the bar, the current one lit (bars mode only).
    const count = s.locked ? s.beatsPerBar : 0;
    if (this.beats.childElementCount !== count) this.beats.replaceChildren(...Array.from({ length: count }, () => html("i")));
    const current = s.state === "empty" || s.state === "stopped" ? -1 : Math.floor((s.position % bar) / beat);
    [...this.beats.children].forEach((dot, i) => dot.classList.toggle("is-on", i === current));
  }

  drawLayers() {
    const s = this.status;
    const recordingFirst = s.state === "recording" || s.state === "closing";
    const playhead = recordingFirst ? 1 : s.length > 0 ? s.position / s.length : 0;
    const style = getComputedStyle(this.element);
    const colours = [style.getPropertyValue("--loop-1"), style.getPropertyValue("--loop-2"), style.getPropertyValue("--loop-3"), style.getPropertyValue("--loop-4")];

    this.layerRows.forEach(({ canvas }, i) => {
      const width = canvas.clientWidth, height = canvas.clientHeight;
      if (!width || !height) return;
      const scale = window.devicePixelRatio || 1;
      if (canvas.width !== Math.round(width * scale)) canvas.width = Math.round(width * scale);
      if (canvas.height !== Math.round(height * scale)) canvas.height = Math.round(height * scale);
      const g = canvas.getContext("2d");
      g.setTransform(scale, 0, 0, scale, 0, 0);
      g.clearRect(0, 0, width, height);

      const peaks = i < s.layers ? this.overviews[i] : null;
      if (Array.isArray(peaks) && peaks.length) {
        g.globalAlpha = s.mutes[i] ? 0.3 : 0.9;
        g.fillStyle = colours[i] || "#8c7bff";
        const step = width / peaks.length;
        for (let b = 0; b < peaks.length; b++) {
          const h = Math.max(1, Math.sqrt(clamp(peaks[b])) * (height - 2));
          g.fillRect(b * step, (height - h) / 2, Math.max(1, step - 0.5), h);
        }
        g.globalAlpha = 1;
      }

      if (i < s.layers && s.state !== "empty") {
        g.fillStyle = "rgba(233, 236, 244, 0.85)";
        g.fillRect(Math.min(width - 1.5, playhead * width), 0, 1.5, height);
      }
    });
  }
}
