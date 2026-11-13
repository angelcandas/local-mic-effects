// The setlist (standalone app): a drawer with the show's songs, each with its sound,
// scenes, tempo, key, backing track and lyrics, and a stage view with big lyrics and
// next/previous song. Engine: src/Setlist.*; the page uses the "setlist" native function.
import * as Juce from "./juce/index.js";
import { NOTE_NAMES } from "./modules.js";
import { html } from "./widgets.js";
import { markLearnable } from "./midilearn.js";

const setlist = Juce.getNativeFunction("setlist");

export const SETLIST_KEYS = [["Page Down / Page Up", "Next / previous song (setlist; page-turner pedals send these)"]];

// "[01:23.45] words" lines -> [{ time, text }] sorted by time; null for plain lyrics.
export function parseLrc(text) {
  const lines = [];
  for (const raw of String(text ?? "").split(/\r?\n/)) {
    const stamps = [...raw.matchAll(/\[(\d+):(\d+(?:\.\d+)?)\]/g)];
    if (!stamps.length) continue;
    const words = raw.replace(/\[[^\]]*\]/g, "").trim();
    for (const [, m, s] of stamps) lines.push({ time: Number(m) * 60 + Number(s), text: words });
  }
  return lines.length ? lines.sort((a, b) => a.time - b.time) : null;
}

const button = (className, text, title, onClick) => {
  const b = html("button", className, text);
  b.type = "button";
  if (title) b.title = title;
  b.addEventListener("click", onClick);
  return b;
};

export class SetlistPanel {
  constructor(toggle, onStatus) {
    this.toggle = toggle;
    this.onStatus = onStatus; // shows warnings in the recorder's status line
    this.state = { songs: [], current: -1, available: false, recent: [] };
    this.selected = -1;
    this.position = 0;

    this.drawer = html("aside", "setlist-drawer");
    this.drawer.hidden = true;
    this.drawer.setAttribute("aria-label", "Setlist");
    document.body.append(this.drawer);
    this.buildDrawer();
    this.buildStage();

    toggle.addEventListener("click", () => this.show(this.drawer.hidden));
    document.addEventListener("keydown", (event) => {
      if (document.body.classList.contains("is-daw") || event.target.closest?.("input, textarea, select")) return;
      if (event.key === "PageDown") { event.preventDefault(); this.step(1); }
      else if (event.key === "PageUp") { event.preventDefault(); this.step(-1); }
      else if (event.key === "Escape" && !this.stage.hidden) { event.preventDefault(); this.showStage(false); }
    });
    window.__JUCE__.backend.addEventListener("midiAction", (action) => {
      if (action === "song.next") this.step(1);
      if (action === "song.previous") this.step(-1);
    });

    this.send("list");
  }

  send(action, args) {
    return setlist(action, args).then((state) => { this.update(state); return state; });
  }

  step(delta) {
    if (!this.state.songs.length) return;
    this.send("step", { delta });
  }

  show(on) {
    this.drawer.hidden = !on;
    this.toggle.setAttribute("aria-pressed", String(on));
  }

  buildDrawer() {
    const head = html("div", "setlist-head");
    this.title = html("strong", "setlist-title");
    this.dirty = html("span", "setlist-dirty");
    const close = button("mini setlist-close", "×", "Close", () => this.show(false));
    head.append(this.title, this.dirty, close);

    const files = html("div", "setlist-files");
    this.recent = document.createElement("select");
    this.recent.className = "dropdown setlist-recent";
    this.recent.title = "Recent setlists";
    this.recent.addEventListener("change", () => {
      if (this.recent.value) this.send("openRecent", { file: this.recent.value });
      this.recent.value = "";
    });
    files.append(
      button("tool", "New", "Start an empty setlist", () => {
        if (this.state.dirty && !this.newArmed) { this.newArmed = true; this.onStatus("The setlist has unsaved changes: press New again to drop them."); setTimeout(() => { this.newArmed = false; }, 3000); return; }
        this.newArmed = false;
        this.send("new");
      }),
      button("tool", "Open…", "Open a .micfxset file", () => this.send("open")),
      button("tool", "Save", "Save the setlist", () => this.send("save")),
      button("tool", "Save as…", "Save the setlist as a new file", () => this.send("saveAs")),
      this.recent);

    this.songs = html("ol", "setlist-songs");

    const actions = html("div", "setlist-actions");
    this.addButton = button("tool is-accent", "+ Add the current sound as a song", "Adds a song with the sound, scenes, looper tempo and backing track you have now", () => this.send("add").then(() => { this.selected = this.state.current; this.render(); }));
    this.updateButton = button("tool", "Save the sound to this song", "The current sound and scenes replace the selected song's", () => this.send("update", { index: this.selected }));
    this.stageButton = button("tool setlist-stage-open", "Stage view", "Big lyrics and next / previous song, for the stage", () => this.showStage(true));
    actions.append(this.addButton, this.updateButton, this.stageButton);

    // The selected song's details.
    this.editor = html("div", "setlist-editor");
    const field = (label, control) => { const l = html("label", "setlist-field"); l.append(html("span", "", label), control); return l; };
    this.nameInput = Object.assign(document.createElement("input"), { className: "setlist-input", maxLength: 60 });
    this.nameInput.addEventListener("change", () => this.info({ name: this.nameInput.value }));
    this.keySelect = document.createElement("select");
    this.keySelect.className = "dropdown";
    this.keySelect.append(new Option("No key", "-1"), ...NOTE_NAMES.map((n, i) => new Option(n, String(i))));
    this.keySelect.addEventListener("change", () => this.info({ key: Number(this.keySelect.value) }));
    this.modeSelect = document.createElement("select");
    this.modeSelect.className = "dropdown";
    this.modeSelect.append(new Option("Major", "0"), new Option("Minor", "1"));
    this.modeSelect.addEventListener("change", () => this.info({ minor: this.modeSelect.value === "1" }));
    this.tempoInput = Object.assign(document.createElement("input"), { type: "number", min: 0, max: 300, step: 0.5, className: "setlist-input setlist-tempo" });
    this.tempoInput.title = "The looper's tempo for this song (0: leave the looper as it is)";
    this.tempoInput.addEventListener("change", () => this.info({ tempo: Number(this.tempoInput.value) || 0 }));
    const keyRow = html("div", "setlist-row");
    keyRow.append(field("Key", this.keySelect), field("Mode", this.modeSelect), field("BPM", this.tempoInput));
    this.backing = html("span", "setlist-backing");
    const backingRow = html("div", "setlist-row");
    backingRow.append(html("span", "setlist-label", "Backing"), this.backing,
                      button("text-button", "Choose…", "Pick the song's backing track", () => this.send("chooseBacking", { index: this.selected })),
                      button("text-button", "None", "No backing track", () => this.info({ backing: "" })));
    this.lyrics = Object.assign(document.createElement("textarea"), { className: "setlist-lyrics", rows: 6, placeholder: "Lyrics (plain text, or .lrc lines like [01:02.50] words)" });
    this.lyrics.addEventListener("change", () => this.info({ lyrics: this.lyrics.value }));
    this.notes = Object.assign(document.createElement("textarea"), { className: "setlist-notes", rows: 2, placeholder: "Notes (capo, who starts, cues…)" });
    this.notes.addEventListener("change", () => this.info({ notes: this.notes.value }));
    const lyricsHead = html("div", "setlist-row");
    lyricsHead.append(html("span", "setlist-label", "Lyrics"), button("text-button", "Load .txt / .lrc…", "Read the lyrics from a file", () => this.send("chooseLyrics", { index: this.selected })));
    this.sceneNames = html("p", "setlist-scenes");
    this.editor.append(field("Name", this.nameInput), keyRow, backingRow, this.sceneNames, lyricsHead, this.lyrics, this.notes);

    this.drawer.append(head, files, this.songs, actions, this.editor);
  }

  info(fields) {
    if (this.selected >= 0) this.send("info", { index: this.selected, ...fields });
  }

  buildStage() {
    this.stage = html("div", "stage-view");
    this.stage.hidden = true;
    const top = html("div", "stage-top");
    this.stageCount = html("span", "stage-count");
    this.stageTitle = html("h2", "stage-title");
    this.stageInfo = html("span", "stage-info");
    const close = button("tool stage-close", "Close", "Back to the effects (Esc)", () => this.showStage(false));
    top.append(this.stageCount, this.stageTitle, this.stageInfo, close);
    this.stageLyrics = html("div", "stage-lyrics");
    this.stageNotes = html("p", "stage-notes");
    const bottom = html("div", "stage-bottom");
    this.stagePrevious = button("stage-nav", "‹ Previous song", "Previous song (Page Up)", () => this.step(-1));
    this.stageNext = button("stage-nav is-next", "Next song ›", "Next song (Page Down)", () => this.step(1));
    markLearnable(this.stagePrevious, "action:song.previous", "Previous song");
    markLearnable(this.stageNext, "action:song.next", "Next song");
    bottom.append(this.stagePrevious, this.stageNext);
    this.stage.append(top, this.stageLyrics, this.stageNotes, bottom);
    document.body.append(this.stage);
  }

  showStage(on) {
    this.stage.hidden = !on;
    document.body.classList.toggle("is-stage", on);
    if (on) { this.show(false); this.renderStage(); }
  }

  update(state) {
    if (!state || typeof state !== "object" || !Array.isArray(state.songs)) return;
    const songChanged = state.current !== this.state.current;
    this.state = state;
    this.toggle.hidden = !state.available;
    if (state.error) this.onStatus(state.error);
    else if (state.warning) this.onStatus(state.warning);
    if (songChanged && state.current >= 0) this.selected = state.current;
    if (this.selected >= state.songs.length) this.selected = state.songs.length - 1;
    this.render();
    if (!this.stage.hidden) this.renderStage();
  }

  render() {
    const s = this.state;
    this.title.textContent = s.name || "Setlist";
    this.dirty.textContent = s.dirty ? "•" : "";
    this.recent.replaceChildren(new Option("Recent…", ""), ...(s.recent ?? []).map((path) => new Option(path.split(/[\\/]/).pop().replace(/\.micfxset$/, ""), path)));
    this.recent.disabled = !(s.recent ?? []).length;

    this.songs.replaceChildren(...s.songs.map((song, index) => {
      const row = html("li", "setlist-song");
      row.classList.toggle("is-current", index === s.current);
      row.classList.toggle("is-selected", index === this.selected);
      const meta = [song.key >= 0 ? `${NOTE_NAMES[song.key]}${song.minor ? "m" : ""}` : "", song.tempo > 0 ? `${song.tempo} BPM` : "",
                    song.scenes?.length ? `${song.scenes.length} scene${song.scenes.length === 1 ? "" : "s"}` : ""].filter(Boolean).join(" · ");
      const name = button("setlist-song-name", song.name, "Load this song (its sound, scenes, tempo, key and backing track)", () => {
        this.selected = index;
        this.send("load", { index });
      });
      name.append(html("small", "", meta));
      const up = button("mini", "↑", "Move up", () => this.send("move", { index, to: index - 1 }).then(() => { this.selected = Math.max(0, index - 1); this.render(); }));
      const down = button("mini", "↓", "Move down", () => this.send("move", { index, to: index + 1 }).then(() => { this.selected = Math.min(s.songs.length - 1, index + 1); this.render(); }));
      const remove = button("mini", "×", "Remove from the setlist", () => this.send("remove", { index }));
      up.disabled = index === 0;
      down.disabled = index === s.songs.length - 1;
      row.append(html("span", "setlist-number", String(index + 1)), name, up, down, remove);
      return row;
    }));
    if (!s.songs.length) this.songs.append(html("li", "setlist-empty", "No songs yet. Set up the sound (and scenes, tempo, backing track) for a song, then add it here."));

    const song = s.songs[this.selected];
    this.editor.hidden = !song;
    this.updateButton.disabled = !song;
    this.stageButton.disabled = !s.songs.length;
    if (song) {
      if (document.activeElement !== this.nameInput) this.nameInput.value = song.name;
      this.keySelect.value = String(song.key ?? -1);
      this.modeSelect.value = song.minor ? "1" : "0";
      if (document.activeElement !== this.tempoInput) this.tempoInput.value = String(song.tempo || "");
      this.backing.textContent = song.backing ? song.backing.split(/[\\/]/).pop() : "None";
      this.backing.title = song.backing;
      if (document.activeElement !== this.lyrics) this.lyrics.value = song.lyrics ?? "";
      if (document.activeElement !== this.notes) this.notes.value = song.notes ?? "";
      this.sceneNames.textContent = song.scenes?.length ? `Scenes: ${song.scenes.join(", ")}` : "No scenes saved with this song.";
    }
  }

  renderStage() {
    const s = this.state;
    const song = s.songs[s.current] ?? s.songs[0];
    const index = s.songs.indexOf(song);
    this.stageCount.textContent = song ? `${index + 1} / ${s.songs.length}` : "";
    this.stageTitle.textContent = song?.name ?? "No song loaded";
    this.stageInfo.textContent = song ? [song.key >= 0 ? `${NOTE_NAMES[song.key]} ${song.minor ? "minor" : "major"}` : "", song.tempo > 0 ? `${song.tempo} BPM` : ""].filter(Boolean).join(" · ") : "";
    this.stageNotes.textContent = song?.notes ?? "";
    this.stagePrevious.disabled = index <= 0;
    this.stageNext.disabled = index < 0 || index >= s.songs.length - 1;
    this.stageNext.textContent = index >= 0 && index < s.songs.length - 1 ? `Next: ${s.songs[index + 1].name} ›` : "Last song";

    this.timed = parseLrc(song?.lyrics);
    if (this.timed) this.stageLyrics.replaceChildren(...this.timed.map((line) => html("p", "stage-line", line.text || "♪")));
    else this.stageLyrics.replaceChildren(...String(song?.lyrics || "").split(/\r?\n/).map((line) => html("p", "stage-line is-plain", line || " ")));
    this.stageLyrics.scrollTop = 0;
    this.lastLine = -1;
    this.followPosition(this.position);
  }

  // From the "meters" event: timed (.lrc) lyrics follow the backing track.
  followPosition(seconds) {
    this.position = seconds ?? 0;
    if (this.stage.hidden || !this.timed) return;
    let line = -1;
    for (let i = 0; i < this.timed.length && this.timed[i].time <= this.position + 0.05; i++) line = i;
    if (line === this.lastLine) return;
    this.lastLine = line;
    [...this.stageLyrics.children].forEach((p, i) => { p.classList.toggle("is-now", i === line); p.classList.toggle("is-past", i < line); });
    const now = this.stageLyrics.children[Math.max(0, line)];
    now?.scrollIntoView({ block: "center", behavior: "smooth" });
  }
}
