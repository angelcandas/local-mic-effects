// The Patterns lane under the ruler (blocks that place a pattern on the song) and the ghost
// notes each pattern block shows on the MIDI tracks it plays. The rack (the Patterns tab) is
// in patterns.js; the Draw tool's strokes are in drawtool.js.
//
// Blocks are in BEATS (start, length); the lane draws them through the tempo map.
import { html } from "./widgets.js";
import { patternColour } from "./patterns.js";

const ROW_H = 30;
const MAX_LANE_ROWS = 16; // Edit::maxPatternRows

// The Patterns lane. screen: zoom, snap, snapTime, tempoMap, beatsPerBar, session, dragging,
// selectedPattern, tool, patternLaneFolded.
export function createPatternLane(screen, daw, { popMenu, openPatterns }) {
  const element = html("div", "pattern-lane");
  element.title = "Patterns: click an empty spot to place the selected pattern · drag to move · drag the right edge to repeat";
  element.tabIndex = -1;
  let width = 0, clips = [], patterns = [];
  let selected = null;

  const patternOf = (id) => patterns.find((p) => p.id === id);
  const barsOf = (beats) => Math.floor(beats / (screen.beatsPerBar || 4) + 1e-6) * (screen.beatsPerBar || 4);
  const folded = () => Boolean(screen.patternLaneFolded);
  const rowCount = () => (folded() ? 1 : Math.max(1, ...clips.map((c) => c.row + 1)));
  const rowTop = (row) => (folded() ? 0 : row) * ROW_H + 2;

  // The first row where [start, end) is free (so overlapping blocks stack).
  const freeRow = (start, length, skip) => {
    for (let row = 0; row < MAX_LANE_ROWS; ++row)
      if (!clips.some((c) => c.id !== skip && c.row === row && c.start < start + length - 1e-6 && c.start + c.length > start + 1e-6)) return row;
    return MAX_LANE_ROWS - 1;
  };

  const preview = (clip, pattern, box) => {
    const canvas = html("canvas", "pl-preview");
    const w = Math.max(1, Math.round(box.width)), h = ROW_H - 6;
    canvas.width = w;
    canvas.height = h;
    const g = canvas.getContext("2d");
    const notes = pattern.parts.flatMap((p) => (p.mute ? [] : p.notes));
    if (!notes.length) return canvas;
    const lo = Math.min(...notes.map((n) => n.pitch)), hi = Math.max(...notes.map((n) => n.pitch));
    const map = screen.tempoMap, origin = map.toSeconds(clip.start);
    g.fillStyle = "rgba(255,255,255,0.7)";
    for (let rep = 0; rep * pattern.length < clip.length - 1e-6; ++rep)
      for (const n of notes) {
        const at = rep * pattern.length + n.time;
        if (at >= clip.length) continue;
        const x = (map.toSeconds(clip.start + at) - origin) * screen.zoom;
        const y = 4 + (hi === lo ? 0.5 : 1 - (n.pitch - lo) / (hi - lo)) * (h - 10);
        g.fillRect(Math.round(x), Math.round(y), Math.max(1, Math.round(2)), 2);
      }
    return canvas;
  };

  const draw = () => {
    const map = screen.tempoMap, zoom = screen.zoom;
    element.replaceChildren();
    element.style.width = `${width}px`;
    element.style.height = `${rowCount() * ROW_H + 4}px`;
    element.classList.toggle("is-folded", folded());
    for (const clip of clips) {
      const pattern = patternOf(clip.pattern);
      if (!pattern) continue;
      const left = clip.startSeconds * zoom, size = Math.max(6, (clip.endSeconds - clip.startSeconds) * zoom);
      const block = html("div", `pl-block${clip.mute ? " is-muted" : ""}${selected === clip.id ? " is-selected" : ""}`);
      block.dataset.id = clip.id;
      block.tabIndex = 0;
      block.style.cssText = `left:${left}px;width:${size}px;top:${rowTop(clip.row)}px;height:${ROW_H - 2}px;--pat:${patternColour(pattern)}`;
      block.append(preview(clip, pattern, { width: size }), html("span", "pl-name", pattern.name), html("span", "pl-edge"));
      // The seams between repeats of the pattern.
      for (let rep = 1; rep * pattern.length < clip.length - 1e-6; ++rep) {
        const seam = html("i", "pl-seam");
        seam.style.left = `${(map.toSeconds(clip.start + rep * pattern.length) - map.toSeconds(clip.start)) * zoom}px`;
        block.append(seam);
      }
      element.append(block);
    }
  };

  const select = (id) => {
    selected = id;
    screen.selectedPatternClip = id;
    element.querySelectorAll(".pl-block").forEach((b) => b.classList.toggle("is-selected", Number(b.dataset.id) === id));
  };

  const clipOf = (id) => clips.find((c) => c.id === id);

  const duplicate = async (clip) => {
    const row = freeRow(clip.start + clip.length, clip.length, -1);
    const id = await daw("addPatternClip", { pattern: clip.pattern, start: clip.start + clip.length, length: clip.length, row });
    if (typeof id === "number") select(id);
  };

  // Copies of the block back to back up to the loop range's end (whole copies only).
  const repeatsToLoopEnd = (clip) => {
    const loop = screen.session?.loop;
    if (!loop || !(loop.end > loop.start)) return [];
    const end = screen.tempoMap.toBeats(loop.end);
    const starts = [];
    for (let at = clip.start + clip.length; at + clip.length <= end + 1e-6 && starts.length < 256; at += clip.length) starts.push(at);
    return starts;
  };

  const menu = (event, clip) => {
    event.preventDefault();
    select(clip.id);
    const repeats = repeatsToLoopEnd(clip);
    popMenu(event.clientX, event.clientY, ({ item, rule, heading }) => {
      heading(patternOf(clip.pattern)?.name ?? "Pattern");
      item("Edit pattern", () => { screen.selectedPattern = clip.pattern; openPatterns(); });
      item("Make unique", async () => { const id = await daw("makePatternUnique", { id: clip.id }); if (typeof id === "number") screen.selectedPattern = id; },
        { hint: "Copy the pattern for this block only, so editing it doesn't change the other blocks" });
      item("Turn into MIDI clips", () => daw("patternToClips", { id: clip.id }), { hint: "Write the block out as ordinary MIDI clips on the tracks" });
      item(clip.mute ? "Unmute" : "Mute", () => daw("setPatternClip", { id: clip.id, mute: !clip.mute }));
      item("Duplicate", () => duplicate(clip), { shortcut: "Ctrl+D" });
      item("Repeat to the loop end", () => daw("addPatternClips", { pattern: clip.pattern, starts: repeats, length: clip.length }), {
        disabled: !repeats.length, hint: repeats.length ? `${repeats.length} more cop${repeats.length === 1 ? "y" : "ies"} up to the end of the loop range` : "Set a loop range after this block first",
      });
      const others = patterns.filter((p) => p.id !== clip.pattern);
      if (others.length) {
        rule();
        heading("Use another pattern");
        for (const p of others) item(p.name, () => daw("setPatternClip", { id: clip.id, pattern: p.id }), { indent: true });
      }
      rule();
      item("Delete", () => daw("removePatternClip", { id: clip.id }), { danger: true, shortcut: "Delete" });
    });
  };

  const local = (event) => event.clientX - element.getBoundingClientRect().left;

  element.addEventListener("contextmenu", (event) => {
    const block = event.target.closest(".pl-block");
    if (block && screen.tool !== "draw") menu(event, clipOf(Number(block.dataset.id)));
    else event.preventDefault();
  });

  element.addEventListener("dblclick", (event) => {
    const block = event.target.closest(".pl-block");
    const clip = block && clipOf(Number(block.dataset.id));
    if (!clip) return;
    screen.selectedPattern = clip.pattern;
    openPatterns();
  });

  element.addEventListener("keydown", (event) => {
    const clip = clipOf(selected);
    if (!clip) return;
    const mod = event.ctrlKey || event.metaKey;
    if (event.key === "Delete" || event.key === "Backspace") {
      event.preventDefault();
      event.stopPropagation();
      select(null);
      daw("removePatternClip", { id: clip.id });
    } else if (mod && event.key.toLowerCase() === "d") {
      event.preventDefault();
      event.stopPropagation();
      duplicate(clip);
    }
  });

  element.addEventListener("pointerdown", async (event) => {
    if (event.button !== 0) return;
    const blockElement = event.target.closest(".pl-block");
    const map = screen.tempoMap;

    if (!blockElement) {
      if (screen.tool === "draw") return; // the Draw tool paints (drawtool.js)
      // An empty spot: place the selected pattern at the bar.
      const pattern = patterns.find((p) => p.id === screen.selectedPattern) ?? patterns[0];
      if (!pattern) return;
      const start = barsOf(map.toBeats(local(event) / screen.zoom));
      const id = await daw("addPatternClip", { pattern: pattern.id, start, length: pattern.length, row: freeRow(start, pattern.length, -1) });
      if (typeof id === "number") select(id);
      return;
    }

    const clip = clipOf(Number(blockElement.dataset.id));
    if (!clip) return;
    select(clip.id);
    if (event.detail >= 2) return;
    const pattern = patternOf(clip.pattern);
    const resize = event.target.closest(".pl-edge") !== null;
    const startX = local(event);
    const startSeconds = clip.startSeconds, endSeconds = clip.endSeconds;
    let start = clip.start, length = clip.length, moved = false;
    screen.dragging = true;
    blockElement.setPointerCapture?.(event.pointerId);

    const move = (e) => {
      const dx = local(e) - startX;
      if (Math.abs(dx) > 2) moved = true;
      if (!moved) return;
      if (resize) {
        // Whole repeats of the pattern (Alt: free).
        const seconds = endSeconds + dx / screen.zoom;
        const beats = map.toBeats(Math.max(startSeconds, seconds)) - clip.start;
        length = e.altKey ? Math.max(1 / 16, beats) : Math.max(pattern.length, Math.round(beats / pattern.length) * pattern.length);
        blockElement.style.width = `${Math.max(6, (map.toSeconds(clip.start + length) - startSeconds) * screen.zoom)}px`;
      } else {
        start = Math.max(0, map.toBeats(screen.snapTime(startSeconds + dx / screen.zoom, e)));
        blockElement.style.left = `${map.toSeconds(start) * screen.zoom}px`;
      }
    };
    const up = () => {
      blockElement.removeEventListener("pointermove", move);
      blockElement.removeEventListener("pointerup", up);
      blockElement.removeEventListener("pointercancel", up);
      screen.dragging = false;
      if (moved) daw("setPatternClip", { id: clip.id, start, length, row: freeRow(start, length, clip.id) });
      screen.flushSession?.();
    };
    blockElement.addEventListener("pointermove", move);
    blockElement.addEventListener("pointerup", up);
    blockElement.addEventListener("pointercancel", up);
  });

  return {
    element,
    update(session, laneWidth) {
      patterns = session.patterns ?? [];
      clips = session.patternClips ?? [];
      width = laneWidth;
      if (selected != null && !clips.some((c) => c.id === selected)) selected = null;
      if (screen.selectedPatternClip != null && clips.some((c) => c.id === screen.selectedPatternClip)) selected = screen.selectedPatternClip;
      draw();
    },
    rows: () => rowCount(),
    rowHeight: ROW_H,
    // Fold the lane to one row (blocks overlap) or open it to show every row.
    toggleFold() {
      screen.patternLaneFolded = !folded();
      screen.rememberView?.();
      draw();
    },
    // A block at a time in seconds (for the ghost notes' double-click), or null.
    blockAt(seconds) {
      const beats = screen.tempoMap.toBeats(seconds);
      return clips.find((c) => !c.mute && beats >= c.start && beats < c.start + c.length) ?? null;
    },
  };
}

//==================================================================================== ghost notes
// Every MIDI track's lane shows, dimmed in the pattern's colour, the notes the pattern blocks
// play on it: one canvas per block, under the clips, not editable (double-click opens the
// pattern, see drawtool.js). Called after the track rows are drawn.
export function drawGhosts(screen, session) {
  const patterns = new Map((session.patterns ?? []).map((p) => [p.id, p]));
  const map = screen.tempoMap, zoom = screen.zoom;
  for (const row of screen.rows.querySelectorAll(":scope > .track-row[data-track]")) {
    const lane = row.querySelector(":scope > .lane");
    if (!lane) continue;
    lane.querySelectorAll(":scope > .pl-ghost").forEach((g) => g.remove());
    const track = session.tracks.find((t) => t.id === Number(row.dataset.track));
    if (!track || track.kind !== "midi") continue;
    const height = lane.clientHeight || 60;
    for (const clip of session.patternClips ?? []) {
      const pattern = patterns.get(clip.pattern);
      const part = pattern?.parts.find((p) => p.track === track.id);
      if (clip.mute || !part || part.mute || !part.notes?.length) continue;
      const muted = new Set(part.muted ?? []);
      const notes = part.notes.filter((n) => !muted.has(n.pitch) && n.time < pattern.length);
      if (!notes.length) continue;
      const left = clip.startSeconds * zoom, size = Math.max(2, (clip.endSeconds - clip.startSeconds) * zoom);
      if (size > 32000) continue; // past what a canvas can hold
      const canvas = html("canvas", "pl-ghost");
      canvas.width = Math.round(size);
      canvas.height = Math.max(8, height - 4);
      canvas.style.cssText = `left:${left}px;width:${size}px;--pat:${patternColour(pattern)}`;
      canvas.title = `${pattern.name}: plays here (double-click to edit the pattern)`;
      const g = canvas.getContext("2d");
      const lo = Math.min(...notes.map((n) => n.pitch)), hi = Math.max(...notes.map((n) => n.pitch));
      const span = Math.max(12, hi - lo + 1);
      const noteH = Math.max(2, Math.min(6, (canvas.height - 6) / span));
      g.fillStyle = patternColour(pattern);
      const origin = map.toSeconds(clip.start);
      for (let rep = 0; rep * pattern.length < clip.length - 1e-6; ++rep)
        for (const n of notes) {
          const at = rep * pattern.length + n.time;
          if (at >= clip.length) continue;
          const x = (map.toSeconds(clip.start + at) - origin) * zoom;
          const w = Math.max(2, (map.toSeconds(clip.start + Math.min(clip.length, at + n.length)) - origin) * zoom - x - 1);
          const y = 3 + (1 - (n.pitch - lo + 0.5) / span) * (canvas.height - 6) - noteH / 2;
          g.fillRect(Math.round(x), Math.round(y), Math.round(w), Math.round(noteH));
        }
      lane.prepend(canvas);
    }
  }
}
