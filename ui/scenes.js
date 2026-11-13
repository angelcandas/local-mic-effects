// The scene bar at the top of the Live effect chain: snapshots of the sound for the parts
// of a song. Click a scene to glide to it; "+ Scene" saves the current sound; the active
// scene shows a dot when the sound has moved away from it, and "Save" puts the changes
// into it. Double-click renames; right-click opens a small menu. Engine: src/Scenes.*.
import * as Juce from "./juce/index.js";
import { html } from "./widgets.js";
import { markLearnable } from "./midilearn.js";

const scenes = Juce.getNativeFunction("scenes");

export const SCENE_KEYS = [["[ / ]", "Previous / next scene"]];

const GLIDES = [[0, "Instant"], [0.25, "0.25 s"], [0.5, "0.5 s"], [1, "1 s"], [2, "2 s"], [4, "4 s"], [8, "8 s"]];

export class SceneBar {
  constructor(element) {
    this.element = element;
    this.state = { scenes: [], active: -1, modified: false, glide: 0.5, max: 8 };

    const label = html("span", "scene-label", "Scenes");
    this.previous = this.button("scene-step", "‹", "Previous scene ([)", () => this.send("step", { delta: -1 }));
    this.next = this.button("scene-step", "›", "Next scene (])", () => this.send("step", { delta: 1 }));
    markLearnable(this.previous, "action:scene.previous", "Previous scene");
    markLearnable(this.next, "action:scene.next", "Next scene");
    this.list = html("div", "scene-list");
    this.add = this.button("scene-add", "+ Scene", "Save the current sound as a new scene", () => this.send("add"));
    this.save = this.button("scene-save", "Save", "Put the changes into the active scene", () => this.send("update", { index: this.state.active }));
    this.glide = document.createElement("select");
    this.glide.className = "dropdown scene-glide";
    this.glide.title = "How long a scene takes to glide in";
    for (const [seconds, text] of GLIDES) this.glide.append(Object.assign(document.createElement("option"), { value: seconds, textContent: text }));
    this.glide.addEventListener("change", () => this.send("glide", { seconds: Number(this.glide.value) }));
    const glideLabel = html("label", "scene-glide-label");
    glideLabel.append(html("span", "", "Glide"), this.glide);

    this.menu = html("div", "scene-menu");
    this.menu.hidden = true;
    document.addEventListener("pointerdown", (event) => { if (!this.menu.contains(event.target)) this.menu.hidden = true; });

    element.append(label, this.previous, this.list, this.next, this.add, this.save, glideLabel, this.menu);

    document.addEventListener("keydown", (event) => {
      if (document.body.classList.contains("is-daw") || event.metaKey || event.ctrlKey || event.altKey
          || event.target.closest?.("input, textarea, select, [contenteditable=true]")) return;
      if (event.key === "[") { event.preventDefault(); this.send("step", { delta: -1 }); }
      else if (event.key === "]") { event.preventDefault(); this.send("step", { delta: 1 }); }
    });

    this.build();
    this.send("list");
  }

  button(className, text, title, onClick) {
    const button = html("button", `tool ${className}`, text);
    button.type = "button";
    button.title = title;
    button.addEventListener("click", onClick);
    return button;
  }

  send(action, args) {
    return scenes(action, args).then((state) => this.update(state));
  }

  update(state) {
    if (!state || typeof state !== "object" || !Array.isArray(state.scenes)) return;
    const names = state.scenes.map((s) => s.name).join("\n");
    const rebuild = names !== this.state.scenes.map((s) => s.name).join("\n") || !this.list.childElementCount && state.scenes.length;
    this.state = state;

    if (rebuild && !this.list.querySelector("input")) this.build();
    [...this.list.querySelectorAll(".scene")].forEach((button, i) => {
      button.classList.toggle("is-active", i === state.active);
      button.classList.toggle("is-modified", i === state.active && state.modified);
    });
    this.element.classList.toggle("is-empty", state.scenes.length === 0);
    this.add.disabled = state.scenes.length >= state.max;
    this.save.hidden = !(state.active >= 0 && state.modified);
    this.save.textContent = state.active >= 0 ? `Save to ${state.scenes[state.active]?.name ?? ""}` : "Save";
    this.previous.disabled = this.next.disabled = state.scenes.length === 0;
    if (document.activeElement !== this.glide) this.glide.value = String(GLIDES.reduce((best, [s]) => Math.abs(s - state.glide) < Math.abs(best - state.glide) ? s : best, 0.5));
  }

  build() {
    this.list.replaceChildren(...this.state.scenes.map((scene, index) => {
      const button = html("button", "scene", scene.name);
      button.type = "button";
      button.title = `${scene.name}: click to glide here · double-click to rename · right-click for more`;
      markLearnable(button, `action:scene.${index + 1}`, `Scene ${scene.name}`);
      button.addEventListener("click", () => this.send("recall", { index }));
      button.addEventListener("dblclick", () => this.rename(button, index));
      button.addEventListener("contextmenu", (event) => {
        if (document.body.classList.contains("is-learning")) return;
        event.preventDefault();
        this.openMenu(button, index);
      });
      return button;
    }));
    if (this.state.scenes.length === 0)
      this.list.append(html("span", "scene-hint", "Save the sound of each part of the song (verse, chorus…) and glide between them."));
  }

  rename(button, index) {
    const input = Object.assign(document.createElement("input"), { className: "scene-rename", value: this.state.scenes[index]?.name ?? "", maxLength: 40 });
    button.replaceWith(input);
    input.focus();
    input.select();
    let done = false;
    const finish = (save) => {
      if (done) return;
      done = true;
      const name = input.value.trim();
      input.remove();
      if (save && name) this.send("rename", { index, name }).then(() => this.build());
      else this.build();
    };
    input.addEventListener("keydown", (event) => {
      event.stopPropagation();
      if (event.key === "Enter") finish(true);
      if (event.key === "Escape") finish(false);
    });
    input.addEventListener("blur", () => finish(true));
  }

  openMenu(button, index) {
    const item = (text, onClick, danger = false) => {
      const b = html("button", danger ? "is-danger" : "", text);
      b.type = "button";
      b.addEventListener("click", () => { this.menu.hidden = true; onClick(); });
      return b;
    };
    this.menu.replaceChildren(
      item("Rename", () => this.rename(this.list.querySelectorAll(".scene")[index], index)),
      item("Save the current sound here", () => this.send("update", { index })),
      item("Delete", () => this.send("remove", { index }).then(() => this.build()), true));
    const bar = this.element.getBoundingClientRect(), rect = button.getBoundingClientRect();
    this.menu.style.left = `${rect.left - bar.left}px`;
    this.menu.style.top = `${rect.bottom - bar.top + 4}px`;
    this.menu.hidden = false;
  }
}
