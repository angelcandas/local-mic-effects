// Sound check (Live): sing for 10-20 seconds and the effect chain is set for this voice,
// mic and room. The panel opens from the "Sound check" button under the input meter.
//
//   pick a style -> Start -> sing (a level bar and the voice heard so far) -> Done (or wait)
//   -> what changed and why, the input gain advice, Before / After, Revert, Save as scene, Keep.
// Or "Keep this sound, fit the levels": the chain you loaded (a vocal chain, a sound of yours)
// stays as it is, and only its input gain, gate and de-esser thresholds and the denoiser's
// room are set for this voice.
//
// Engine: src/SoundCheck.* (native function "soundCheck", event "soundCheck").
import * as Juce from "./juce/index.js";
import { html } from "./widgets.js";

const soundCheck = Juce.getNativeFunction("soundCheck");
const liveHistory = Juce.getNativeFunction("liveHistory");
const LENGTHS = [[10, "10 s"], [15, "15 s"], [20, "20 s"]];

const row = (className, ...children) => { const element = html("div", className); element.append(...children); return element; };

export class SoundCheckPanel {
  constructor(button) {
    this.button = button;
    this.state = { phase: "idle", styles: [], style: 0 };
    this.seconds = 15;

    this.element = html("div", "soundcheck");
    this.element.hidden = true;
    this.element.setAttribute("role", "dialog");
    this.element.setAttribute("aria-label", "Sound check");
    this.panel = html("div", "soundcheck-panel");
    const close = html("button", "soundcheck-close", "×");
    close.type = "button";
    close.title = "Close (Esc)";
    close.addEventListener("click", () => this.close());
    this.title = html("h2", "", "Sound check");
    this.body = html("div", "soundcheck-body");
    this.panel.append(close, this.title, this.body);
    this.element.append(this.panel);
    document.body.append(this.element);

    this.element.addEventListener("pointerdown", (event) => { if (event.target === this.element) this.close(); });
    document.addEventListener("keydown", (event) => {
      if (this.element.hidden) return;
      if (event.key === "Escape") { event.preventDefault(); event.stopPropagation(); this.close(); }
    }, true);

    button.addEventListener("click", () => this.open());
    window.__JUCE__.backend.addEventListener("soundCheck", (state) => this.update(state));
    this.send("state");
  }

  send(action, args) {
    return soundCheck(action, args ?? {}).then((state) => this.update(state)).catch(() => {});
  }

  open() {
    liveHistory("hold", "Sound check"); // every try, compare and revert in here is one undo step
    this.element.hidden = false;
    this.render();
  }

  // Closing while listening cancels; closing with a result keeps it.
  close() {
    const done = this.state.phase === "listening" ? this.send("cancel")
               : this.state.phase === "done" ? this.send("keep") : Promise.resolve();
    this.element.hidden = true;
    done.then(() => liveHistory("release"));
  }

  update(state) {
    if (!state || typeof state !== "object") return;
    const phaseChanged = state.phase !== this.state.phase || state.error !== this.state.error;
    this.state = state;
    this.button.classList.toggle("is-busy", state.phase === "listening" || state.phase === "analysing");
    this.button.textContent = state.phase === "listening" ? `Listening… ${Math.max(0, Math.ceil(state.target - state.seconds))}`
                            : state.phase === "analysing" ? "Listening…" : "Sound check";
    if (state.phase !== "idle" && this.element.hidden) this.open();
    if (this.element.hidden) return;
    if (phaseChanged || state.phase !== "listening" || !this.meter) this.render();
    else this.updateListening();
  }

  render() {
    const { phase } = this.state;
    this.body.replaceChildren();
    this.meter = null;
    if (phase === "listening") this.renderListening();
    else if (phase === "analysing") this.body.append(html("p", "soundcheck-lead", "Listening to what you sang…"));
    else if (phase === "done") this.renderResult();
    else this.renderStart();
  }

  renderStart() {
    const { state } = this;
    this.body.append(html("p", "soundcheck-lead",
      "Sing the way you'll perform for 10 to 20 seconds, with a few short pauses, and the effect chain is set for your voice, your mic and this room."));
    if (state.scene) this.body.append(html("p", "soundcheck-ok", `Saved as the scene “${state.scene}”.`));
    if (state.error) this.body.append(html("p", "soundcheck-error", state.error));

    const options = html("div", "soundcheck-options");
    const style = document.createElement("select");
    style.className = "dropdown";
    (state.styles ?? []).forEach((name, i) => style.append(Object.assign(document.createElement("option"), { value: i, textContent: name })));
    style.value = String(state.style ?? 0);
    const styleLabel = html("label", "soundcheck-field");
    styleLabel.append(html("span", "", "Style"), style);
    styleLabel.title = "Shapes the EQ and how much reverb the voice gets";

    const length = document.createElement("select");
    length.className = "dropdown";
    for (const [seconds, text] of LENGTHS) length.append(Object.assign(document.createElement("option"), { value: seconds, textContent: text }));
    length.value = String(this.seconds);
    length.addEventListener("change", () => { this.seconds = Number(length.value); });
    const lengthLabel = html("label", "soundcheck-field");
    lengthLabel.append(html("span", "", "Listen for"), length);
    options.append(styleLabel, lengthLabel);

    const tips = html("ul", "soundcheck-tips");
    for (const tip of ["Stand where you'll sing, at your usual distance from the mic.",
                       "Sing loud and soft parts. The backing track can play, on headphones.",
                       "Keep the other effects as you like them: only the clean-up effects and the reverb change."])
      tips.append(html("li", "", tip));

    const start = html("button", "tool is-accent soundcheck-start", "Start");
    start.type = "button";
    start.addEventListener("click", () => this.send("start", { seconds: this.seconds, style: Number(style.value) }));
    const fit = html("button", "tool soundcheck-fit", "Keep this sound, fit the levels");
    fit.type = "button";
    fit.title = "Keep the effects you have loaded (a vocal chain, a sound of yours) and set only the input level, "
              + "the gate and de-esser thresholds and the denoiser's room for your voice";
    fit.addEventListener("click", () => this.send("start", { seconds: this.seconds, style: Number(style.value), fit: true }));
    this.body.append(options, tips, row("soundcheck-actions", fit, start));
    start.focus();
  }

  renderListening() {
    this.body.append(html("p", "soundcheck-lead", "Sing now…"));
    this.meter = html("div", "soundcheck-meter");
    this.meterFill = html("div", "soundcheck-meter-fill");
    this.meter.append(this.meterFill);
    this.progress = html("div", "soundcheck-progress");
    this.progressFill = html("div", "soundcheck-progress-fill");
    this.progress.append(this.progressFill);
    this.info = html("p", "soundcheck-info");

    const done = html("button", "tool is-accent", "Done");
    done.type = "button";
    done.title = "Stop listening now and set the chain";
    done.addEventListener("click", () => this.send("finish"));
    this.doneButton = done;
    const cancel = html("button", "tool", "Cancel");
    cancel.type = "button";
    cancel.addEventListener("click", () => this.send("cancel"));
    this.body.append(this.meter, this.progress, this.info, row("soundcheck-actions", cancel, done));
    this.updateListening();
  }

  updateListening() {
    const { state } = this;
    const level = Math.max(0, Math.min(1, ((state.level ?? -100) + 60) / 60));
    this.meterFill.style.width = `${(level * 100).toFixed(1)}%`;
    this.meterFill.classList.toggle("is-hot", (state.level ?? -100) > -3);
    this.progressFill.style.width = `${Math.min(100, (100 * state.seconds) / Math.max(1, state.target)).toFixed(1)}%`;
    const voice = state.voiceSeconds ?? 0;
    this.info.textContent = voice < state.minVoiceSeconds
      ? `Heard ${voice.toFixed(1)} s of voice: keep singing (at least ${state.minVoiceSeconds} s).`
      : `Heard ${voice.toFixed(1)} s of voice. ${Math.max(0, Math.ceil(state.target - state.seconds))} s left, or press Done.`;
    this.doneButton.disabled = !state.canFinish;
  }

  renderResult() {
    const { state } = this;
    const sung = `${Number(state.sung ?? 0).toFixed(1)} s of singing`;
    this.body.append(html("p", "soundcheck-lead", state.fit
      ? `The levels are fitted to your voice, from ${sung}; the sound itself is as you loaded it. Listen, then keep it or go back.`
      : `Set for your voice, from ${sung}. Listen, then keep it or go back.`));

    if (state.advice) {
      const advice = html("p", `soundcheck-advice${/good/.test(state.advice) ? " is-good" : ""}`);
      advice.append(html("strong", "", "Input  "), document.createTextNode(state.advice));
      this.body.append(advice);
    }

    const list = html("ul", "soundcheck-changes");
    for (const change of state.changes ?? []) {
      const item = html("li");
      item.append(html("span", "soundcheck-module", change.module), html("span", "", change.text));
      list.append(item);
    }
    if (!(state.changes ?? []).length) list.append(html("li", "", "Nothing needed changing."));
    this.body.append(list);

    if ((state.notes ?? []).length) {
      const notes = html("ul", "soundcheck-notes");
      for (const note of state.notes) notes.append(html("li", "", note));
      this.body.append(notes);
    }
    if (state.error) this.body.append(html("p", "soundcheck-error", state.error));

    const compare = html("div", "segmented soundcheck-compare");
    compare.title = "Switch between the sound from before and the new one";
    for (const [value, text] of [["before", "Before"], ["after", "After"]]) {
      const b = html("button", value === state.comparing ? "is-selected" : "", text);
      b.type = "button";
      b.addEventListener("click", () => this.send("compare", { after: value === "after" }));
      compare.append(b);
    }

    const revert = html("button", "tool", "Revert");
    revert.type = "button";
    revert.title = "Go back to the sound from before";
    revert.addEventListener("click", () => this.send("revert"));
    const scene = html("button", "tool", "Save as scene");
    scene.type = "button";
    scene.title = "Keep this sound and save it as a new scene";
    scene.addEventListener("click", () => this.send("saveScene", { name: "Sound check" }));
    const keep = html("button", "tool is-accent", "Keep");
    keep.type = "button";
    keep.addEventListener("click", () => this.send("keep").then(() => { this.element.hidden = true; liveHistory("release"); }));
    this.body.append(row("soundcheck-actions", compare, html("span", "soundcheck-spacer"), revert, scene, keep));
  }
}
