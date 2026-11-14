// Macros: eight knobs for one chain (a DAW track, return, group or the master). A macro
// turns any number of its effects' values at once, each between its own min and max.
//
// Map mode: press Map, pick a macro, then click the controls it should turn (click a mapped
// one again to take it off). Each mapped control shows a dot in its macro's colour; the
// ranges are listed under the knobs, where min and max can be typed (min above max turns
// the value the other way).
//
// The engine side is in src/daw/Edit (MACROS) and the bridge's setMacro / mapMacro /
// unmapMacro / nameMacro commands.

import { MODULES, MODULE_KEYS } from "./modules.js";
import { html } from "./widgets.js";

const controlLabel = (param) => MODULES.flatMap((m) => m.controls ?? []).find((c) => c.param === param)?.label ?? param;

export const MACRO_COLOURS = ["#5ad1ff", "#ffb44d", "#b98cff", "#6be38f", "#ff7a9c", "#ffe066", "#7aa8ff", "#ff9a5a"];

export class MacroPanel {
  // kit: { daw, ValueKnob, parameterInfo (), toast, button }
  // chain (): the element holding this owner's effect cards (controls carry data-param,
  // cards data-insert).
  constructor(owner, kit, chain) {
    this.owner = owner;
    this.kit = kit;
    this.chain = chain;
    this.selected = 0;
    this.mapping = false;
    this.element = html("div", "macro-panel");

    const head = html("div", "macro-head");
    this.mapButton = kit.button("tool macro-map", "Map", "Map: pick a macro, then click the controls it turns", () => this.setMapping(!this.mapping));
    head.append(html("span", "macro-title", "Macros"), this.mapButton);

    this.grid = html("div", "macro-grid");
    this.knobs = [];
    for (let index = 0; index < 8; ++index) {
      const cell = html("div", "macro-cell");
      cell.style.setProperty("--macro", MACRO_COLOURS[index]);
      const knob = new kit.ValueKnob({
        label: `Macro ${index + 1}`, min: 0, max: 100, value: 0, defaultValue: 0,
        format: (v) => `${Math.round(v)}%`,
        onChange: (value, first) => kit.daw("setMacro", { track: this.owner.id, index, value: value / 100, continue: !first }),
      });
      const name = knob.element.querySelector(".name");
      name.title = "Double-click to name this macro";
      name.addEventListener("dblclick", (event) => { event.stopPropagation(); this.rename(index, name); });
      cell.addEventListener("pointerdown", () => { if (this.mapping) this.select(index); }, true);
      cell.append(knob.element);
      this.grid.append(cell);
      this.knobs.push({ knob, cell, name });
    }

    this.ranges = html("div", "macro-ranges");
    this.element.append(head, this.grid, this.ranges);

    // In Map mode a click on a control maps it (or unmaps it) instead of changing it.
    this.onChainClick = (event) => {
      if (!this.mapping) return;
      const control = event.target.closest("[data-param]");
      const card = event.target.closest(".insert[data-insert]");
      if (!control || !card || !this.chain().contains(card)) return;
      event.preventDefault();
      event.stopPropagation();
      this.toggleMap(Number(card.dataset.insert), control.dataset.param);
    };
    this.update(owner);
  }

  get macros() { return this.owner.macros ?? []; }

  update(owner) {
    this.owner = owner;
    this.macros.forEach((macro, index) => {
      const { knob, cell, name } = this.knobs[index];
      knob.update(macro.value * 100);
      const label = macro.name || `Macro ${index + 1}`;
      name.textContent = label;
      knob.element.setAttribute("aria-label", label);
      cell.classList.toggle("is-used", macro.maps.length > 0);
      cell.classList.toggle("is-selected", this.mapping && index === this.selected);
    });
    this.renderRanges();
    this.markControls();
  }

  // Each mapped control: a dot in its macro's colour.
  markControls() {
    const chain = this.chain();
    if (!chain) return;
    for (const element of chain.querySelectorAll(".is-macro-mapped")) {
      element.classList.remove("is-macro-mapped");
      element.style.removeProperty("--macro");
    }
    this.macros.forEach((macro, index) => {
      for (const map of macro.maps) {
        const control = chain.querySelector(`.insert[data-insert="${map.insert}"] [data-param="${map.param}"]`);
        if (!control) continue;
        control.classList.add("is-macro-mapped");
        control.style.setProperty("--macro", MACRO_COLOURS[index]);
      }
    });
  }

  // The selected macro's mappings (in Map mode), each with its min and max to type.
  renderRanges() {
    this.ranges.replaceChildren();
    this.ranges.hidden = !this.mapping;
    if (!this.mapping) return;
    const macro = this.macros[this.selected];
    const title = html("div", "macro-ranges-title", `${macro?.name || `Macro ${this.selected + 1}`}: click controls to map them`);
    this.ranges.append(title);
    for (const map of macro?.maps ?? []) {
      const insert = this.owner.inserts.find((x) => x.id === map.insert);
      const row = html("div", "macro-range");
      const field = (key) => {
        const input = Object.assign(html("input", "macro-range-value"), { type: "number", value: String(Math.round(map[key] * 100) / 100), step: "any" });
        input.title = key === "min" ? "Where the value is with the macro at 0%" : "Where the value is with the macro at 100%";
        input.addEventListener("keydown", (event) => { event.stopPropagation(); if (event.key === "Enter") input.blur(); });
        input.addEventListener("change", () => {
          const value = Number(input.value);
          if (!Number.isFinite(value)) return;
          this.kit.daw("mapMacro", { track: this.owner.id, index: this.selected, insert: map.insert, param: map.param,
                                     min: key === "min" ? value : map.min, max: key === "max" ? value : map.max });
        });
        return input;
      };
      const remove = this.kit.button("mini is-danger", "×", "Unmap", () =>
        this.kit.daw("unmapMacro", { track: this.owner.id, index: this.selected, insert: map.insert, param: map.param }));
      row.append(html("span", "macro-range-name", `${insert?.name || MODULES.find((m) => MODULE_KEYS[m.id] === insert?.type)?.name || insert?.type || "?"} · ${controlLabel(map.param)}`), field("min"), html("span", "", "→"), field("max"), remove);
      this.ranges.append(row);
    }
    if (!macro?.maps.length) this.ranges.append(html("div", "macro-range-none", "Nothing mapped yet."));
  }

  setMapping(on) {
    this.mapping = on;
    this.mapButton.setAttribute("aria-pressed", String(on));
    const chain = this.chain();
    chain?.classList.toggle("is-macro-mapping", on);
    if (on) chain?.addEventListener("click", this.onChainClick, true);
    else chain?.removeEventListener("click", this.onChainClick, true);
    // A drag on a control mustn't change it either while mapping.
    if (on) chain?.addEventListener("pointerdown", this.onChainClick, true);
    else chain?.removeEventListener("pointerdown", this.onChainClick, true);
    this.update(this.owner);
  }

  select(index) {
    this.selected = index;
    this.update(this.owner);
  }

  async toggleMap(insert, param) {
    const index = this.selected;
    const mapped = this.macros[index]?.maps.some((m) => m.insert === insert && m.param === param);
    if (mapped) return this.kit.daw("unmapMacro", { track: this.owner.id, index, insert, param });
    const info = this.kit.parameterInfo()[param];
    if (!info) return this.kit.toast("That control can't follow a macro.");
    // Its full range, so the macro sweeps all of it (type a narrower range below).
    const result = await this.kit.daw("mapMacro", { track: this.owner.id, index, insert, param, min: info.min ?? 0, max: info.max ?? 1 });
    if (result?.ok === false || result?.error) this.kit.toast(result.error ?? "Couldn't map that control.", "is-error");
  }

  rename(index, name) {
    const input = Object.assign(html("input", "macro-name-entry"), { value: this.macros[index]?.name ?? "", placeholder: `Macro ${index + 1}`, maxLength: 30 });
    let done = false;
    const finish = (commit) => {
      if (done) return;
      done = true;
      input.replaceWith(name);
      if (commit) this.kit.daw("nameMacro", { track: this.owner.id, index, name: input.value });
    };
    input.addEventListener("keydown", (event) => {
      event.stopPropagation();
      if (event.key === "Enter") finish(true);
      else if (event.key === "Escape") finish(false);
    });
    input.addEventListener("blur", () => finish(true));
    for (const type of ["pointerdown", "dblclick"]) input.addEventListener(type, (event) => event.stopPropagation());
    name.replaceWith(input);
    input.focus();
    input.select();
  }

  destroy() {
    if (this.mapping) this.setMapping(false);
  }
}
