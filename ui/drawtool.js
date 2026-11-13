// The Draw tool (P), as FL Studio's pencil and brush in one: with a pattern or a sound picked
// (the "brush": a pattern in the rack or the library's Project list, or an audio file in the
// library), a click on an empty spot places it, a drag paints copies back to back, and a
// right-click or right-drag erases the clips and pattern blocks under the pointer. Each stroke
// is one undo step. Patterns go in the Patterns lane, sounds on audio tracks.
//
// Dragging an existing clip or block still moves it, as in the Select tool.
import { html } from "./widgets.js";

export function createDrawTool(screen, daw, { toast }) {
  // The chip by the tools that tells what the Draw tool places; a click picks the tool.
  const chip = html("button", "draw-brush");
  chip.type = "button";
  chip.addEventListener("click", () => screen.setTool("draw"));

  const brushPattern = () => (screen.session?.patterns ?? []).find((p) => p.id === screen.selectedPattern) ?? screen.session?.patterns?.[0] ?? null;
  const brush = () => (screen.brush?.kind === "audio" ? screen.brush : brushPattern() ? { kind: "pattern", pattern: brushPattern() } : null);

  const renderChip = () => {
    const b = brush();
    const name = b?.kind === "audio" ? b.name : b?.pattern.name;
    chip.textContent = name ? `Draw: ${name}` : "Draw: pick a pattern or a sound";
    chip.title = b?.kind === "audio"
      ? `The Draw tool places ${b.name}${b.loop ? ` (${b.loop.bars} bar${b.loop.bars === 1 ? "" : "s"}, in tempo)` : ""} on audio tracks: click to place, drag to paint, right-click to erase (P)`
      : "The Draw tool places the picked pattern in the Patterns lane: click to place, drag to paint, right-click to erase (P). Pick a pattern in the rack or the library's Project list, or a sound in the library.";
    chip.classList.toggle("is-on", screen.tool === "draw");
    chip.hidden = !b && screen.tool !== "draw";
  };

  // Picks what the Draw tool places: a library payload ({ kind: "pattern", id } or
  // { kind: "audio", file, name }).
  const pick = async (payload) => {
    if (payload?.kind === "pattern") {
      screen.brush = null;
      screen.selectedPattern = payload.id;
      screen.patternsPanel?.update(screen.session);
    } else if (payload?.kind === "audio") {
      const info = await daw("audioInfo", { file: payload.file });
      if (!info?.ok) return;
      screen.brush = { kind: "audio", file: payload.file, name: payload.name, length: info.length, loop: info.loop ?? null };
    }
    renderChip();
  };

  //------------------------------------------------------------------ painting
  const barsOf = (beats) => Math.floor(beats / (screen.beatsPerBar || 4) + 1e-6) * (screen.beatsPerBar || 4);
  const secondsAt = (event, lane) => Math.max(0, (event.clientX - lane.getBoundingClientRect().left) / screen.zoom);

  // A stroke: from the first click, copies `size` long (in the unit `at` and `toX` use) as
  // far as the pointer goes. Shows ghosts in `lane`; `done(starts)` on release.
  const stroke = (event, lane, origin, size, at, toX, ghostClass, top, height, done) => {
    event.preventDefault();
    event.stopPropagation();
    const ghosts = [];
    let starts = [origin];
    const show = () => {
      ghosts.forEach((g) => g.remove());
      ghosts.length = 0;
      for (const s of starts) {
        const g = html("div", ghostClass);
        const left = toX(s), right = toX(s + size);
        g.style.cssText = `left:${left}px;width:${Math.max(4, right - left)}px;top:${top}px;height:${height}px`;
        lane.append(g);
        ghosts.push(g);
      }
    };
    show();
    screen.dragging = true;
    lane.setPointerCapture?.(event.pointerId);
    const move = (e) => {
      const count = Math.max(1, Math.min(256, Math.floor((at(e) - origin) / size) + 1));
      if (count !== starts.length) {
        starts = Array.from({ length: count }, (_, i) => origin + i * size);
        show();
      }
    };
    const up = async () => {
      lane.removeEventListener("pointermove", move);
      lane.removeEventListener("pointerup", up);
      lane.removeEventListener("pointercancel", up);
      screen.dragging = false;
      try { await done(starts); } finally { ghosts.forEach((g) => g.remove()); screen.flushSession?.(); }
    };
    lane.addEventListener("pointermove", move);
    lane.addEventListener("pointerup", up);
    lane.addEventListener("pointercancel", up);
  };

  const paintPatterns = (event, lane) => {
    const b = brush();
    if (b?.kind !== "pattern") {
      toast(b?.kind === "audio" ? "The picked sound goes on audio tracks. Pick a pattern to draw here." : "Pick a pattern first (in the Patterns tab or the library's Project list).");
      return;
    }
    const map = screen.tempoMap;
    const origin = barsOf(map.toBeats(secondsAt(event, lane)));
    const size = b.pattern.length;
    stroke(event, lane, origin, size, (e) => map.toBeats(secondsAt(e, lane)), (beats) => map.toSeconds(beats) * screen.zoom,
      "pl-block is-ghost", 2, 26,
      (starts) => daw("addPatternClips", { pattern: b.pattern.id, starts, length: size }));
  };

  const paintAudio = (event, lane, track) => {
    const b = brush();
    if (b?.kind !== "audio") {
      toast(b?.kind === "pattern" ? "Patterns are drawn in the Patterns lane under the ruler. Pick a sound in the library to draw on audio tracks." : "Pick a sound in the library first.");
      return;
    }
    const map = screen.tempoMap;
    const at = secondsAt(event, lane);
    // A loop starts on a bar; anything else on the grid.
    const origin = b.loop ? map.toSeconds(Math.round(map.toBeats(at) / (screen.beatsPerBar || 4)) * (screen.beatsPerBar || 4)) : screen.snapTime(at, event);
    const size = Math.max(0.01, b.length);
    stroke(event, lane, origin, size, (e) => secondsAt(e, lane), (s) => s * screen.zoom, "clip-ghost", 2, Math.max(10, lane.clientHeight - 4),
      async (starts) => {
        // The first copy brings the file into the project; the others use that copy.
        let file = b.file;
        for (const [i, start] of starts.entries()) {
          const result = await daw("importFile", { file, track: track.id, start, match: Boolean(b.loop), ...(i ? { continue: true } : {}) });
          if (!result?.ok) break;
          file = result.file || file;
        }
      });
  };

  //------------------------------------------------------------------ erasing
  const erase = (event) => {
    event.preventDefault();
    event.stopPropagation();
    const clips = new Set(), blocks = new Set();
    const take = (e) => {
      const under = document.elementFromPoint(e.clientX, e.clientY);
      const clip = under?.closest?.(".clip[data-clip]");
      const block = under?.closest?.(".pl-block[data-id]");
      if (clip && !clips.has(Number(clip.dataset.clip))) { clips.add(Number(clip.dataset.clip)); clip.classList.add("is-erasing"); }
      if (block && !block.classList.contains("is-ghost") && !blocks.has(Number(block.dataset.id))) { blocks.add(Number(block.dataset.id)); block.classList.add("is-erasing"); }
    };
    take(event);
    screen.dragging = true;
    const move = (e) => take(e);
    const up = async () => {
      window.removeEventListener("pointermove", move, true);
      window.removeEventListener("pointerup", up, true);
      window.removeEventListener("pointercancel", up, true);
      screen.dragging = false;
      let first = true;
      if (blocks.size) { await daw("removePatternClips", { ids: [...blocks] }); first = false; }
      for (const id of clips) { await daw("removeClip", { id, ...(first ? {} : { continue: true }) }); first = false; }
      screen.flushSession?.();
    };
    window.addEventListener("pointermove", move, true);
    window.addEventListener("pointerup", up, true);
    window.addEventListener("pointercancel", up, true);
  };

  //------------------------------------------------------------------ wiring
  // The arrangement's gestures, taken before the lanes and clips see them (capture).
  const attach = (area) => {
  area.addEventListener("pointerdown", (event) => {
    if (screen.tool !== "draw") return;
    if (event.button === 2) return erase(event);
    if (event.button !== 0) return;
    if (event.target.classList?.contains("pattern-lane")) return paintPatterns(event, event.target);
    if (event.target.classList?.contains("lane")) {
      const row = event.target.closest(".track-row[data-track]");
      const track = screen.session?.tracks.find((t) => t.id === Number(row?.dataset.track));
      if (track && track.kind !== "midi") paintAudio(event, event.target, track);
      else if (track) {
        event.preventDefault();
        event.stopPropagation();
        toast("MIDI tracks play patterns from the Patterns lane under the ruler: draw the pattern there.");
      }
    }
  }, true);
  area.addEventListener("contextmenu", (event) => {
    if (screen.tool !== "draw") return;
    event.preventDefault();
    event.stopPropagation();
  }, true);
  // A MIDI track's ghost notes: a double-click opens the pattern that plays there.
  area.addEventListener("dblclick", (event) => {
    if (!event.target.classList?.contains("lane")) return;
    const row = event.target.closest(".track-row[data-track]");
    const track = screen.session?.tracks.find((t) => t.id === Number(row?.dataset.track));
    if (track?.kind !== "midi") return;
    const block = screen.patternLane?.blockAt(secondsAt(event, event.target));
    const part = block && screen.session.patterns?.find((p) => p.id === block.pattern)?.parts.find((p) => p.track === track.id);
    if (!part?.notes?.length) return;
    event.preventDefault();
    event.stopPropagation();
    screen.selectedPattern = block.pattern;
    screen.openPatterns();
  }, true);
  };

  return { chip, pick, update: renderChip, attach };
}
