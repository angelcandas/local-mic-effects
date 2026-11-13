// Song map (DAW): finds the song's sections (intro, verse, chorus...), its bars and, for a
// take played without a click, the tempo of the performance. The bridge command "songMap"
// renders the mix (or reads one clip) and analyses it offline, answering with "dawSongMap"
// events (see handleSongMap in src/DawBridge.cpp, src/analysis/Structure.h). Applying puts
// the sections on the ruler as coloured ranges (markers with a length) and/or makes the
// tempo map follow the playing, as one undo step.
//
// This view owns the panel; the DAW screen gives it `host`:
//   daw(command, args)   the bridge call
//   seek(seconds), loop(start, end)
//   setJob(id, job) / endJob(id)   the activity list's running jobs
//   closeOthers()        hide the other floating panels
import { html } from "./widgets.js";

const KIND_HINT = {
  intro: "first, heard once",
  verse: "comes back, quieter than the chorus",
  chorus: "the loudest part that comes back",
  bridge: "heard once, in the middle",
  outro: "last, heard once",
  part: "nothing repeats, so no names",
};

export class SongMapView {
  constructor(panel, host) {
    this.panel = panel;
    this.host = host;
    this.map = null;
    this.clip = null; // { id, name } when mapping one clip
    const $ = (selector) => panel.querySelector(selector);
    this.strip = $(".songmap-strip");
    this.list = $(".songmap-list");
    this.summary = $(".songmap-summary");

    $(".songmap-close").addEventListener("click", () => this.toggle(false));
    $(".songmap-cancel").addEventListener("click", () => host.daw("songMap", { action: "cancel" }));
    $(".songmap-detect").addEventListener("click", () => this.detect());
    $(".songmap-apply").addEventListener("click", () => this.apply());
    $("[name=source]").addEventListener("change", (event) => { event.target.dataset.touched = "1"; });
    window.__JUCE__.backend.addEventListener("dawSongMap", (event) => this.onEvent(event));
  }

  get running() { return this.panel.classList.contains("is-running"); }

  // `clip`: map one clip (an imported song) instead of the whole mix.
  toggle(show = this.panel.hidden, clip = null) {
    this.panel.hidden = !show;
    if (!show) return;
    this.host.closeOthers?.();
    if (!this.running && clip) {
      this.clip = clip;
      delete this.panel.querySelector("[name=source]").dataset.touched;
    }
    this.renderSource();
    this.render();
  }

  renderSource() {
    const select = this.panel.querySelector("[name=source]");
    const clipOption = select.querySelector("option[value=clip]");
    clipOption.hidden = !this.clip;
    clipOption.textContent = this.clip ? `The clip “${this.clip.name}”` : "A clip";
    if (this.clip && !select.dataset.touched) select.value = "clip";
    if (!this.clip) select.value = "mix";
  }

  setResult(text, isError = false) {
    this.panel.querySelector(".songmap-result").replaceChildren(text ? html("span", isError ? "is-error" : "", text) : "");
  }

  async detect() {
    const fromClip = this.panel.querySelector("[name=source]").value === "clip" && this.clip;
    this.setResult("Starting…");
    const result = await this.host.daw("songMap", { action: "detect", ...(fromClip ? { clip: this.clip.id } : {}) });
    if (result?.ok === false) this.setResult(result.error, true);
  }

  async apply() {
    const markers = this.panel.querySelector("[name=markers]").checked;
    const follow = this.panel.querySelector("[name=follow]").checked;
    if (!markers && !follow) return this.setResult("Tick what to add first.", true);
    const result = await this.host.daw("songMap", { action: "apply", markers, follow });
    if (result?.ok === false) return this.setResult(result.error, true);
    const parts = [];
    if (markers) parts.push(`${result.sections} section${result.sections === 1 ? "" : "s"} on the ruler`);
    if (follow) parts.push(result.leadInBars > 0 ? `the tempo follows the playing (${result.leadInBars} bar${result.leadInBars === 1 ? "" : "s"} of lead-in)` : "the tempo follows the playing");
    this.setResult(`Done: ${parts.join(", ")}. Ctrl+Z undoes it.`);
  }

  onEvent(event) {
    const running = event.state === "analysing";
    this.panel.classList.toggle("is-running", running);
    this.panel.querySelector(".export-bar").style.setProperty("--progress", event.progress ?? 1);
    if (running) {
      this.host.setJob("songmap", { label: "Song map", progress: event.progress ?? null, cancel: () => this.host.daw("songMap", { action: "cancel" }),
                                    open: () => this.toggle(true) });
      this.panel.hidden = false;
      return this.setResult("Listening to the song…");
    }
    this.host.endJob("songmap");
    if (event.state === "cancelled") return this.setResult("Cancelled.");
    if (event.state === "error") return this.setResult(event.error ?? "Something went wrong.", true);
    if (event.state !== "ready") return;

    this.map = event;
    this.panel.hidden = false;
    this.setResult(event.sections.length ? "" : "No sections found: is there music in the song?");
    // Follow the performance is worth it for a take whose tempo moves; a song made to a
    // click only needs the sections.
    this.panel.querySelector("[name=follow]").checked = event.hasBeats && event.drift > 0.012;
    this.render();
  }

  render() {
    const map = this.map;
    const $ = (selector) => this.panel.querySelector(selector);
    $(".songmap-detect").textContent = map ? "Listen again" : "Find sections";
    $(".songmap-apply").disabled = !map || !map.sections.length;
    $("[name=follow]").disabled = !map?.hasBeats;
    this.strip.hidden = this.list.hidden = this.summary.hidden = !map;
    $(".songmap-options").hidden = !map;
    if (!map) return;

    // Tempo: one number for a steady song; the range for a take that moves.
    const tempos = map.barTempos ?? [];
    const lo = tempos.length ? Math.min(...tempos) : map.bpm, hi = tempos.length ? Math.max(...tempos) : map.bpm;
    const stats = [];
    if (map.hasBeats) {
      stats.push(["Tempo", map.drift > 0.012 ? `${Math.round(lo)}–${Math.round(hi)} BPM` : `${map.bpm.toFixed(1)} BPM`]);
      stats.push(["Bars", String(map.bars)]);
      stats.push(["Timing", map.drift > 0.012 ? `moves ${(map.drift * 100).toFixed(1)} %` : "steady (a click?)"]);
    } else {
      stats.push(["Tempo", "no steady beat"]);
    }
    stats.push(["Sections", String(map.sections.length)]);
    this.summary.replaceChildren(...stats.map(([label, value]) => {
      const stat = html("div", "songmap-stat");
      stat.append(html("small", "", label), html("strong", "", value));
      return stat;
    }));

    // The strip: the sections side by side, as long as they are.
    const start = map.sections[0]?.start ?? 0, end = map.sections.at(-1)?.end ?? 1;
    this.strip.replaceChildren(...map.sections.map((s) => {
      const block = html("button", "songmap-block");
      block.style.flexGrow = String(Math.max(0.01, s.end - s.start));
      block.style.setProperty("--section", s.colour);
      block.title = `${s.name}: ${s.bars} bars from ${formatTime(s.start)}. Click to go there, double-click to loop it.`;
      block.append(html("span", "songmap-letter", s.letter), html("span", "songmap-name", s.name));
      block.addEventListener("click", () => this.host.seek(s.start));
      block.addEventListener("dblclick", () => this.host.loop(s.start, s.end));
      return block;
    }));
    this.strip.style.setProperty("--span", String(end - start));

    this.list.replaceChildren(...map.sections.map((s) => {
      const row = html("div", "songmap-row");
      row.style.setProperty("--section", s.colour);
      const name = html("span", "songmap-row-name");
      name.append(html("i", "songmap-dot"), html("strong", "", s.name), html("small", "", ` ${s.letter} · ${KIND_HINT[s.kind] ?? ""}`));
      const where = html("span", "songmap-row-where", `${formatTime(s.start)} · ${s.bars} bars`);
      const loop = html("button", "mini songmap-loop", "Loop");
      loop.title = "Loop this section";
      loop.addEventListener("click", () => this.host.loop(s.start, s.end));
      row.append(name, where, loop);
      row.addEventListener("click", (event) => { if (event.target !== loop) this.host.seek(s.start); });
      return row;
    }));
  }
}

function formatTime(seconds) {
  const m = Math.floor(seconds / 60), s = Math.floor(seconds % 60);
  return `${m}:${String(s).padStart(2, "0")}`;
}
