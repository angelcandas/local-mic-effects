// The Warp tool of the clip editor (ui/clipeditor.js): warp tabs pin a moment of the audio
// to a moment of the song, and the audio stretches between neighbouring tabs.
//
//   hover                         a preview line, on a note start within 8 px
//   click                         adds a tab there; nothing moves
//   press and drag                adds a tab (unless one is under the pointer) and moves that
//                                 moment; only the audio between its neighbouring tabs stretches.
//                                 The clip's edges are pinned the first time
//   drag a handle (ruler lane)    slides the tab along the audio: the audio stays where it is
//                                 at the pointer, the stretch moves
//   Alt-click / double-click      removes a tab; Delete removes the selected ones
//   click a tab, drag a box in    selects; ← / → nudge 1 ms (Shift 10 ms); Tab / Shift+Tab
//     the ruler lane              the next / previous tab; Esc deselects
//   Ctrl/Cmd at the press         flips "Hold neighbours" (pin the note starts either side)
//   Alt while dragging            no snap
//
// Tabs are the clip's warp markers [[source, target], ...] (file seconds, rendered seconds)
// as the session sends them; setWarp stores them (see src/daw/Warp.h). The waveform follows
// the hand (the lane's drag/remap) and, while playing, the edit is heard (livePreview).
import { clamp, html } from "./widgets.js";
import { livePreview, prefs, toSource, toTarget } from "./rhythm.js";

const MAX_STRETCH = 7.9; // the engine drops a stretch over 8x (warp::Map)
const GRAB_PX = 6, SNAP_PX = 8;

export function createWarpTool(ctx) {
  const { screen, daw, RULER } = ctx;
  let selected = []; // sources (file seconds) of the selected tabs of the active lane
  let shown = false;
  let nudged = null, nudgeTimer = 0; // tabs being nudged by keys, stored after a pause
  let busy = false; // a drag in progress

  //================================================================ the section's body
  const body = html("div", "warp-tool");
  const count = html("p", "panel-caption warp-tool-count");
  const row = html("div", "panel-actions");
  const button = (text, title, action) => {
    const b = html("button", "tool", text);
    b.type = "button";
    b.title = title;
    b.addEventListener("click", action);
    return b;
  };
  const atHits = button("Tabs at hitpoints", "Pin every note start where it plays now, ready to drag", () => tabsAtHitpoints());
  const reset = button("Reset", "Remove every warp tab: the clip's timing as recorded (a musical clip keeps following the tempo)", () => {
    selected = [];
    daw("clearWarp", { clips: [ctx.active().clip.id] });
  });
  row.append(atHits, reset);
  const hold = html("label", "rhythm-check");
  const holdBox = html("input");
  Object.assign(holdBox, { type: "checkbox", checked: prefs.get("holdNeighbours", false) });
  holdBox.addEventListener("change", () => prefs.set("holdNeighbours", holdBox.checked));
  hold.append(holdBox, document.createTextNode(" Hold neighbours"));
  hold.title = "Dragging a note start pins the note starts either side, so only that note moves (Ctrl/Cmd at the press flips it for one drag)";
  const bypassNote = html("p", "panel-caption warp-tool-note", "Compare Time is on: you hear the clip without its tabs. Switch it off to edit them.");
  body.append(count, row, hold, bypassNote);

  //================================================================ helpers
  const markersOf = (lane) => (nudged && lane === ctx.active() ? nudged : lane.clip.warp ?? []).map((m) => [...m]);
  const isSelected = (source) => selected.some((s) => Math.abs(s - source) < 1e-6);
  const perPx = () => ctx.perPx();
  const pointerY = (event) => event.clientY - ctx.wave.getBoundingClientRect().top;

  // The tab under the pointer (by where it plays), within GRAB_PX.
  const tabAt = (lane, markers, renderedTime) => {
    let best = -1, distance = GRAB_PX * perPx();
    markers.forEach(([, t], i) => { const d = Math.abs(t - renderedTime); if (d < distance) { distance = d; best = i; } });
    return best;
  };
  // A note start within SNAP_PX of a moment (rendered time), or -1.
  const hitAt = (lane, renderedTime) => {
    let best = -1, distance = SNAP_PX * perPx();
    lane.hits.forEach(([t], i) => { const d = Math.abs(t - renderedTime); if (d < distance) { distance = d; best = i; } });
    return best;
  };
  // Where a new tab goes: on a note start nearby (unless Alt), or right at the pointer.
  const newTabTime = (lane, renderedTime, event) => {
    if (event.altKey) return { time: renderedTime, hit: -1 };
    const hit = hitAt(lane, renderedTime);
    return hit >= 0 ? { time: lane.hits[hit][0], hit } : { time: renderedTime, hit: -1 };
  };

  // How far tab i can go (rendered time) and keep both stretches either side within 8x.
  const bounds = (markers, i, source = markers[i][0]) => {
    let low = -Infinity, high = Infinity;
    if (i > 0) {
      const [ps, pt] = markers[i - 1];
      low = Math.max(low, pt + Math.max(0.01, (source - ps) / MAX_STRETCH));
      high = Math.min(high, pt + (source - ps) * MAX_STRETCH);
    }
    if (i < markers.length - 1) {
      const [ns, nt] = markers[i + 1];
      high = Math.min(high, nt - Math.max(0.01, (ns - source) / MAX_STRETCH));
      low = Math.max(low, nt - (ns - source) * MAX_STRETCH);
    }
    return [low, high];
  };

  // The clip's edges pinned where they play, the first time a tab goes in.
  const withEdges = (lane, markers) => {
    if (markers.length) return markers;
    const c = lane.clip, map = lane.map();
    return [[toSource(map, c.offset), c.offset], [toSource(map, c.offset + c.length), c.offset + c.length]];
  };
  // Adds a tab where `time` plays now (unless one is within 3 px); returns its index.
  const addTab = (lane, markers, time) => {
    const near = markers.findIndex(([, t]) => Math.abs(t - time) < 3 * perPx());
    if (near >= 0) return near;
    const source = toSource(lane.map(), time);
    markers.push([source, time]);
    markers.sort((x, y) => x[0] - y[0]);
    return markers.findIndex(([s]) => s === source);
  };

  const store = (lane, markers) => {
    daw("setWarp", { id: lane.clip.id, markers: markers.length > 1 ? markers : [] });
    screen.flushSession?.();
  };
  // The picture of an edit, before it renders: new rendered time -> where it was.
  const pictureOf = (before, after) => (t) => toTarget(before, toSource(after, t));

  const removeTabs = (lane, indices) => {
    const markers = markersOf(lane).filter((_, i) => !indices.includes(i));
    selected = [];
    const keep = markers.length > 1 ? markers : [];
    lane.settle(pictureOf(lane.map(), keep.length ? keep : []));
    store(lane, keep);
  };

  //================================================================ gestures
  const pointerdown = (event) => {
    const lane = ctx.laneAt(event.clientY) ?? ctx.active();
    if (lane.clip.timeBypass) { ctx.toast("Compare Time is on: switch it off to edit the warp tabs."); return true; }
    flushNudge();
    const inRuler = pointerY(event) < RULER;
    const time = lane.toRendered(ctx.timeAt(event.clientX));
    const markers = markersOf(lane);
    const index = tabAt(lane, markers, time);

    if (index >= 0 && (event.altKey || event.detail >= 2)) { removeTabs(lane, [index]); return true; }
    if (inRuler) {
      if (index >= 0) slideHandle(event, lane, markers, index);
      else selectBox(event, lane);
      return true;
    }
    press(event, lane, markers, index, time);
    return true;
  };

  // In the waveform: a click adds (or selects) a tab, a drag moves a moment of the audio.
  const press = (event, lane, markers, index, time) => {
    let dragging = false;
    const move = (e) => {
      if (!dragging && Math.abs(e.clientX - event.clientX) < 3) return;
      if (!dragging) {
        dragging = true;
        cleanup();
        moveTab(event, e, lane, markers, index, time);
      }
    };
    const up = () => {
      cleanup();
      if (dragging) return;
      if (index >= 0) {
        // Select it (Shift / Ctrl adds to the selection).
        const source = markers[index][0];
        selected = event.shiftKey || event.ctrlKey || event.metaKey ? (isSelected(source) ? selected.filter((s) => Math.abs(s - source) > 1e-6) : [...selected, source]) : [source];
        ctx.redraw();
        return;
      }
      // A new tab where the audio plays now: nothing moves.
      const { time: at } = newTabTime(lane, time, event);
      const next = withEdges(lane, markers);
      const added = addTab(lane, next, at);
      selected = [next[added][0]];
      store(lane, next);
    };
    const cleanup = () => {
      ctx.wave.removeEventListener("pointermove", move);
      ctx.wave.removeEventListener("pointerup", up);
      ctx.wave.removeEventListener("pointercancel", up);
    };
    ctx.wave.setPointerCapture?.(event.pointerId);
    ctx.wave.addEventListener("pointermove", move);
    ctx.wave.addEventListener("pointerup", up);
    ctx.wave.addEventListener("pointercancel", up);
  };

  // Drags a moment of the audio (an existing tab, or one made at the press).
  const moveTab = (event, firstMove, lane, original, grabbed, time) => {
    const before = lane.map();
    let markers = original.map((m) => [...m]);
    let index = grabbed;
    if (index < 0) {
      markers = withEdges(lane, markers);
      const { time: at, hit } = newTabTime(lane, time, event);
      const holdOn = holdBox.checked !== (event.ctrlKey || event.metaKey);
      if (holdOn && hit >= 0) {
        // Hold neighbours: the note starts either side stay where they are.
        for (const other of [hit - 1, hit + 1]) if (lane.hits[other]) addTab(lane, markers, lane.hits[other][0]);
      }
      index = addTab(lane, markers, at);
    }
    const source = markers[index][0], startTarget = markers[index][1];
    selected = [source];
    const [low, high] = bounds(markers, index);
    const live = livePreview(daw, screen, "previewWarp", lane.clip.id);
    const line = html("div", "warp-tab is-dragging");
    ctx.layer.append(line);
    let target = startTarget;
    const withTarget = (t) => markers.map((m, i) => (i === index ? [m[0], t] : m));
    busy = true;
    screen.dragging = true;

    const place = () => {
      Object.assign(line.style, { left: `${ctx.xOf(lane.toTimeline(target))}px`, top: `${RULER + lane.top}px`, height: `${lane.height}px` });
    };
    const move = (e) => {
      const free = startTarget + (e.clientX - event.clientX) * perPx();
      const snapped = lane.toRendered(screen.snapTime(lane.toTimeline(free), e));
      target = clamp(snapped, low, high);
      place();
      lane.setDrag(Math.abs(target - startTarget) > 1e-4 ? pictureOf(before, withTarget(target)) : null);
      live.send({ markers: withTarget(target) });
      ctx.paintSoon();
    };
    const up = () => {
      ctx.wave.removeEventListener("pointermove", move);
      ctx.wave.removeEventListener("pointerup", up);
      ctx.wave.removeEventListener("pointercancel", up);
      busy = false;
      screen.dragging = false;
      line.remove();
      lane.setDrag(null);
      const changed = Math.abs(target - startTarget) > 1e-4 || markers.length !== original.length;
      live.end(changed);
      if (changed) {
        const next = withTarget(target);
        lane.settle(pictureOf(before, next)); // keep the predicted picture until the real one is rendered
        store(lane, next);
      }
      ctx.paintSoon();
    };
    ctx.wave.addEventListener("pointermove", move);
    ctx.wave.addEventListener("pointerup", up);
    ctx.wave.addEventListener("pointercancel", up);
    move(firstMove);
  };

  // A handle in the ruler lane: the tab slides along the audio, and the audio under the
  // pointer stays where it plays (the new tab sits on the map as it was).
  const slideHandle = (event, lane, original, index) => {
    const before = lane.map();
    const markers = original.map((m) => [...m]);
    const [, startTarget] = markers[index];
    const low = index > 0 ? markers[index - 1][1] + 0.01 : -Infinity;
    const high = index < markers.length - 1 ? markers[index + 1][1] - 0.01 : Infinity;
    const live = livePreview(daw, screen, "previewWarp", lane.clip.id);
    let target = startTarget, moved = false;
    const next = () => markers.map((m, i) => (i === index ? [toSource(before, target), target] : m));
    busy = true;
    screen.dragging = true;
    const move = (e) => {
      if (!moved && Math.abs(e.clientX - event.clientX) < 3) return;
      moved = true;
      let t = lane.toRendered(ctx.timeAt(e.clientX));
      if (!e.altKey) { const hit = hitAt(lane, t); if (hit >= 0) t = lane.hits[hit][0]; }
      target = clamp(t, low, high);
      nudged = next();
      lane.setDrag(pictureOf(before, nudged));
      live.send({ markers: nudged });
      ctx.redraw();
      ctx.paintSoon();
    };
    const up = () => {
      ctx.wave.removeEventListener("pointermove", move);
      ctx.wave.removeEventListener("pointerup", up);
      ctx.wave.removeEventListener("pointercancel", up);
      busy = false;
      screen.dragging = false;
      nudged = null;
      lane.setDrag(null);
      const changed = moved && Math.abs(target - startTarget) > 1e-4;
      live.end(changed);
      if (changed) {
        const result = next();
        selected = [result[index][0]];
        lane.settle(pictureOf(before, result));
        store(lane, result);
      } else if (!moved) {
        const source = markers[index][0];
        selected = event.shiftKey || event.ctrlKey || event.metaKey ? [...selected, source] : [source];
      }
      ctx.redraw();
      ctx.paintSoon();
    };
    ctx.wave.setPointerCapture?.(event.pointerId);
    ctx.wave.addEventListener("pointermove", move);
    ctx.wave.addEventListener("pointerup", up);
    ctx.wave.addEventListener("pointercancel", up);
  };

  // A box dragged in the ruler lane selects the tabs inside it; a click there locates.
  const selectBox = (event, lane) => {
    const from = ctx.timeAt(event.clientX);
    let moved = false;
    const box = html("div", "clip-editor-range is-select");
    const move = (e) => {
      if (!moved && Math.abs(e.clientX - event.clientX) < 3) return;
      moved = true;
      if (!box.isConnected) ctx.layer.append(box);
      const x0 = ctx.xOf(from), x1 = ctx.xOf(ctx.timeAt(e.clientX));
      Object.assign(box.style, { left: `${Math.min(x0, x1)}px`, width: `${Math.abs(x1 - x0)}px` });
    };
    const up = (e) => {
      ctx.wave.removeEventListener("pointermove", move);
      ctx.wave.removeEventListener("pointerup", up);
      ctx.wave.removeEventListener("pointercancel", up);
      box.remove();
      if (!moved) {
        selected = [];
        daw("transport", { action: "seek", time: Math.max(0, ctx.timeAt(e.clientX)) });
        ctx.redraw();
        return;
      }
      const to = ctx.timeAt(e.clientX);
      const [lo, hi] = [Math.min(from, to), Math.max(from, to)];
      const inside = markersOf(lane).filter(([, t]) => lane.toTimeline(t) >= lo && lane.toTimeline(t) <= hi).map(([s]) => s);
      selected = e.shiftKey ? [...selected, ...inside] : inside;
      ctx.redraw();
    };
    ctx.wave.setPointerCapture?.(event.pointerId);
    ctx.wave.addEventListener("pointermove", move);
    ctx.wave.addEventListener("pointerup", up);
    ctx.wave.addEventListener("pointercancel", up);
  };

  //================================================================ hover
  const hoverLine = html("div", "warp-hover");
  const hover = (event) => {
    if (busy) return;
    const lane = ctx.laneAt(event.clientY) ?? ctx.active();
    if (lane.clip.timeBypass) return leave();
    const time = lane.toRendered(ctx.timeAt(event.clientX));
    const markers = markersOf(lane);
    const onTab = tabAt(lane, markers, time) >= 0;
    const inRuler = pointerY(event) < RULER;
    ctx.wave.classList.toggle("is-on-tab", onTab);
    ctx.wave.classList.toggle("is-in-ruler", inRuler && !onTab);
    if (onTab || inRuler) { hoverLine.remove(); return; }
    const { time: at, hit } = newTabTime(lane, time, event);
    Object.assign(hoverLine.style, { left: `${ctx.xOf(lane.toTimeline(at))}px`, top: `${RULER + lane.top}px`, height: `${lane.height}px` });
    hoverLine.classList.toggle("is-on-hit", hit >= 0);
    if (!hoverLine.isConnected) ctx.layer.append(hoverLine);
  };
  const leave = () => {
    hoverLine.remove();
    ctx.wave.classList.remove("is-on-tab", "is-in-ruler");
  };

  //================================================================ keys
  const flushNudge = () => {
    if (!nudged) return;
    clearTimeout(nudgeTimer);
    const lane = ctx.active(), markers = nudged;
    nudged = null;
    lane.settle(pictureOf(lane.map(), markers));
    store(lane, markers);
  };
  const keydown = (event) => {
    const lane = ctx.active();
    const markers = markersOf(lane);
    const picked = markers.map(([s], i) => (isSelected(s) ? i : -1)).filter((i) => i >= 0);
    const mod = event.ctrlKey || event.metaKey;
    if ((event.key === "Delete" || event.key === "Backspace") && picked.length) {
      flushNudge();
      removeTabs(lane, picked);
      return true;
    }
    if ((event.key === "ArrowLeft" || event.key === "ArrowRight") && picked.length && !mod && !event.altKey) {
      if (lane.clip.timeBypass) return true;
      const step = (event.key === "ArrowLeft" ? -1 : 1) * (event.shiftKey ? 0.01 : 0.001);
      const next = markers.map((m) => [...m]);
      for (const i of picked) {
        const [low, high] = bounds(next, i);
        next[i][1] = clamp(next[i][1] + step, low, high);
      }
      const before = lane.map();
      nudged = next;
      lane.setDrag(pictureOf(before, next));
      ctx.redraw();
      ctx.paintSoon();
      clearTimeout(nudgeTimer);
      nudgeTimer = setTimeout(() => { lane.setDrag(null); flushNudge(); }, 250); // one undo step for a run of presses
      return true;
    }
    if (event.key === "Tab" && markers.length && !mod) {
      const current = picked.length ? picked[picked.length - 1] : (event.shiftKey ? markers.length : -1);
      const next = clamp(current + (event.shiftKey ? -1 : 1), 0, markers.length - 1);
      selected = [markers[next][0]];
      ctx.redraw();
      return true;
    }
    if (event.key === "Escape" && selected.length) {
      selected = [];
      ctx.redraw();
      return true;
    }
    return false;
  };

  // Pins every note start (as shown) where it plays now.
  const tabsAtHitpoints = () => {
    const lane = ctx.active();
    if (lane.clip.timeBypass || !lane.hits.length) return;
    const markers = withEdges(lane, markersOf(lane));
    const c = lane.clip;
    for (const [t] of lane.hits) if (t > c.offset + 0.005 && t < c.offset + c.length - 0.005) addTab(lane, markers, t);
    store(lane, markers);
  };

  //================================================================ drawing
  // Tabs: a line through the lane and a handle in the ruler lane, with the stretch factor of
  // the audio before it ("1.25": stretched, "0.80": squeezed).
  const factorClass = (f) => (f < 0.5 || f > 2 ? "is-extreme" : f < 0.67 || f > 1.5 ? "is-far" : "");
  const draw = (visible) => {
    shown = visible;
    ctx.layer.querySelectorAll(".warp-tab:not(.is-dragging)").forEach((e) => e.remove());
    const w = ctx.width();
    for (const lane of ctx.lanes) {
      const markers = markersOf(lane);
      const free = lane.clip.freeWarp; // musical: the factor is of the free warp alone, not the tempo bend
      markers.forEach(([s, t], i) => {
        const x = ctx.xOf(lane.toTimeline(t));
        if (x < -40 || x > w + 40) return;
        const tab = html("div", "warp-tab");
        if (isSelected(s) && lane === ctx.active()) tab.classList.add("is-selected");
        if (!visible) tab.classList.add("is-quiet");
        Object.assign(tab.style, { left: `${x}px`, top: `${lane === ctx.lanes[0] ? 0 : RULER + lane.top}px`, height: `${(lane === ctx.lanes[0] ? RULER : 0) + lane.top + lane.height}px` });
        if (visible && i > 0) {
          const [ps, pt] = markers[i - 1];
          const f = free?.[i] && free?.[i - 1] && free.length === markers.length
            ? (free[i][1] - free[i - 1][1]) / Math.max(1e-6, s - ps) : (t - pt) / Math.max(1e-6, s - ps);
          const room = x - ctx.xOf(lane.toTimeline(pt));
          if (Math.abs(f - 1) > 0.005 && room > 36) {
            const label = html("span", `warp-factor ${factorClass(f)}`.trim(), f.toFixed(2));
            label.title = f > 1 ? "The audio before this tab is stretched (slower)" : "The audio before this tab is squeezed (faster)";
            tab.append(label);
          }
        }
        ctx.layer.append(tab);
      });
    }
  };

  const update = () => {
    const lane = ctx.active(), c = lane.clip;
    const n = c.warp?.length ?? 0;
    count.textContent = n ? `${n} warp tabs${selected.length ? `, ${selected.length} selected` : ""}.` : "No warp tabs: the clip plays as recorded.";
    reset.disabled = !n;
    atHits.disabled = !lane.hits.length || Boolean(c.timeBypass);
    bypassNote.hidden = !c.timeBypass;
    selected = selected.filter((s) => (c.warp ?? []).some(([x]) => Math.abs(x - s) < 1e-6));
  };

  const hint = () => (ctx.active().clip.timeBypass
    ? "Compare Time is on: the clip plays without its warp tabs. Switch it off to edit them."
    : "Click to pin a tab, drag to move the audio there (it stretches between the tabs either side). Drag a handle on the ruler to slide a tab along the audio. Alt-click or double-click removes one.");

  return { body, pointerdown, hover, leave, keydown, draw, update, hint, get shown() { return shown; }, dispose: () => clearTimeout(nudgeTimer) };
}
