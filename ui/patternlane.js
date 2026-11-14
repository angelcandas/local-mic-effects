// The Patterns lane under the ruler (blocks that place a pattern on the song) and the ghost
// notes each pattern block shows on the MIDI tracks it plays. The rack (the Patterns tab) is
// in patterns.js; the Draw tool's strokes are in drawtool.js.
//
// Blocks are in BEATS (start, length); the lane draws them through the tempo map.
import { html } from "./widgets.js";
import { patternColour } from "./patterns.js";

const ROW_H = 30;
const MAX_LANE_ROWS = 16; // Edit::maxPatternRows
const MIN_BLOCK = 0.25;   // Edit::minPatternBeats

// Calls fn(note, at) for each note a block plays, `at` in beats from the block's start: the
// block starts `offset` beats into its pattern (a left trim) and repeats it on its grid.
export function blockHits(clip, pattern, notes, fn) {
  const size = pattern.length;
  if (!(size > 0)) return;
  const offset = ((clip.offset || 0) % size + size) % size;
  for (let pass = -offset, n = 0; pass < clip.length - 1e-6 && n < 4096; pass += size, ++n)
    for (const note of notes) {
      const at = pass + note.time;
      if (at > -1e-6 && at < clip.length - 1e-6) fn(note, Math.max(0, at));
    }
}

// Where the repeats of the pattern start inside a block, in beats from its start.
const seamsOf = (clip, pattern) => {
  const size = pattern.length, seams = [];
  if (!(size > 0)) return seams;
  const offset = ((clip.offset || 0) % size + size) % size;
  for (let at = size - offset; at < clip.length - 1e-6 && seams.length < 512; at += size) if (at > 1e-6) seams.push(at);
  return seams;
};

// The Patterns lane. screen: zoom, snap, snapTime, tempoMap, beatsPerBar, session, dragging,
// selectedPattern, tool, patternLaneFolded.
export function createPatternLane(screen, daw, { popMenu, openPatterns }) {
  const element = html("div", "pattern-lane");
  element.title = "Patterns: click an empty spot to place the selected pattern · drag to move (Ctrl: copy) · drag an edge to trim or repeat · Shift+click to pick several";
  element.tabIndex = -1;
  let width = 0, clips = [], patterns = [];
  let selected = null;          // the block clicked last
  const picked = new Set();     // the blocks picked together (selected among them)
  let active = false;           // the last click in the arrangement was in this lane (its keys apply)

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
    blockHits(clip, pattern, notes, (n, at) => {
      const x = (map.toSeconds(clip.start + at) - origin) * screen.zoom;
      const y = 4 + (hi === lo ? 0.5 : 1 - (n.pitch - lo) / (hi - lo)) * (h - 10);
      g.fillRect(Math.round(x), Math.round(y), 2, 2);
    });
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
      const block = html("div", `pl-block${clip.mute ? " is-muted" : ""}${isPicked(clip.id) ? " is-selected" : ""}`);
      block.dataset.id = clip.id;
      block.tabIndex = 0;
      block.style.cssText = `left:${left}px;width:${size}px;top:${rowTop(clip.row)}px;height:${ROW_H - 2}px;--pat:${patternColour(pattern)}`;
      block.append(preview(clip, pattern, { width: size }), html("span", "pl-name", pattern.name), html("span", "pl-edge-left"), html("span", "pl-edge"));
      // The seams between repeats of the pattern.
      for (const at of seamsOf(clip, pattern)) {
        const seam = html("i", "pl-seam");
        seam.style.left = `${(map.toSeconds(clip.start + at) - map.toSeconds(clip.start)) * zoom}px`;
        block.append(seam);
      }
      element.append(block);
    }
  };

  function isPicked(id) { return id === selected || picked.has(id); }
  const mark = () => element.querySelectorAll(".pl-block").forEach((b) => b.classList.toggle("is-selected", isPicked(Number(b.dataset.id))));

  // Picks one block (or none); `add`: adds it to the picked ones, or takes it out.
  const select = (id, add = false) => {
    if (add && id != null) {
      if (selected != null) picked.add(selected);
      if (picked.has(id)) { picked.delete(id); id = [...picked].pop() ?? null; }
      else picked.add(id);
      if (picked.size <= 1) picked.clear();
    } else picked.clear();
    selected = id;
    screen.selectedPatternClip = id;
    mark();
  };
  const selectMany = (ids) => {
    picked.clear();
    ids.forEach((id) => picked.add(id));
    selected = ids[ids.length - 1] ?? null;
    if (picked.size <= 1) picked.clear();
    screen.selectedPatternClip = selected;
    mark();
  };

  const clipOf = (id) => clips.find((c) => c.id === id);
  // The blocks an edit applies to: the picked ones, or the selected one.
  const chosen = () => (picked.size ? [...picked] : selected != null ? [selected] : []).map(clipOf).filter(Boolean);
  const each = (items, run) => (screen.eachStep ? screen.eachStep(items, run) : items.reduce((p, x, i) => p.then(() => run(x, i > 0)), Promise.resolve()));

  // New blocks like `items` (copies), `delta` beats later, one undo step; picks them.
  const copyBlocks = async (items, delta) => {
    const made = [];
    const taken = clips.slice();
    await each(items, async (c, more) => {
      const start = Math.max(0, c.start + delta);
      let row = c.row;
      if (taken.some((o) => o.row === row && o.start < start + c.length - 1e-6 && o.start + o.length > start + 1e-6))
        for (row = 0; row < MAX_LANE_ROWS - 1 && taken.some((o) => o.row === row && o.start < start + c.length - 1e-6 && o.start + o.length > start + 1e-6); ++row);
      taken.push({ ...c, start, row });
      const id = await daw("addPatternClip", { pattern: c.pattern, start, length: c.length, row, ...(c.offset ? { offset: c.offset } : {}), ...(c.mute ? { mute: true } : {}), ...(more ? { continue: true } : {}) });
      if (typeof id === "number") made.push(id);
    });
    if (made.length) selectMany(made);
  };

  // Ctrl+D: the picked blocks again, right after the last of them ends.
  const duplicate = (items = chosen()) => {
    if (!items.length) return;
    const first = Math.min(...items.map((c) => c.start)), end = Math.max(...items.map((c) => c.start + c.length));
    return copyBlocks(items, end - first);
  };
  const removeBlocks = (items = chosen()) => {
    if (!items.length) return;
    select(null);
    return items.length === 1 ? daw("removePatternClip", { id: items[0].id }) : daw("removePatternClips", { ids: items.map((c) => c.id) });
  };
  const copy = (cut) => {
    const items = chosen();
    if (!items.length) return false;
    const first = Math.min(...items.map((c) => c.start));
    screen.blockClipboard = items.map((c) => ({ pattern: c.pattern, start: c.start - first, length: c.length, row: c.row, offset: c.offset || 0, mute: c.mute }));
    if (cut) removeBlocks(items);
    return true;
  };
  // Ctrl+V: at the playhead (the copied blocks keep their spacing).
  const paste = () => {
    const board = screen.blockClipboard;
    if (!board?.length) return false;
    const at = screen.tempoMap.toBeats(Math.max(0, screen.status?.position ?? 0));
    copyBlocks(board.filter((c) => patternOf(c.pattern)).map((c) => ({ ...c, start: c.start + at })), 0);
    return true;
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
    if (!isPicked(clip.id)) select(clip.id);
    const group = chosen();
    const repeats = repeatsToLoopEnd(clip);
    popMenu(event.clientX, event.clientY, ({ item, rule, heading }) => {
      heading(patternOf(clip.pattern)?.name ?? "Pattern");
      item("Edit pattern", () => { screen.selectedPattern = clip.pattern; openPatterns(); });
      item("Make unique", async () => { const id = await daw("makePatternUnique", { id: clip.id }); if (typeof id === "number") screen.selectedPattern = id; },
        { hint: "Copy the pattern for this block only, so editing it doesn't change the other blocks" });
      item("Turn into MIDI clips", () => daw("patternToClips", { id: clip.id }), { hint: "Write the block out as ordinary MIDI clips on the tracks" });
      item(clip.mute ? "Unmute" : "Mute", () => daw("setPatternClip", { id: clip.id, mute: !clip.mute }));
      item(group.length > 1 ? `Duplicate ${group.length} blocks` : "Duplicate", () => duplicate(group), { shortcut: "Ctrl+D" });
      item("Copy", () => copy(false), { shortcut: "Ctrl+C" });
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
      item(group.length > 1 ? `Delete ${group.length} blocks` : "Delete", () => removeBlocks(group), { danger: true, shortcut: "Delete" });
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

  // The lane's keys apply while the last click in the arrangement was in it.
  document.addEventListener("pointerdown", (event) => {
    if (element.contains(event.target)) { active = true; return; }
    if (screen.arrange?.contains(event.target)) {
      active = false;
      if (selected != null || picked.size) select(null);
    }
  }, true);

  // Keys: Delete, Ctrl+D, Ctrl+C / X / V, Ctrl+A (blocks). True when handled.
  const onKey = (event) => {
    if (!active) return false;
    const mod = event.ctrlKey || event.metaKey;
    const key = event.key.toLowerCase();
    const done = () => { event.preventDefault(); event.stopPropagation(); return true; };
    if (mod && !event.shiftKey && !event.altKey) {
      if (key === "v") return paste() ? done() : false;
      if (key === "a" && clips.length) { selectMany(clips.map((c) => c.id)); return done(); }
    }
    if (!chosen().length) return false;
    if (!mod && (event.key === "Delete" || event.key === "Backspace")) { removeBlocks(); return done(); }
    if (mod && !event.shiftKey && !event.altKey) {
      if (key === "d") { duplicate(); return done(); }
      if (key === "c") { copy(false); return done(); }
      if (key === "x") { copy(true); return done(); }
    }
    if (event.key === "Escape") { select(null); return done(); }
    return false;
  };

  element.addEventListener("pointerdown", async (event) => {
    if (event.button !== 0) return;
    const blockElement = event.target.closest(".pl-block");
    const map = screen.tempoMap;

    if (!blockElement) {
      if (screen.tool !== "select") return; // the Draw tool paints (drawtool.js); others don't place
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
    const pattern = patternOf(clip.pattern);
    if (!pattern) return;

    // The Cut tool: cut the block where the click is (on the grid).
    if (screen.tool === "cut") {
      event.preventDefault();
      const at = map.toBeats(screen.snapTime(local(event) / screen.zoom, event));
      if (at > clip.start + 1e-3 && at < clip.start + clip.length - 1e-3) daw("splitPatternClip", { id: clip.id, at });
      return;
    }
    // Shift+click: add the block to the picked ones, or take it out.
    if (event.shiftKey) { event.preventDefault(); return select(clip.id, true); }
    const ctrl = event.ctrlKey || event.metaKey; // Ctrl+click adds too; Ctrl+drag copies
    if (!ctrl && !isPicked(clip.id)) select(clip.id);
    else if (!ctrl) { selected = clip.id; screen.selectedPatternClip = clip.id; }
    if (event.detail >= 2) return;

    const mode = event.target.closest(".pl-edge") ? "end" : event.target.closest(".pl-edge-left") ? "start" : "move";
    // Moving one picked block moves them all (each keeps its row and spacing).
    const group = mode === "move" && isPicked(clip.id) && picked.size > 1 ? chosen() : [clip];
    const earliest = Math.min(...group.map((c) => c.start));
    const startX = local(event);
    const startSeconds = clip.startSeconds, endSeconds = clip.endSeconds, end = clip.start + clip.length;
    let start = clip.start, length = clip.length, offset = clip.offset || 0, moved = false, copying = ctrl;
    const elements = new Map(group.map((c) => [c.id, element.querySelector(`.pl-block[data-id="${c.id}"]`)]));
    screen.dragging = true;
    blockElement.setPointerCapture?.(event.pointerId);

    const move = (e) => {
      const dx = local(e) - startX;
      if (Math.abs(dx) > 2) moved = true;
      if (!moved) return;
      if (mode === "end") {
        // Whole repeats of the pattern (Alt: free).
        const seconds = endSeconds + dx / screen.zoom;
        const beats = map.toBeats(Math.max(startSeconds, seconds)) - clip.start;
        length = e.altKey ? Math.max(1 / 16, beats) : Math.max(pattern.length, Math.round(beats / pattern.length) * pattern.length);
        blockElement.style.width = `${Math.max(6, (map.toSeconds(clip.start + length) - startSeconds) * screen.zoom)}px`;
      } else if (mode === "start") {
        // Trim from the left: the block starts later (or earlier) in its pattern.
        start = Math.min(end - MIN_BLOCK, Math.max(0, map.toBeats(screen.snapTime(startSeconds + dx / screen.zoom, e))));
        length = end - start;
        offset = (clip.offset || 0) + (start - clip.start);
        blockElement.style.left = `${map.toSeconds(start) * screen.zoom}px`;
        blockElement.style.width = `${Math.max(6, (map.toSeconds(end) - map.toSeconds(start)) * screen.zoom)}px`;
      } else {
        copying = e.ctrlKey || e.metaKey;
        start = Math.max(clip.start - earliest, map.toBeats(screen.snapTime(startSeconds + dx / screen.zoom, e)));
        const delta = start - clip.start;
        for (const c of group) {
          const el = elements.get(c.id);
          if (!el) continue;
          el.style.left = `${map.toSeconds(c.start + delta) * screen.zoom}px`;
          el.classList.toggle("is-copying", copying);
        }
      }
    };
    const up = async () => {
      blockElement.removeEventListener("pointermove", move);
      blockElement.removeEventListener("pointerup", up);
      blockElement.removeEventListener("pointercancel", up);
      screen.dragging = false;
      if (!moved && ctrl) select(clip.id, true);
      else if (moved && mode === "move" && copying) {
        elements.forEach((el) => el?.classList.remove("is-copying"));
        draw(); // the originals stay where they were
        await copyBlocks(group, start - clip.start);
      } else if (moved && mode === "move" && group.length > 1) {
        const delta = start - clip.start;
        await each(group, (c, more) => daw("setPatternClip", { id: c.id, start: c.start + delta, ...(more ? { continue: true } : {}) }));
      } else if (moved) {
        daw("setPatternClip", { id: clip.id, start, length, row: freeRow(start, length, clip.id), ...(mode === "start" ? { offset } : {}) });
      }
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
      for (const id of [...picked]) if (!clips.some((c) => c.id === id)) picked.delete(id);
      if (picked.size <= 1) picked.clear();
      if (screen.selectedPatternClip != null && clips.some((c) => c.id === screen.selectedPatternClip)) selected = screen.selectedPatternClip;
      draw();
    },
    rows: () => rowCount(),
    onKey,
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
      blockHits(clip, pattern, notes, (n, at) => {
        const x = (map.toSeconds(clip.start + at) - origin) * zoom;
        const w = Math.max(2, (map.toSeconds(clip.start + Math.min(clip.length, at + n.length)) - origin) * zoom - x - 1);
        const y = 3 + (1 - (n.pitch - lo + 0.5) / span) * (canvas.height - 6) - noteH / 2;
        g.fillRect(Math.round(x), Math.round(y), Math.round(w), Math.round(noteH));
      });
      lane.prepend(canvas);
    }
  }
}
