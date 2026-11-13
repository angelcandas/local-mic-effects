// The Hitpoints tool of the clip editor (ui/clipeditor.js): the note starts found in the
// audio, which Quantize, the Warp tool's snapping and every Create button use.
//
//   Threshold, Min length, Beats   which of the found note starts count (stored with the clip)
//   click between two             hear that slice; Tab / Shift+Tab the next / previous one
//   click a hitpoint              lock it (kept whatever the filter says) / unlock it
//   Shift-click                   disable it (grey: ignored by everything) / enable it again
//   Alt-click                     add one there
//   drag a hitpoint               move it (it becomes a locked one where it's let go)
//   Ctrl/Cmd+Alt-click            back to automatic
//
// Hand edits are stored in file time, so they stay on the audio when it's warped. The lane's
// hits are [rendered time, strength, state, file time, peak] as the bridge's `hitpoints`
// answer sends them (state: auto | added | locked | disabled).
import { clamp, html } from "./widgets.js";
import { prefs, toSource } from "./rhythm.js";

const GRAB_PX = 6;
const BEATS = [[0, "Off"], [1, "1/4"], [0.5, "1/8"], [0.25, "1/16"], [0.125, "1/32"]];

export function createHitpointTool(ctx) {
  const { daw, RULER, ValueKnob, button, toast } = ctx;
  let shown = false;
  let slice = -1; // the slice last heard: it starts at enabled hit `slice` of the active lane
  let dragging = null; // { from: file time, to: rendered time } while a hitpoint is dragged

  //================================================================ the section's body
  const body = html("div", "hit-tool");
  const count = html("p", "panel-caption hit-tool-count");
  let filter = { threshold: 0.5, minLength: 0.05, beats: 0 };
  let filterTimer = 0, editingUntil = 0;
  const sendFilter = () => {
    clearTimeout(filterTimer);
    editingUntil = Date.now() + 1500; // the knobs don't jump back while the change travels
    ctx.previewFilter(filter); // the lines answer at once; stored once the hand settles
    filterTimer = setTimeout(() => daw("setHitpoints", { id: ctx.active().clip.id, filter }), 350);
  };
  const threshold = new ValueKnob({ label: "Threshold", min: 0, max: 100, step: 1, value: 50, defaultValue: 50, format: (v) => `${Math.round(v)}%`,
                                    onChange: (v) => { filter = { ...filter, threshold: v / 100 }; sendFilter(); } });
  threshold.element.title = "How strong a note start must be to count (higher: only the clearest)";
  const minLength = new ValueKnob({ label: "Min length", min: 0, max: 500, step: 5, value: 50, defaultValue: 50, format: (v) => `${Math.round(v)} ms`,
                                    onChange: (v) => { filter = { ...filter, minLength: v / 1000 }; sendFilter(); } });
  minLength.element.title = "Note starts closer than this to the one before are dropped (the stronger stays)";
  const knobs = html("div", "insert-controls rhythm-knobs");
  knobs.append(threshold.element, minLength.element);
  const beats = html("select", "dropdown");
  beats.title = "Keep only note starts near this grid, as the clip plays now (Off: all)";
  BEATS.forEach(([v, t]) => beats.append(new Option(`Beats: ${t}`, v)));
  beats.addEventListener("change", () => { filter = { ...filter, beats: Number(beats.value) }; sendFilter(); });
  const reset = button("tool", "Reset", "Back to the automatic hitpoints: the default filter, no hand edits", () => {
    daw("setHitpoints", { id: ctx.active().clip.id, filter: null, edits: null });
  });
  const filterRow = html("div", "panel-actions");
  filterRow.append(beats, reset);

  // Create: what the hitpoints make.
  const action = async (name, extra = {}) => {
    const result = await daw("hitpointAction", { id: ctx.active().clip.id, action: name, ...extra });
    if (result?.ok) toast(result.message ?? `${result.count ?? ""} ${name === "split" ? "pieces" : name === "midi" ? "MIDI notes" : name === "markers" ? "markers" : name === "warp" ? "warp tabs" : ""}`.trim());
  };
  const createRow = html("div", "panel-actions hit-tool-create");
  const midiPitch = html("input", "tempo-input hit-tool-pitch");
  Object.assign(midiPitch, { type: "number", min: 0, max: 127, step: 1, value: prefs.get("hitPitch", 36), title: "The MIDI notes' pitch (36: a kick, 38: a snare, 42: a closed hi-hat)" });
  midiPitch.addEventListener("change", () => prefs.set("hitPitch", clamp(Math.round(Number(midiPitch.value) || 36), 0, 127)));
  const grooveName = () => {
    const taken = new Set((ctx.screen.session?.grooves ?? []).map((g) => g.name));
    let n = taken.size + 1;
    while (taken.has(`Groove ${n}`)) ++n;
    return `Groove ${n}`;
  };
  createRow.append(
    button("tool", "Split", "Cut the clip at every hitpoint (short fades at each cut)", () => action("split")),
    button("tool", "Markers", "A song marker at every hitpoint", () => action("markers")),
    button("tool", "Groove", "Save the hitpoints' feel as a groove: Quantize can then use it on other tracks", async () => {
      const name = grooveName();
      const result = await daw("hitpointAction", { id: ctx.active().clip.id, action: "groove", name });
      if (result?.ok) toast(`Saved "${name}": pick it in Quantize's grid list.`);
    }),
    button("tool", "Warp tabs", "A warp tab at every hitpoint, ready to drag (Warp tool)", () => action("warp")),
    button("tool", "MIDI", "A MIDI note at every hitpoint (velocity from its level), on a new drum track", () => action("midi", { pitch: Number(midiPitch.value) || 36 })),
    midiPitch);
  const caption = html("p", "panel-caption", "Click between two to hear the slice (Tab: the next). Click one to lock it, Shift-click to disable it, Alt-click to add one, drag to move it.");
  body.append(count, knobs, filterRow, html("p", "panel-caption hit-tool-create-label", "Create from the hitpoints"), createRow, caption);

  //================================================================ helpers
  const perPx = () => ctx.perPx();
  const lane = () => ctx.active();
  const enabled = (l = lane()) => l.hits.filter((h) => h[2] !== "disabled");
  const hitAt = (l, renderedTime) => {
    let best = -1, distance = GRAB_PX * perPx();
    l.hits.forEach(([t], i) => { const d = Math.abs(t - renderedTime); if (d < distance) { distance = d; best = i; } });
    return best;
  };
  // The hand edits behind the hits shown: every hit that isn't automatic.
  const editsOf = (l) => l.hits.filter((h) => h[2] !== "auto").map((h) => [h[3], h[2]]);
  const storeEdits = (l, edits) => daw("setHitpoints", { id: l.clip.id, edits: edits.length ? edits : null });
  const setState = (l, hit, state) => {
    const edits = editsOf(l).filter(([s]) => Math.abs(s - hit[3]) > 0.006);
    if (state) edits.push([hit[3], state]);
    storeEdits(l, edits);
  };

  // Hear a slice: from enabled hit i to the next one (or the clip's end), at most 1.5 s.
  const audition = (i) => {
    const l = lane(), c = l.clip, list = enabled(l);
    if (!list.length) return;
    slice = clamp(i, -1, list.length - 1);
    const from = slice < 0 ? c.offset : list[slice][0];
    const to = Math.min(slice + 1 < list.length ? list[slice + 1][0] : c.offset + c.length, from + 1.5);
    if (to - from > 0.01) daw("auditionNote", { id: c.id, from, to });
    ctx.redraw();
  };

  //================================================================ pointer
  const pointerdown = (event) => {
    const l = lane();
    if (ctx.laneAt(event.clientY) !== l) return false; // another lane: just looking
    const t = l.toRendered(ctx.timeAt(event.clientX));
    const index = hitAt(l, t);
    const mod = event.ctrlKey || event.metaKey;

    if (event.altKey && mod) { // back to automatic
      if (index >= 0 && l.hits[index][2] !== "auto") setState(l, l.hits[index], null);
      return true;
    }
    if (event.altKey) { // add one here
      if (index < 0) {
        const edits = editsOf(l);
        edits.push([toSource(l.map(), t), "added"]);
        storeEdits(l, edits);
      }
      return true;
    }
    if (index < 0) { // between two: hear that slice
      const list = enabled(l);
      let i = -1;
      list.forEach(([time], k) => { if (time <= t) i = k; });
      audition(i);
      return true;
    }

    const hit = l.hits[index];
    if (event.shiftKey) {
      setState(l, hit, hit[2] === "disabled" ? null : "disabled");
      return true;
    }

    // Press on a hitpoint: a click locks / unlocks it, a drag moves it.
    const startX = event.clientX;
    const move = (e) => {
      if (!dragging && Math.abs(e.clientX - startX) < 3) return;
      const to = clamp(l.toRendered(ctx.timeAt(e.clientX)), l.clip.offset, l.clip.offset + l.clip.length);
      dragging = { from: hit[3], to };
      ctx.redraw();
    };
    const up = () => {
      ctx.wave.removeEventListener("pointermove", move);
      ctx.wave.removeEventListener("pointerup", up);
      ctx.wave.removeEventListener("pointercancel", up);
      if (dragging) {
        const edits = editsOf(l).filter(([s]) => Math.abs(s - hit[3]) > 0.006);
        if (hit[2] !== "added") edits.push([hit[3], "disabled"]); // a found one stays found, now off
        edits.push([toSource(l.map(), dragging.to), "added"]);
        dragging = null;
        storeEdits(l, edits);
        ctx.redraw();
        return;
      }
      if (hit[2] === "auto") setState(l, hit, "locked");
      else if (hit[2] === "locked") setState(l, hit, null);
      else if (hit[2] === "disabled") setState(l, hit, null);
    };
    ctx.wave.setPointerCapture?.(event.pointerId);
    ctx.wave.addEventListener("pointermove", move);
    ctx.wave.addEventListener("pointerup", up);
    ctx.wave.addEventListener("pointercancel", up);
    return true;
  };

  let hovered = -1;
  const hover = (event) => {
    const l = lane();
    const index = ctx.laneAt(event.clientY) === l ? hitAt(l, l.toRendered(ctx.timeAt(event.clientX))) : -1;
    ctx.wave.classList.toggle("is-on-hit", index >= 0);
    if (index !== hovered) { hovered = index; ctx.redraw(); }
  };
  const leave = () => { ctx.wave.classList.remove("is-on-hit"); if (hovered >= 0) { hovered = -1; ctx.redraw(); } };

  const keydown = (event) => {
    const mod = event.ctrlKey || event.metaKey || event.altKey;
    if (event.key === "Tab" && !mod) {
      const n = enabled().length;
      if (!n) return false;
      audition(slice + (event.shiftKey ? -1 : 1) < -1 ? n - 1 : slice + (event.shiftKey ? -1 : 1) > n - 1 ? -1 : slice + (event.shiftKey ? -1 : 1));
      return true;
    }
    return false;
  };

  //================================================================ drawing
  // Each hitpoint: a triangle in the ruler lane (single lane) or at the lane's top, and a
  // line through the lane, its opacity from its strength. Locked and added: solid; disabled: grey.
  const draw = (visible) => {
    shown = visible;
    ctx.layer.querySelectorAll(".hit-mark, .hit-slice").forEach((e) => e.remove());
    if (!visible) return;
    const l = lane(), w = ctx.width();
    const single = ctx.lanes.length < 2;
    const list = enabled(l);
    if (slice >= -1 && list.length && slice < list.length) {
      const from = slice < 0 ? l.clip.offset : list[slice][0];
      const to = slice + 1 < list.length ? list[slice + 1][0] : l.clip.offset + l.clip.length;
      const x0 = ctx.xOf(l.toTimeline(from)), x1 = ctx.xOf(l.toTimeline(to));
      if (x1 > 0 && x0 < w && slice !== -2) {
        const band = html("div", "hit-slice");
        Object.assign(band.style, { left: `${x0}px`, width: `${Math.max(1, x1 - x0)}px`, top: `${RULER + l.top}px`, height: `${l.height}px` });
        ctx.layer.append(band);
      }
    }
    l.hits.forEach(([t, strength, state, source], i) => {
      const moving = dragging && Math.abs(dragging.from - source) < 1e-6;
      const x = ctx.xOf(l.toTimeline(moving ? dragging.to : t));
      if (x < -10 || x > w + 10) return;
      const mark = html("div", `hit-mark is-${state}${i === hovered ? " is-hover" : ""}${moving ? " is-dragging" : ""}`);
      Object.assign(mark.style, { left: `${x}px`, top: `${single ? 0 : RULER + l.top}px`, height: `${(single ? RULER : 0) + l.height}px` });
      if (state === "auto") mark.style.opacity = String(0.45 + 0.55 * strength);
      mark.title = state === "disabled" ? "Disabled: ignored (Shift-click to enable)" : state === "locked" ? "Locked: kept whatever the filter says (click to unlock)"
        : state === "added" ? "Added by hand (Ctrl/Cmd+Alt-click removes it)" : `Found in the audio, strength ${Math.round(strength * 100)}% (click to lock, Shift-click to disable)`;
      ctx.layer.append(mark);
    });
  };

  const update = () => {
    const l = lane(), c = l.clip;
    const off = l.hits.filter((h) => h[2] === "disabled").length;
    count.textContent = l.hits.length ? `${l.hits.length - off} hitpoints${off ? ` (${off} disabled)` : ""}${c.hitsEdited ? ", edited by hand or filtered" : ""}.` : "No note starts found yet.";
    reset.disabled = !c.hitsEdited;
    const f = l.hitFilter;
    if (f && Date.now() > editingUntil) {
      filter = { threshold: f.threshold, minLength: f.minLength, beats: f.beats };
      threshold.update(Math.round(f.threshold * 100));
      minLength.update(Math.round(f.minLength * 1000));
      beats.value = String(f.beats);
      if (beats.selectedIndex < 0) beats.value = "0";
    }
    if (slice >= enabled(l).length) slice = -2;
  };
  const hint = () => "Click between two hitpoints to hear the slice, click one to lock it, Shift-click to disable, Alt-click to add, drag to move.";

  return { body, pointerdown, hover, leave, keydown, draw, update, hint, get shown() { return shown; },
           dispose: () => clearTimeout(filterTimer) };
}
