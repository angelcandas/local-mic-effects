// MIDI learn on the Live screen. The "MIDI" chip in the top bar switches learn mode on:
// every control that can be mapped is outlined, a click picks one, and the next pedal,
// knob, key or program change that arrives is mapped to it (src/MidiMap.*). Right-click
// a mapped control to remove its mapping. Mappings are kept by the app, not the preset.
//
// Controls opt in with data-learn ("param:<id>" or "action:<name>") and an optional
// data-learn-label; markLearnable() sets both.
import * as Juce from "./juce/index.js";
import { html } from "./widgets.js";

const midi = Juce.getNativeFunction("midi");

export function markLearnable(element, target, label) {
  element.dataset.learn = target;
  if (label) element.dataset.learnLabel = label;
  return element;
}

// "Echo · Time": the control's name, after its effect card's when it's on one.
function labelOf(element) {
  const own = element.dataset.learnLabel || element.getAttribute("aria-label") || element.textContent.trim() || element.dataset.learn;
  const card = element.closest(".card")?.querySelector("header h2")?.textContent;
  return card && !own.startsWith(card) ? `${card} · ${own}` : own;
}

export class MidiLearn {
  constructor(button) {
    this.button = button;
    this.mappings = [];
    this.active = false;
    this.target = null;
    this.lastCount = 0;

    this.banner = html("div", "learn-banner");
    this.banner.hidden = true;
    this.bannerText = html("span", "learn-text");
    this.list = html("div", "learn-list");
    const clear = html("button", "tool learn-clear", "Clear all");
    clear.type = "button";
    clear.title = "Remove every MIDI mapping";
    clear.addEventListener("click", () => {
      // Two clicks, like the looper's Clear.
      if (!this.mappings.length) return;
      if (!clear.classList.contains("is-armed")) {
        clear.classList.add("is-armed");
        clear.textContent = "Sure?";
        setTimeout(() => { clear.classList.remove("is-armed"); clear.textContent = "Clear all"; }, 2500);
        return;
      }
      clear.classList.remove("is-armed");
      clear.textContent = "Clear all";
      midi("clear").then((state) => this.apply(state));
    });
    const done = html("button", "tool is-accent learn-done", "Done");
    done.type = "button";
    done.addEventListener("click", () => this.setActive(false));
    this.banner.append(html("strong", "", "MIDI learn"), this.bannerText, this.list, clear, done);
    document.body.append(this.banner);

    button.addEventListener("click", () => this.setActive(!this.active));

    // In learn mode a click picks the control instead of using it.
    for (const type of ["pointerdown", "click", "dblclick"])
      document.addEventListener(type, (event) => this.onPointer(event), true);
    document.addEventListener("contextmenu", (event) => this.onContextMenu(event), true);
    document.addEventListener("keydown", (event) => {
      if (this.active && event.key === "Escape") { event.stopPropagation(); this.target ? this.cancel() : this.setActive(false); }
    }, true);

    window.__JUCE__.backend.addEventListener("midiLearned", (event) => {
      this.target = null;
      this.apply(event?.midi);
      this.say(`${event?.name ?? "MIDI"} now controls ${this.labelFor(event?.target)}. Pick another control, or Done.`);
      const element = document.querySelector(`[data-learn="${CSS.escape(event?.target ?? "")}"]`);
      element?.classList.add("is-learned");
      setTimeout(() => element?.classList.remove("is-learned"), 900);
    });

    midi("list").then((state) => this.apply(state));
  }

  setActive(on) {
    this.active = on;
    document.body.classList.toggle("is-learning", on);
    this.button.setAttribute("aria-pressed", String(on));
    this.banner.hidden = !on;
    if (!on && this.target) this.cancel();
    if (on) this.say("Click a knob, switch or button, then move a pedal, knob or key. Right-click a mapped control to unmap it.");
    this.render();
  }

  cancel() {
    this.target = null;
    midi("cancel").then((state) => this.apply(state));
    this.say("Click a knob, switch or button, then move a pedal, knob or key.");
  }

  say(text) { this.bannerText.textContent = text; }

  labelFor(target) {
    const element = target && document.querySelector(`[data-learn="${CSS.escape(target)}"]`);
    return element ? labelOf(element) : target ?? "";
  }

  onPointer(event) {
    if (!this.active || event.target.closest?.(".learn-banner, .midi-button, .loop-toggle, .keys-button")) return;
    const element = event.target.closest?.("[data-learn]");
    // Nothing else on the page reacts while learning.
    event.preventDefault();
    event.stopPropagation();
    if (!element || event.type !== "pointerdown" || event.button !== 0) return;
    this.target = element.dataset.learn;
    midi("learn", { target: this.target }).then((state) => this.apply(state));
    this.say(`Move a pedal, knob or key for ${labelOf(element)}… (Esc to stop)`);
    this.render();
  }

  onContextMenu(event) {
    if (!this.active) return;
    event.preventDefault();
    event.stopPropagation();
    const element = event.target.closest?.("[data-learn]");
    if (element && this.mappings.some((m) => m.target === element.dataset.learn))
      midi("remove", { target: element.dataset.learn }).then((state) => {
        this.apply(state);
        this.say(`${labelOf(element)} is no longer mapped.`);
      });
  }

  apply(state) {
    if (!state || typeof state !== "object") return;
    this.mappings = Array.isArray(state.mappings) ? state.mappings : [];
    this.render();
  }

  // From the "meters" event: flashes the chip when MIDI arrives.
  update(status) {
    if (!status) return;
    if (status.count !== this.lastCount) {
      this.lastCount = status.count;
      this.button.classList.remove("is-receiving");
      void this.button.offsetWidth;
      this.button.classList.add("is-receiving");
      this.button.title = `MIDI learn · last: ${status.last}`;
    }
  }

  render() {
    const byTarget = new Map(this.mappings.map((m) => [m.target, m.name]));
    for (const element of document.querySelectorAll("[data-learn]")) {
      const name = byTarget.get(element.dataset.learn);
      element.classList.toggle("is-mapped", Boolean(name));
      element.classList.toggle("is-learn-target", this.target === element.dataset.learn);
      if (name) element.dataset.learnName = name;
      else delete element.dataset.learnName;
    }
    this.button.classList.toggle("has-mappings", this.mappings.length > 0);
    this.list.replaceChildren(...this.mappings.map((m) => {
      const chip = html("span", "learn-chip");
      chip.append(html("b", "", m.name), html("span", "", this.labelFor(m.target)));
      const remove = html("button", "learn-remove", "×");
      remove.type = "button";
      remove.title = "Remove this mapping";
      remove.addEventListener("click", () => midi("remove", { target: m.target }).then((state) => this.apply(state)));
      chip.append(remove);
      return chip;
    }));
  }
}
