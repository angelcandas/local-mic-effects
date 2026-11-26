// Labels in Practice: cues for the singer written on the notes and phrases of the lane (Grit,
// Prepare position, Slower, Piano, Forte...), the way a vocal coach marks a lead sheet.
//
//   right-click a note       label it (or its phrase, when you click between notes)
//   Labels button            the label library: icon, colour and text, shared by every song
//
// The engine (src/Practice.*) keeps the library in the settings and each song's labels in its
// folder (labels-v1.txt); this module is the popover, the library editor and the chips drawn on
// the lane. Events: "practice" { state: "annotations", annotations }.
import { html } from "./widgets.js";

const MAX_STACK = 3;

// Light or dark text for a label colour.
const inkFor = (colour) => {
  const n = parseInt(colour.slice(1), 16);
  const lum = (0.299 * (n >> 16) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255)) / 255;
  return lum > 0.58 ? "#10131a" : "#ffffff";
};

export class LabelTools {
  // `host`: { practice, section, stage, getSong(), now(), secondsShown, playhead, layout() -> { top, rowHeight, ratio } }
  constructor(host) {
    this.host = host;
    this.library = [];
    this.annotations = [];   // [{ kind, time, labels }]
    this.noteLabels = new Map();
    this.phraseLabels = new Map();
    this.target = null;      // { kind, time, labels } while the popover is open
    this.build();
    host.practice("labels").then((r) => { if (r) { this.setLibrary(r.library); this.setAnnotations(r.annotations); } }).catch(() => {});
  }

  build() {
    // The popover for a note or phrase.
    this.pop = html("div", "practice-labelpop");
    this.pop.hidden = true;
    this.pop.addEventListener("pointerdown", (event) => event.stopPropagation());
    this.pop.addEventListener("keydown", (event) => { if (event.key === "Escape") { this.closePop(); event.stopPropagation(); } });
    document.addEventListener("pointerdown", (event) => { if (!this.pop.hidden && !this.pop.contains(event.target)) this.closePop(); });
    this.host.stage.append(this.pop);

    // The library editor.
    this.editor = html("div", "practice-labels");
    this.editor.hidden = true;
    this.host.section.insertBefore(this.editor, this.host.stage);
  }

  //==============================================================================
  setLibrary(library) {
    this.library = (library ?? []).map((l) => ({ ...l }));
    if (!this.editor.hidden) this.buildEditor();
  }

  setAnnotations(annotations) {
    this.annotations = annotations ?? [];
    this.match();
  }

  // Which note or phrase each annotation is on (they're sent by start time, as the song plays).
  match() {
    this.noteLabels.clear();
    this.phraseLabels.clear();
    const song = this.host.getSong();
    if (!song) return;
    for (const a of this.annotations) {
      const list = a.kind === "phrase" ? song.phrases : song.notes;
      const i = list.findIndex((x) => Math.abs(x.start - a.time) < 0.03);
      if (i >= 0) (a.kind === "phrase" ? this.phraseLabels : this.noteLabels).set(i, a.labels);
    }
  }

  //==============================================================================
  // What's under a pointer in the lane: a note, else the phrase around it.
  targetAt(event) {
    const song = this.host.getSong();
    const layout = this.host.layout();
    if (!song || !layout) return null;
    const box = this.host.stage.getBoundingClientRect();
    const t = this.host.now() + ((event.clientX - box.left) / box.width - this.host.playhead) * this.host.secondsShown;
    const midi = layout.top - (event.clientY - box.top) / (layout.rowHeight / layout.ratio);
    let best = -1, distance = 1.1;
    song.notes.forEach((n, i) => {
      if (t < n.start - 0.15 || t > n.end + 0.15) return;
      const d = Math.abs(n.pitch - midi);
      if (d < distance) { distance = d; best = i; }
    });
    if (best >= 0) return { kind: "note", time: song.notes[best].start, index: best, labels: this.noteLabels.get(best) ?? [] };
    const phrase = song.phrases.findIndex((p) => t >= p.start - 0.3 && t <= p.end + 0.3);
    if (phrase >= 0) return { kind: "phrase", time: song.phrases[phrase].start, index: phrase, labels: this.phraseLabels.get(phrase) ?? [] };
    return null;
  }

  openFor(event) {
    const target = this.targetAt(event);
    if (!target) return false;
    event.preventDefault();
    this.target = target;
    this.buildPop();
    const box = this.host.stage.getBoundingClientRect();
    this.pop.hidden = false;
    const x = Math.min(event.clientX - box.left, Math.max(0, box.width - this.pop.offsetWidth - 8));
    const y = Math.min(event.clientY - box.top, Math.max(0, box.height - this.pop.offsetHeight - 8));
    this.pop.style.left = `${Math.max(4, x)}px`;
    this.pop.style.top = `${Math.max(4, y)}px`;
    return true;
  }

  closePop() { this.pop.hidden = true; this.target = null; }

  buildPop() {
    const t = this.target;
    this.pop.replaceChildren();
    this.pop.append(html("div", "practice-labelpop-title", t.kind === "phrase" ? "Label this phrase" : "Label this note"));
    const chips = html("div", "practice-labelpop-chips");
    for (const label of this.library) {
      const on = t.labels.some((l) => l.text === label.text);
      const chip = Object.assign(html("button", `practice-label-chip${on ? " is-on" : ""}`), { type: "button", title: on ? "Click to remove" : "Add to this " + t.kind });
      chip.style.setProperty("--label", label.colour);
      chip.append(html("span", "practice-label-icon", label.icon), html("span", "", label.text));
      chip.addEventListener("click", () => {
        const labels = on ? t.labels.filter((l) => l.text !== label.text) : [...t.labels, label];
        this.apply(labels);
      });
      chips.append(chip);
    }
    this.pop.append(chips);

    // A new label, made on the spot and kept in the library.
    const form = html("form", "practice-labelpop-new");
    const icon = Object.assign(document.createElement("input"), { className: "practice-label-field is-icon", placeholder: "✦", maxLength: 4, title: "An icon: an emoji or a letter" });
    const text = Object.assign(document.createElement("input"), { className: "practice-label-field", placeholder: "New label (e.g. Open the vowel)", maxLength: 40 });
    const colour = Object.assign(document.createElement("input"), { type: "color", value: "#8c7bff", className: "practice-label-colour", title: "Colour" });
    const add = Object.assign(html("button", "tool is-accent", "Add"), { type: "submit" });
    form.append(icon, text, colour, add);
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const name = text.value.trim();
      if (!name) return;
      const label = { id: "", text: name, icon: icon.value.trim() || "•", colour: colour.value };
      this.setLibrary(await this.host.practice("saveLabels", { library: [...this.library, label] }));
      this.apply([...t.labels, this.library[this.library.length - 1] ?? label]);
    });
    this.pop.append(form);

    const foot = html("div", "practice-labelpop-foot");
    const clear = Object.assign(html("button", "text-button", "Clear labels"), { type: "button" });
    clear.disabled = !t.labels.length;
    clear.addEventListener("click", () => this.apply([]));
    const manage = Object.assign(html("button", "text-button", "Label library…"), { type: "button" });
    manage.addEventListener("click", () => { this.closePop(); this.toggleEditor(true); });
    foot.append(clear, manage);
    this.pop.append(foot);
  }

  async apply(labels) {
    const t = this.target;
    if (!t) return;
    t.labels = labels;
    this.buildPop();
    await this.host.practice("annotate", { kind: t.kind, time: t.time, labels: labels.map(({ text, icon, colour }) => ({ text, icon, colour })) });
  }

  //==============================================================================
  toggleEditor(on = this.editor.hidden) {
    this.editor.hidden = !on;
    if (on) this.buildEditor();
  }

  buildEditor() {
    const e = this.editor;
    e.replaceChildren();
    const head = html("div", "practice-labels-head");
    const close = Object.assign(html("button", "mini", "×"), { type: "button", title: "Close" });
    close.addEventListener("click", () => this.toggleEditor(false));
    head.append(html("strong", "", "Label library"), html("small", "", "Shared by every song. Right-click a note in the lane to use them."), close);
    e.append(head);

    const rows = html("div", "practice-labels-rows");
    const save = () => this.host.practice("saveLabels", { library: this.library }).then((r) => { this.library = r.map((l) => ({ ...l })); });
    this.library.forEach((label, i) => {
      const row = html("div", "practice-labels-row");
      const icon = Object.assign(document.createElement("input"), { className: "practice-label-field is-icon", value: label.icon, maxLength: 4 });
      const text = Object.assign(document.createElement("input"), { className: "practice-label-field", value: label.text, maxLength: 40 });
      const colour = Object.assign(document.createElement("input"), { type: "color", value: label.colour, className: "practice-label-colour" });
      const del = Object.assign(html("button", "mini", "×"), { type: "button", title: "Remove from the library (songs keep the labels they have)" });
      const preview = html("span", "practice-label-chip is-preview");
      const paint = () => { preview.style.setProperty("--label", label.colour); preview.replaceChildren(html("span", "practice-label-icon", label.icon), html("span", "", label.text)); };
      paint();
      icon.addEventListener("change", () => { label.icon = icon.value.trim() || "•"; paint(); save(); });
      text.addEventListener("change", () => { label.text = text.value.trim() || label.text; paint(); save(); });
      colour.addEventListener("change", () => { label.colour = colour.value; paint(); save(); });
      del.addEventListener("click", () => { this.library.splice(i, 1); save().then(() => this.buildEditor()); });
      row.append(icon, text, colour, preview, del);
      rows.append(row);
    });
    e.append(rows);

    const tools = html("div", "practice-labels-tools");
    const add = Object.assign(html("button", "tool", "+ New label"), { type: "button" });
    add.addEventListener("click", async () => {
      this.library.push({ id: "", text: "New label", icon: "•", colour: "#8c7bff" });
      await save();
      this.buildEditor();
    });
    const reset = Object.assign(html("button", "text-button", "Back to the standard set"), { type: "button" });
    reset.addEventListener("click", async () => { this.setLibrary(await this.host.practice("saveLabels", { reset: true })); });
    const exportButton = Object.assign(html("button", "text-button", "Export / import…"), { type: "button" });
    const box = Object.assign(document.createElement("textarea"), { className: "practice-labels-json", spellcheck: false, rows: 4,
                                                                    placeholder: "Paste a library here and press Import" });
    box.hidden = true;
    const importButton = Object.assign(html("button", "tool", "Import"), { type: "button" });
    importButton.hidden = true;
    exportButton.addEventListener("click", () => {
      box.hidden = importButton.hidden = !box.hidden;
      box.value = JSON.stringify(this.library.map(({ text, icon, colour }) => ({ text, icon, colour })), null, 1);
    });
    importButton.addEventListener("click", async () => {
      try {
        const list = JSON.parse(box.value);
        if (!Array.isArray(list)) throw new Error("not a list");
        this.setLibrary(await this.host.practice("saveLabels", { library: list }));
      } catch { box.value = "That isn't a label list. Export one to see the format."; }
    });
    tools.append(add, reset, exportButton, importButton);
    e.append(tools, box);
  }

  //==============================================================================
  // The chips on the lane: above each labelled note, and at the top of each labelled phrase.
  draw(g, { x, y, pxPerSecond, barHeight, from, to, ratio, width }) {
    const song = this.host.getSong();
    if (!song || (!this.noteLabels.size && !this.phraseLabels.size)) return;
    const h = 15 * ratio, pad = 5 * ratio;
    g.font = `600 ${10 * ratio}px ui-sans-serif, system-ui, sans-serif`;
    g.textBaseline = "middle";
    const chip = (label, left, top) => {
      const text = `${label.icon} ${label.text}`;
      const w = g.measureText(text).width + pad * 2;
      if (left + w < 0 || left > width) return w;
      g.fillStyle = label.colour;
      g.globalAlpha = 0.95;
      g.beginPath();
      g.roundRect(left, top, w, h, h / 2);
      g.fill();
      g.globalAlpha = 1;
      g.fillStyle = inkFor(label.colour);
      g.fillText(text, left + pad, top + h / 2 + ratio * 0.3);
      return w;
    };
    for (const [i, labels] of this.noteLabels) {
      const n = song.notes[i];
      if (!n || n.end < from || n.start > to) continue;
      let top = y(n.pitch) - barHeight / 2 - h - 3 * ratio;
      for (const label of labels.slice(0, MAX_STACK)) { chip(label, x(n.start), top); top -= h + 2 * ratio; }
    }
    for (const [i, labels] of this.phraseLabels) {
      const p = song.phrases[i];
      if (!p || p.end < from || p.start > to) continue;
      let left = x(p.start);
      for (const label of labels) left += chip(label, left, 4 * ratio) + 3 * ratio;
    }
  }
}
