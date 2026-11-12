// Take lanes under a track, comping, and Auto Comp (see daw::Edit "Takes and comping"
// and src/analysis/TakeComp.h).
//
// A track with takes gets a "takes" switch in its header. Unfolded, a tools row and one
// lane per take sit under it. The parts of each take the comp plays are lit up. In a
// lane, click to comp that part of the song from this take (the comp's piece under the
// click), or drag across to comp exactly that range. The ear button plays one take
// instead of the comp. "Pick best" asks Auto Comp to choose, phrase by phrase, and its
// reasons show along the tools row.

import { html } from "./widgets.js";

const COMP_FADE = 0.008; // daw::Edit::compFadeSeconds

export const takeRowHeight = (rowHeight) => Math.max(34, Math.round(rowHeight * 0.62));

export class TakeLanes {
  constructor(screen, { daw, toast }) {
    this.screen = screen;
    this.daw = daw;
    this.toast = toast;
    this.notes = new Map();   // track id -> Auto Comp picks, shown along the tools row
    this.busy = null;         // { track, progress } while Auto Comp listens
    this.waiting = [];        // resolvers for autoComp() promises
    this.version = 0;         // bumps when what the rows show changes outside the session
    window.__JUCE__.backend.addEventListener("dawTakeComp", (event) => this.onEvent(event));
  }

  // The header switch that folds and unfolds a track's lanes (null without takes).
  createToggle(track) {
    const count = track.takes?.length ?? 0;
    if (!count) return null;
    const shown = Boolean(track.takesShown);
    // Three stacked lanes and the count: "≡ 3".
    const b = html("button", "tt tt-takes");
    b.type = "button";
    b.innerHTML = `<svg viewBox="0 0 12 10" width="11" height="9" aria-hidden="true"><path d="M1 1.5h10M1 5h10M1 8.5h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>${count}`;
    b.setAttribute("aria-label", `${count} take${count === 1 ? "" : "s"}`);
    b.title = shown ? "Fold the take lanes" : "Show the take lanes, to pick the best parts of each take";
    b.setAttribute("aria-pressed", String(shown));
    b.addEventListener("click", (event) => {
      event.stopPropagation();
      this.daw("takes", { action: "show", track: track.id, on: !shown });
    });
    return b;
  }

  // The rows under a track: tools, then one lane per take. Empty when folded.
  createRows(track, colour) {
    if (!track.takes?.length || !track.takesShown) return [];
    const height = takeRowHeight(this.screen.rowHeight);
    return [this.createToolsRow(track, colour), ...track.takes.map((take) => this.createTakeRow(track, take, colour, height))];
  }

  createToolsRow(track, colour) {
    const row = html("div", "take-tools-row");
    row.style.setProperty("--track-color", colour);

    const head = html("div", "take-head take-tools-head");
    const busy = this.busy && (this.busy.track === track.id || this.busy.track < 0);
    const pick = html("button", "mini take-pick", busy ? `Listening… ${Math.round((this.busy.progress ?? 0) * 100)}%` : "✦ Pick best");
    pick.type = "button";
    pick.disabled = Boolean(this.busy);
    pick.title = track.takes.length < 2 ? "Record or add another take to choose between"
      : "Auto Comp: listens to every take and builds the comp from the best one in each phrase (in tune, whole line, no clipping). One undo takes it back.";
    if (track.takes.length < 2) pick.disabled = true;
    pick.addEventListener("click", () => this.autoComp(track.id));
    const listening = track.listenTake >= 0;
    const comp = html("button", "mini take-comp-listen", "Comp");
    comp.type = "button";
    comp.title = "Hear the comp (what the track plays)";
    comp.setAttribute("aria-pressed", String(!listening));
    comp.addEventListener("click", () => this.daw("takes", { action: "listen", track: track.id, take: -1 }));
    head.append(html("span", "take-tools-label", "Takes"), comp, pick);

    const lane = html("div", "take-tools-lane");
    for (const note of this.notes.get(track.id) ?? []) {
      const tag = html("span", "take-note", `${note.name}: ${note.reason}`);
      tag.style.left = `${note.start * this.screen.zoom}px`;
      tag.style.width = `${Math.max(2, (note.end - note.start) * this.screen.zoom)}px`;
      tag.title = `${note.name}, ${formatTime(note.start)}–${formatTime(note.end)}: ${note.reason} (score ${Math.round(note.score)})`;
      lane.append(tag);
    }
    row.append(head, lane);
    return row;
  }

  createTakeRow(track, take, colour, height) {
    const row = html("div", "take-row");
    row.dataset.take = take.id;
    row.style.setProperty("--track-color", colour);
    row.style.height = `${height}px`;
    const listening = track.listenTake === take.id;
    row.classList.toggle("is-listening", listening);

    const head = html("div", "take-head");
    const name = html("input", "take-name");
    Object.assign(name, { value: take.name, spellcheck: false, title: "Rename the take" });
    name.addEventListener("change", () => this.daw("takes", { action: "rename", take: take.id, name: name.value.trim() || take.name }));
    name.addEventListener("keydown", (event) => { if (event.key === "Enter") name.blur(); });

    const listen = html("button", "mini take-listen", "🎧");
    listen.type = "button";
    listen.title = listening ? "Back to the comp" : `Hear ${take.name} on its own, instead of the comp`;
    listen.setAttribute("aria-pressed", String(listening));
    listen.addEventListener("click", () => this.daw("takes", { action: "listen", track: track.id, take: listening ? -1 : take.id }));

    const remove = html("button", "mini is-danger take-remove", "×");
    remove.type = "button";
    remove.title = "Delete this take lane (the comp keeps what it uses of it)";
    remove.addEventListener("click", async () => {
      await this.daw("takes", { action: "remove", take: take.id });
      this.toast(`Deleted ${take.name}. Ctrl+Z brings it back.`);
    });
    head.append(name, listen, remove);

    const lane = html("div", "take-lane");
    lane.title = `${take.name}: click to use it for that part of the song, drag to use it for a range`;
    const zoom = this.screen.zoom;

    // Where the comp plays this take (between the crossfades' middles).
    const used = (track.clips ?? []).filter((c) => c.take === take.id).map((c) => {
      const inner = (fade) => (Math.abs((fade ?? 0) - COMP_FADE) < 1e-4 ? COMP_FADE / 2 : 0);
      return [c.start + inner(c.fadeIn), c.start + c.length - inner(c.fadeOut)];
    });

    for (const clip of take.clips) {
      const element = html("div", "take-clip");
      element.style.left = `${clip.start * zoom}px`;
      element.style.width = `${Math.max(2, clip.length * zoom)}px`;
      element.classList.toggle("is-missing", Boolean(clip.missing));

      const canvas = html("canvas", "clip-wave");
      canvas.dataset.color = colour;
      canvas.clip = clip;
      canvas.laneHeight = height - 6;
      element.append(canvas);

      for (const [a, b] of used) {
        const from = Math.max(a, clip.start), to = Math.min(b, clip.start + clip.length);
        if (to - from <= 0.001) continue;
        const lit = html("div", "take-used");
        lit.style.left = `${(from - clip.start) * zoom}px`;
        lit.style.width = `${(to - from) * zoom}px`;
        element.append(lit);
      }
      this.screen.drawWaveform(canvas);
      lane.append(element);
    }

    const selection = html("div", "take-select");
    selection.hidden = true;
    lane.append(selection);
    lane.addEventListener("pointerdown", (event) => this.startComp(event, lane, selection, track, take));

    row.append(head, lane);
    return row;
  }

  // Click: comp the piece under the pointer from this take. Drag: comp the range.
  startComp(event, lane, selection, track, take) {
    if (event.button !== 0) return;
    event.preventDefault();
    const box = lane.getBoundingClientRect();
    const timeAt = (e) => Math.max(0, (e.clientX - box.left) / this.screen.zoom);
    const t0 = timeAt(event);
    let moved = false, range = null;
    lane.setPointerCapture?.(event.pointerId);
    this.screen.select(track.id, null, false);

    const move = (e) => {
      if (!moved && Math.abs(e.clientX - event.clientX) < 4) return;
      moved = true;
      const a = this.screen.snapTime(t0, e), b = this.screen.snapTime(timeAt(e), e);
      range = [Math.min(a, b), Math.max(a, b)];
      selection.hidden = false;
      selection.style.left = `${range[0] * this.screen.zoom}px`;
      selection.style.width = `${(range[1] - range[0]) * this.screen.zoom}px`;
    };
    const up = async () => {
      lane.removeEventListener("pointermove", move);
      lane.removeEventListener("pointerup", up);
      lane.removeEventListener("pointercancel", up);
      selection.hidden = true;
      this.clearNotes(track.id);
      if (moved && range && range[1] - range[0] > 0.01)
        await this.daw("takes", { action: "comp", take: take.id, start: range[0], end: range[1] });
      else if (!moved)
        await this.daw("takes", { action: "compAt", take: take.id, time: t0 });
    };
    lane.addEventListener("pointermove", move);
    lane.addEventListener("pointerup", up);
    lane.addEventListener("pointercancel", up);
  }

  clearNotes(trackId) {
    if (this.notes.delete(trackId)) this.version++;
  }

  // Auto Comp on one track (or every track with takes, -1). Resolves true when it's done
  // and applied, false when it was cancelled or failed.
  async autoComp(trackId) {
    const result = await this.daw("takes", { action: "auto", track: trackId });
    if (result?.ok === false) return false;
    this.busy = { track: trackId, progress: 0 };
    this.version++;
    this.screen.renderArrange(true);
    return new Promise((resolve) => this.waiting.push(resolve));
  }

  cancel() {
    if (this.busy) this.daw("takes", { action: "cancel" });
  }

  onEvent(event) {
    if (event.state === "analysing") {
      if (!this.busy) this.busy = { track: -1, progress: 0 };
      this.busy.progress = event.progress ?? 0;
      for (const button of this.screen.rows.querySelectorAll(".take-pick"))
        button.textContent = `Listening… ${Math.round(this.busy.progress * 100)}%`;
      return;
    }

    this.busy = null;
    const done = event.state === "done";
    if (done) {
      const summaries = [];
      for (const { track, picks, summary } of event.tracks ?? []) {
        this.notes.set(track, picks);
        const name = this.screen.session?.tracks.find((t) => t.id === track)?.name;
        if (summary) summaries.push(name ? `${name}: ${summary}` : summary);
      }
      this.toast(`Auto Comp. ${summaries.join(" ")} Ctrl+Z puts the old comp back.`);
    } else if (event.state === "error") {
      this.toast(event.error || "Auto Comp failed.", "is-error");
    }
    this.version++;
    this.screen.renderArrange(true);
    for (const resolve of this.waiting.splice(0)) resolve(done);
  }
}

function formatTime(seconds) {
  const m = Math.floor(seconds / 60), s = seconds - m * 60;
  return `${m}:${s.toFixed(1).padStart(4, "0")}`;
}
