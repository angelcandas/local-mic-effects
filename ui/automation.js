// Automation lanes under a track (see daw::Edit::setLane for the targets and how they play).
//
// The curve button in a track's header unfolds its lanes: one row per target (volume ride,
// pan, a send, any effect value, a macro), picked from the menu in the row's head. In a lane:
//   click            adds a point (snapped to the grid; Alt: free)
//   drag a point     moves it, with the other selected points
//   drag the lane    selects the points in that time range (Shift: adds to the selection)
//   Ctrl+drag, or the Draw tool: draws a curve over what was there
//   double-click or right-click a point: deletes it; Delete: deletes the selected points
// Every gesture is one undo step: the lane is sent whole when the pointer goes up.

import { MODULES } from "./modules.js";
import { formatValue, html, svg } from "./widgets.js";

const PAD = 5;          // px above and below the curve's range
const HIT = 7;          // px: how near a point counts as on it
const DRAW_STEP = 6;    // px between the points of a drawn curve
const RIDE_RANGE = [-24, 12];
const SEND_RANGE = [-61, 6];

export const laneRowHeight = (rowHeight) => Math.max(48, Math.round(rowHeight * 0.6));

const controlLabel = (param) => MODULES.flatMap((m) => m.controls ?? []).find((c) => c.param === param)?.label;

export class AutomationLanes {
  // kit: { daw, toast, popMenu, effectName, panText, dbText, parameterInfo (), returnLetter }
  constructor(screen, kit) {
    this.screen = screen;
    this.kit = kit;
    this.open = new Set();        // track ids whose lanes are unfolded
    this.targets = new Map();     // track id -> the targets shown, in order
    this.version = 0;             // bumps when what the rows show changes outside the session
    this.selection = null;        // { track, target, times: Set } the selected points
    this.lanes = new Map();       // `${track}|${target}` -> the lane being shown (to redraw it in place)
    document.addEventListener("keydown", (event) => this.onKey(event), true);
  }

  // View state: which tracks are unfolded and which lanes they show.
  saveState() {
    return { open: [...this.open], targets: Object.fromEntries(this.targets) };
  }

  restoreState(state) {
    this.open = new Set(Array.isArray(state?.open) ? state.open.filter(Number.isFinite) : []);
    this.targets = new Map();
    for (const [id, list] of Object.entries(state?.targets ?? {}))
      if (Array.isArray(list)) this.targets.set(Number(id), list.filter((t) => typeof t === "string"));
    this.version++;
  }

  isOpen(trackId) { return this.open.has(trackId); }

  setOpen(track, on) {
    if (on) {
      this.open.add(track.id);
      if (!this.shownTargets(track).length) this.targets.set(track.id, ["volume"]);
    } else {
      this.open.delete(track.id);
    }
    this.changed();
  }

  changed() {
    this.version++;
    this.screen.rememberView();
    this.screen.renderArrange(true);
  }

  // The targets a track shows: the ones picked, then any other lane with points.
  shownTargets(track) {
    const list = [...(this.targets.get(track.id) ?? [])].filter((t) => this.spec(track, t));
    for (const lane of track.lanes ?? []) if (!list.includes(lane.target) && this.spec(track, lane.target)) list.push(lane.target);
    return list;
  }

  // The header button that folds and unfolds the lanes.
  createToggle(track) {
    const count = track.lanes?.length ?? 0;
    const open = this.isOpen(track.id);
    const b = html("button", "tt tt-lanes");
    b.type = "button";
    b.innerHTML = `<svg viewBox="0 0 14 10" width="13" height="9" aria-hidden="true"><path d="M1 8 L4.5 3 L8 6 L13 1.5" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>`;
    b.setAttribute("aria-label", count ? `Automation: ${count} lane${count === 1 ? "" : "s"}` : "Automation");
    b.title = open ? "Fold the automation lanes" : "Automation: draw how the volume, pan, sends, effects or macros move over time";
    b.setAttribute("aria-pressed", String(open));
    b.classList.toggle("has-lanes", count > 0);
    b.addEventListener("click", (event) => {
      event.stopPropagation();
      this.setOpen(track, !open);
    });
    return b;
  }

  // What can be automated on a track, for the target menu: [[group, [[target, name]...]]...].
  choices(track) {
    const returns = this.screen.session?.returns ?? [];
    const mixer = [["volume", "Volume"], ["pan", "Pan"]];
    returns.forEach((bus, i) => {
      if (bus.id !== track.output) mixer.push([`send:${bus.id}`, `Send ${this.kit.returnLetter(i)} · ${bus.name}`]);
    });
    const groups = [["Mixer", mixer]];
    const info = this.kit.parameterInfo();
    const addInserts = (inserts, prefix) => {
      for (const insert of inserts ?? []) {
        const name = `${prefix}${insert.name || this.kit.effectName(insert)}`;
        const values = Object.keys(insert.values ?? {}).filter((param) => info[param] || insert.type === "plugin");
        if (values.length) groups.push([name, values.map((param) => [`insert:${insert.id}:${param}`, this.valueName(param)])]);
        for (const chain of insert.chains ?? []) addInserts(chain.inserts, `${name} › ${chain.name || "Chain"} › `);
      }
    };
    addInserts(track.inserts, "");
    const macros = (track.macros ?? []).map((m, i) => [`macro:${i}`, m.name || `Macro ${i + 1}`]);
    if (macros.length) groups.push(["Macros", macros]);
    return groups;
  }

  valueName(param) {
    const known = { pluginOn: "On", pluginInGain: "Input gain", pluginOutGain: "Output gain" };
    if (known[param]) return known[param];
    if (/InGain$/.test(param)) return "Input gain";
    if (/OutGain$/.test(param)) return "Output gain";
    if (MODULES.some((m) => m.power === param) || /On$/.test(param)) return "On / off";
    return controlLabel(param) ?? this.kit.parameterInfo()[param]?.name ?? param;
  }

  findInsert(track, id) {
    const walk = (inserts) => {
      for (const insert of inserts ?? []) {
        if (insert.id === id) return insert;
        for (const chain of insert.chains ?? []) {
          const found = walk(chain.inserts);
          if (found) return found;
        }
      }
      return null;
    };
    return walk(track.inserts);
  }

  // A target's range, scale and text: { name, min, max, toNorm, fromNorm, format, base, step }.
  // Null when the target isn't on the track (an effect was deleted).
  spec(track, target) {
    const [kind, a, b] = target.split(":");
    const linear = (min, max) => ({ min, max, toNorm: (v) => (v - min) / (max - min), fromNorm: (n) => min + n * (max - min) });
    if (kind === "volume")
      return { name: "Volume", ...linear(...RIDE_RANGE), base: 0, step: 0, format: (v) => `${v > 0.05 ? "+" : ""}${v.toFixed(1)} dB`, unit: "dB" };
    if (kind === "pan")
      return { name: "Pan", ...linear(-1, 1), base: track.pan ?? 0, step: 0, format: this.kit.panText };
    if (kind === "send") {
      const returns = this.screen.session?.returns ?? [];
      const index = returns.findIndex((r) => r.id === Number(a));
      if (index < 0) return null;
      const level = (track.sends ?? []).find((s) => s.target === Number(a))?.level ?? -100;
      return { name: `Send ${this.kit.returnLetter(index)}`, ...linear(...SEND_RANGE), base: Math.max(SEND_RANGE[0], level), step: 0,
               format: (v) => (v < -60 ? "off" : `${this.kit.dbText(v)} dB`) };
    }
    if (kind === "macro") {
      const index = Number(a);
      if (!(index >= 0 && index < 8)) return null;
      const macro = track.macros?.[index];
      return { name: macro?.name || `Macro ${index + 1}`, ...linear(0, 1), base: macro?.value ?? 0, step: 0, format: (v) => `${Math.round(v * 100)}%` };
    }
    if (kind === "insert") {
      const insert = this.findInsert(track, Number(a));
      if (!insert || !(b in (insert.values ?? {}))) return null;
      const info = this.kit.parameterInfo()[b] ?? (b === "pluginOn" ? { min: 0, max: 1, step: 1 } : { min: -24, max: 24, label: "dB" });
      const min = info.min ?? 0, max = info.max ?? 1, skew = info.skew ?? 1;
      const span = max - min || 1;
      const step = info.choices?.length ? 1 : info.step >= 1 ? info.step : 0;
      return {
        name: `${insert.name || this.kit.effectName(insert)} · ${this.valueName(b)}`, min, max, step,
        toNorm: (v) => Math.pow(Math.min(1, Math.max(0, (v - min) / span)), skew),
        fromNorm: (n) => min + Math.pow(Math.min(1, Math.max(0, n)), 1 / skew) * span,
        base: insert.values[b],
        format: (v) => (info.choices?.length ? info.choices[Math.round(v - min)] ?? String(v) : formatValue(v, info.label)),
      };
    }
    return null;
  }

  // The rows under a track: one per lane shown. Empty when folded.
  createRows(track, colour) {
    if (!this.isOpen(track.id)) return [];
    const targets = this.shownTargets(track);
    if (!targets.length) targets.push("volume");
    const height = laneRowHeight(this.screen.rowHeight);
    return targets.map((target, i) => this.createLaneRow(track, target, colour, height, targets, i));
  }

  createLaneRow(track, target, colour, height, targets, index) {
    const spec = this.spec(track, target);
    const row = html("div", "take-row lane-row");
    row.dataset.target = target;
    row.style.setProperty("--track-color", colour);
    row.style.height = `${height}px`;

    // Head: the target menu, a readout, clear, add and hide.
    const head = html("div", "take-head lane-head");
    const select = html("select", "dropdown lane-target");
    select.title = "What this lane moves";
    for (const [group, items] of this.choices(track)) {
      const optgroup = document.createElement("optgroup");
      optgroup.label = group;
      for (const [value, name] of items) {
        const option = new Option(name, value);
        option.disabled = value !== target && targets.includes(value);
        optgroup.append(option);
      }
      select.append(optgroup);
    }
    select.value = target;
    select.addEventListener("change", () => this.retarget(track, target, select.value));
    const points = (track.lanes ?? []).find((l) => l.target === target)?.points ?? [];
    const readout = html("span", "lane-readout", points.length ? `${points.length} pt` : spec.format(spec.base));
    readout.title = points.length ? `${points.length} point${points.length === 1 ? "" : "s"}` : "No points yet: it stays at the knob's setting. Click in the lane to add one.";
    const clear = this.kit.button("mini lane-clear", "⌫", "Clear this lane (delete all its points)", () => this.commit(track, target, []));
    clear.disabled = !points.length;
    const add = this.kit.button("mini lane-add", "+", "Add a lane under this one", () => this.addLane(track, targets, index));
    const hide = this.kit.button("mini lane-hide", "×", "Hide this lane (its points stay and still play)", () => this.hideLane(track, targets, target));
    head.append(select, readout, clear, add, hide);

    const lane = html("div", "take-lane auto-lane");
    lane.title = "Click: add a point · Drag: select · Ctrl+drag: draw · Double-click a point: delete";
    const state = { track, target, spec, points: points.map(([t, v]) => ({ t, v })), lane, height, readout };
    this.lanes.set(`${track.id}|${target}`, state);
    this.draw(state);
    lane.addEventListener("pointerdown", (event) => this.onPointerDown(event, state));
    lane.addEventListener("dblclick", (event) => this.onDoubleClick(event, state));
    lane.addEventListener("contextmenu", (event) => this.onContextMenu(event, state));
    row.append(head, lane);
    return row;
  }

  retarget(track, from, to) {
    const list = this.shownTargets(track);
    const index = list.indexOf(from);
    if (index >= 0) list[index] = to;
    this.targets.set(track.id, [...new Set(list)]);
    this.changed();
  }

  addLane(track, targets, index) {
    const used = new Set(targets);
    const next = this.choices(track).flatMap(([, items]) => items.map(([value]) => value)).find((value) => !used.has(value));
    if (!next) return this.kit.toast("Every value of this track already has a lane.");
    const list = [...targets];
    list.splice(index + 1, 0, next);
    this.targets.set(track.id, list);
    this.changed();
  }

  hideLane(track, targets, target) {
    const list = targets.filter((t) => t !== target);
    this.targets.set(track.id, list.length ? list : []);
    if (!list.length) this.open.delete(track.id);
    // A lane with points comes back by itself when the lanes are shown: say where it went.
    if ((track.lanes ?? []).some((l) => l.target === target) && list.length)
      this.kit.toast("Hidden. Its points still play; it shows again when you unfold the lanes.");
    this.changed();
  }

  //============================================================================ drawing
  width() { return Math.max(1, this.screen.timelineSeconds() * this.screen.zoom); }
  x(t) { return t * this.screen.zoom; }
  y(state, v) { return PAD + (1 - state.spec.toNorm(v)) * (state.height - 2 * PAD); }
  valueAt(state, clientY) {
    const box = state.lane.getBoundingClientRect();
    const n = 1 - (clientY - box.top - PAD) / Math.max(1, box.height - 2 * PAD);
    return this.quantise(state, state.spec.fromNorm(Math.min(1, Math.max(0, n))));
  }
  timeAt(state, clientX) {
    const box = state.lane.getBoundingClientRect();
    return Math.max(0, (clientX - box.left) / this.screen.zoom);
  }
  quantise(state, v) {
    const { min, max, step } = state.spec;
    if (step > 0) v = min + Math.round((v - min) / step) * step;
    return Math.min(max, Math.max(min, v));
  }

  isSelected(state, point) {
    const s = this.selection;
    return Boolean(s && s.track === state.track.id && s.target === state.target && s.times.has(point.t));
  }

  draw(state) {
    const { lane, points, spec } = state;
    const width = this.width(), height = state.height;
    lane.querySelector("svg")?.remove();
    const box = svg("svg", { class: "auto-curve", width: width.toFixed(0), height, viewBox: `0 0 ${width.toFixed(1)} ${height}` });
    // The middle (0 dB, centre pan) as a faint line for bipolar targets.
    if (spec.min < 0 && spec.max > 0) {
      const y0 = this.y(state, 0).toFixed(1);
      box.append(svg("line", { class: "auto-zero", x1: 0, x2: width.toFixed(1), y1: y0, y2: y0 }));
    }
    if (!points.length) {
      const y = this.y(state, spec.base).toFixed(1);
      box.append(svg("line", { class: "auto-base", x1: 0, x2: width.toFixed(1), y1: y, y2: y }));
    } else {
      const coords = [[0, points[0].v], ...points.map((p) => [p.t, p.v]), [width / this.screen.zoom, points[points.length - 1].v]]
        .map(([t, v]) => `${this.x(t).toFixed(1)},${this.y(state, v).toFixed(1)}`);
      box.append(svg("polygon", { class: "auto-fill", points: `0,${height} ${coords.join(" ")} ${width.toFixed(1)},${height}` }),
                 svg("polyline", { class: "auto-line", points: coords.join(" ") }));
      // Points: all of them when there's room, otherwise only the selected ones.
      const dense = points.length > width / 3;
      for (const point of points) {
        const selected = this.isSelected(state, point);
        if (dense && !selected) continue;
        box.append(svg("circle", { class: `auto-point${selected ? " is-selected" : ""}`, cx: this.x(point.t).toFixed(1), cy: this.y(state, point.v).toFixed(1), r: selected ? 4 : 3.2 }));
      }
    }
    if (state.band) {
      const [a, b] = state.band;
      box.append(svg("rect", { class: "auto-band", x: this.x(a).toFixed(1), y: 0, width: Math.max(1, this.x(b) - this.x(a)).toFixed(1), height }));
    }
    lane.prepend(box);
  }

  // A small label by the pointer while dragging: the time and value.
  showTip(state, point, clientX, clientY) {
    let tip = state.lane.querySelector(".auto-tip");
    if (!point) return tip?.remove();
    if (!tip) state.lane.append((tip = html("span", "auto-tip")));
    tip.textContent = `${state.spec.format(point.v)} · ${formatTime(point.t)}`;
    const box = state.lane.getBoundingClientRect();
    tip.style.left = `${clientX - box.left + 10}px`;
    tip.style.top = `${Math.max(0, Math.min(state.height - 18, clientY - box.top - 22))}px`;
  }

  hit(state, event) {
    const box = state.lane.getBoundingClientRect();
    const px = event.clientX - box.left, py = event.clientY - box.top;
    let best = -1, distance = HIT;
    state.points.forEach((p, i) => {
      const d = Math.hypot(this.x(p.t) - px, this.y(state, p.v) - py);
      if (d <= distance) { best = i; distance = d; }
    });
    return best;
  }

  //============================================================================ editing
  commit(track, target, points) {
    const sorted = [...points].sort((a, b) => a.t - b.t);
    // Two points at one time make a step; three or more there keep only the outer two.
    const list = sorted.filter((p, i) => !(i > 0 && i < sorted.length - 1 && Math.abs(sorted[i - 1].t - p.t) < 1e-4 && Math.abs(sorted[i + 1].t - p.t) < 1e-4));
    return this.kit.daw("setLane", { track: track.id, target, points: list.map((p) => [round(p.t, 3), round(p.v, 4)]) });
  }

  select(state, times) {
    this.selection = times.size ? { track: state.track.id, target: state.target, times } : null;
    for (const other of this.lanes.values()) if (other !== state && other.lane.isConnected) this.draw(other);
    this.draw(state);
  }

  onPointerDown(event, state) {
    if (event.button !== 0) return;
    event.preventDefault();
    event.stopPropagation();
    this.screen.select(state.track.id, null, false);
    const lane = state.lane;
    lane.setPointerCapture?.(event.pointerId);
    const index = this.hit(state, event);
    const drawing = this.screen.tool === "draw" || event.ctrlKey || event.metaKey;

    if (index >= 0 && !drawing) return this.dragPoints(event, state, index);
    if (drawing) return this.drawCurve(event, state);

    // Empty: a click adds a point, a drag selects the points in a time range.
    const t0 = this.timeAt(state, event.clientX);
    const keep = event.shiftKey && this.selection?.track === state.track.id && this.selection?.target === state.target ? new Set(this.selection.times) : new Set();
    let moved = false;
    const move = (e) => {
      if (!moved && Math.abs(e.clientX - event.clientX) < 4) return;
      moved = true;
      const a = this.screen.snapTime(t0, e), b = this.screen.snapTime(this.timeAt(state, e.clientX), e);
      state.band = [Math.min(a, b), Math.max(a, b)];
      const times = new Set(keep);
      for (const p of state.points) if (p.t >= state.band[0] - 1e-6 && p.t <= state.band[1] + 1e-6) times.add(p.t);
      this.select(state, times);
    };
    const up = (e) => {
      lane.removeEventListener("pointermove", move);
      lane.removeEventListener("pointerup", up);
      lane.removeEventListener("pointercancel", up);
      if (moved) {
        state.band = null;
        this.draw(state);
        return;
      }
      if (e.type === "pointercancel") return;
      const point = { t: this.screen.snapTime(t0, event), v: this.valueAt(state, event.clientY) };
      this.selection = { track: state.track.id, target: state.target, times: new Set([round(point.t, 3)]) };
      this.commit(state.track, state.target, [...state.points, point]);
    };
    lane.addEventListener("pointermove", move);
    lane.addEventListener("pointerup", up);
    lane.addEventListener("pointercancel", up);
  }

  // Moves the grabbed point (snapped) and the other selected points by as much.
  dragPoints(event, state, index) {
    const grabbed = state.points[index];
    let times = this.isSelected(state, grabbed) ? new Set(this.selection.times) : new Set([grabbed.t]);
    if (event.shiftKey) {
      // Shift+click a point: adds it to the selection or takes it off.
      const next = new Set(this.selection?.track === state.track.id && this.selection?.target === state.target ? this.selection.times : []);
      if (next.has(grabbed.t)) next.delete(grabbed.t); else next.add(grabbed.t);
      times = next;
    }
    this.select(state, times);
    const origin = state.points.map((p) => ({ ...p, picked: times.has(p.t) }));
    const lane = state.lane, { spec } = state;
    const box = lane.getBoundingClientRect();
    let moved = false;
    const move = (e) => {
      if (!moved && Math.hypot(e.clientX - event.clientX, e.clientY - event.clientY) < 3) return;
      moved = true;
      const dt = this.screen.snapTime(grabbed.t + (e.clientX - event.clientX) / this.screen.zoom, e) - grabbed.t;
      const dn = -(e.clientY - event.clientY) / Math.max(1, box.height - 2 * PAD);
      // Shift while dragging: only up and down.
      const shiftOnly = e.shiftKey && !event.shiftKey;
      const minT = Math.min(...origin.filter((p) => p.picked).map((p) => p.t));
      const offset = shiftOnly ? 0 : Math.max(-minT, dt);
      state.points = origin.map((p) => (p.picked
        ? { t: p.t + offset, v: this.quantise(state, spec.fromNorm(Math.min(1, Math.max(0, spec.toNorm(p.v) + dn)))) }
        : { t: p.t, v: p.v }));
      const lead = state.points[index];
      this.selection = { track: state.track.id, target: state.target, times: new Set(state.points.filter((_, i) => origin[i].picked).map((p) => p.t)) };
      this.draw(state);
      this.showTip(state, lead, e.clientX, e.clientY);
    };
    const up = () => {
      lane.removeEventListener("pointermove", move);
      lane.removeEventListener("pointerup", up);
      lane.removeEventListener("pointercancel", up);
      this.showTip(state, null);
      if (!moved) return;
      this.selection.times = new Set([...this.selection.times].map((t) => round(t, 3)));
      this.commit(state.track, state.target, state.points);
    };
    lane.addEventListener("pointermove", move);
    lane.addEventListener("pointerup", up);
    lane.addEventListener("pointercancel", up);
  }

  // Draws a curve: the stroke replaces the points under it.
  drawCurve(event, state) {
    const lane = state.lane;
    const stroke = [];
    const before = state.points;
    const add = (e) => {
      const t = this.timeAt(state, e.clientX), v = this.valueAt(state, e.clientY);
      const last = stroke[stroke.length - 1];
      if (last && Math.abs(this.x(t) - this.x(last.t)) < DRAW_STEP) { last.v = v; return; }
      stroke.push({ t, v });
    };
    const render = () => {
      if (!stroke.length) return;
      const ordered = [...stroke].sort((a, b) => a.t - b.t);
      const from = ordered[0].t, to = ordered[ordered.length - 1].t;
      state.points = [...before.filter((p) => p.t < from - 1e-6 || p.t > to + 1e-6), ...thin(ordered, state)].sort((a, b) => a.t - b.t);
      this.draw(state);
    };
    add(event);
    render();
    const move = (e) => { add(e); render(); this.showTip(state, stroke[stroke.length - 1], e.clientX, e.clientY); };
    const up = (e) => {
      lane.removeEventListener("pointermove", move);
      lane.removeEventListener("pointerup", up);
      lane.removeEventListener("pointercancel", up);
      this.showTip(state, null);
      if (e.type === "pointercancel") { state.points = before; return this.draw(state); }
      this.selection = null;
      this.commit(state.track, state.target, state.points);
    };
    lane.addEventListener("pointermove", move);
    lane.addEventListener("pointerup", up);
    lane.addEventListener("pointercancel", up);
  }

  onDoubleClick(event, state) {
    const index = this.hit(state, event);
    if (index < 0) return;
    event.preventDefault();
    event.stopPropagation();
    this.deletePoints(state, new Set([state.points[index].t]));
  }

  onContextMenu(event, state) {
    event.preventDefault();
    event.stopPropagation();
    const index = this.hit(state, event);
    if (index >= 0 && !this.isSelected(state, state.points[index])) return this.deletePoints(state, new Set([state.points[index].t]));
    const selected = this.selection?.track === state.track.id && this.selection?.target === state.target ? this.selection.times : null;
    this.kit.popMenu(event.clientX, event.clientY, ({ item, rule }) => {
      if (selected?.size) item(`Delete ${selected.size} selected point${selected.size === 1 ? "" : "s"}`, () => this.deletePoints(state, selected), { shortcut: "Delete" });
      item("Select all points", () => this.select(state, new Set(state.points.map((p) => p.t))), { disabled: !state.points.length });
      rule();
      item("Clear lane", () => this.commit(state.track, state.target, []), { disabled: !state.points.length, danger: true });
    });
  }

  deletePoints(state, times) {
    this.selection = null;
    this.commit(state.track, state.target, state.points.filter((p) => !times.has(p.t)));
  }

  onKey(event) {
    const s = this.selection;
    if (!s || !s.times.size) return;
    const target = event.target;
    if (target?.closest?.("input, textarea, select, [contenteditable]")) return;
    const state = this.lanes.get(`${s.track}|${s.target}`);
    if (!state || !state.lane.isConnected) { this.selection = null; return; }
    if (event.key === "Delete" || event.key === "Backspace") {
      event.preventDefault();
      event.stopPropagation();
      this.deletePoints(state, s.times);
    } else if (event.key === "Escape") {
      this.select(state, new Set());
    }
  }

  // The key for a track's lane rows: built again only when this changes.
  rowsKey(track) {
    if (!this.isOpen(track.id)) return `closed|${this.version}`;
    return JSON.stringify([this.version, track.lanes, this.targets.get(track.id), track.pan, track.sends, track.output,
                           (track.macros ?? []).map((m) => [m.name, m.value]),
                           track.inserts, (this.screen.session?.returns ?? []).map((r) => [r.id, r.name])]);
  }
}

function round(value, digits) {
  const f = 10 ** digits;
  return Math.round(value * f) / f;
}

// Drops the points of a drawn stroke that lie on the line between their neighbours (within
// half a percent of the lane's height), so a straight stroke is two points.
function thin(points, state) {
  if (points.length < 3) return points;
  const tolerance = 0.005;
  const n = (p) => state.spec.toNorm(p.v);
  const kept = [points[0]];
  for (let i = 1; i < points.length - 1; ++i) {
    const a = kept[kept.length - 1], b = points[i], c = points[i + 1];
    const x = (b.t - a.t) / Math.max(1e-9, c.t - a.t);
    if (Math.abs(n(a) + (n(c) - n(a)) * x - n(b)) > tolerance) kept.push(b);
  }
  kept.push(points[points.length - 1]);
  return kept;
}

function formatTime(seconds) {
  const m = Math.floor(seconds / 60), s = seconds - m * 60;
  return `${m}:${s.toFixed(2).padStart(5, "0")}`;
}
