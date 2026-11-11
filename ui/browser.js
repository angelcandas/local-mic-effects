// The library: a collapsible sidebar on the left of the DAW screen. It lists audio
// files (by place: this project, recordings, Music, folders you add), the effects,
// instruments and presets, and lets you drag any of them onto a track:
//   audio file  -> a clip on that track where you drop it (below the tracks: a new track)
//   effect      -> an insert at the end of that track's chain (or the master's)
//   preset      -> replaces that track's chain with the preset's effects
//   instrument  -> that track's instrument (below the tracks: a new MIDI track)
// Double-click (or Enter) does the same on the selected track, at the playhead.
// The folders come from the native side (DawBridge: places, browse, addPlace,
// removePlace, importFile); instruments from "instruments" / "addInstrument".
import { CATEGORIES, MODULES } from "./modules.js";
import { html } from "./widgets.js";

const MIN_WIDTH = 200, MAX_WIDTH = 460;
const DRAG_THRESHOLD = 4; // px before a press becomes a drag

const SECTIONS = [
  { id: "audio",       name: "Audio", icon: "♪", hint: "Drag a file onto a track, or below the tracks for a new one" },
  { id: "effects",     name: "Effects",     icon: "fx", hint: "Drag onto a track or the master; double-click adds to the selected track" },
  { id: "instruments", name: "Instruments", icon: "♫", hint: "Drag onto a track, or below the tracks for a new MIDI track" },
  { id: "presets",     name: "Presets",     icon: "★", hint: "Drag onto a track to replace its effects with the preset's" },
];

const PLACE_ICONS = { project: "◆", recordings: "●", music: "♪", downloads: "↓", desktop: "▭", folder: "▸" };

const prefs = {
  get(key, fallback) { try { const v = localStorage.getItem(`micfx.library.${key}`); return v == null ? fallback : JSON.parse(v); } catch { return fallback; } },
  set(key, value) { try { localStorage.setItem(`micfx.library.${key}`, JSON.stringify(value)); } catch { /* no storage */ } },
};

const sizeText = (bytes) => (bytes >= 1e6 ? `${(bytes / 1e6).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1e3))} kB`);

export class LibraryBrowser {
  // screen: the DawScreen. call(command, args): the native daw function, without the
  // error toast (instruments may not exist yet). run(command, args): with it.
  constructor(screen, { call, run, toast, onEvent }) {
    this.screen = screen;
    this.call = call;
    this.run = run;
    this.toast = toast;

    this.section = prefs.get("section", "audio");
    this.place = prefs.get("place", null);   // the selected place's path (audio section)
    this.query = "";
    this.places = [];
    this.folders = new Map();                // path -> { folders, files, truncated } | "loading" | { error }
    this.open = new Set(prefs.get("open", []));
    this.instruments = null;                 // null: not asked yet; false: not available
    this.drag = null;

    this.build();
    this.setWidth(prefs.get("width", 300));
    this.setCollapsed(prefs.get("collapsed", false));
    this.loadPlaces();

    onEvent("dawPlugins", () => this.loadInstruments());
  }

  //============================================================================ layout
  build() {
    const root = (this.element = html("aside", "daw-library"));
    root.setAttribute("aria-label", "Library");

    // Collapsed: a thin strip of section icons.
    this.rail = html("div", "library-rail");
    this.railButtons = SECTIONS.map((section) => {
      const b = html("button", "library-rail-button", section.icon);
      Object.assign(b, { type: "button", title: `${section.name} (Shift+B shows the library)` });
      b.dataset.section = section.id;
      b.addEventListener("click", () => { this.setSection(section.id); this.setCollapsed(false); });
      return b;
    });
    const expand = html("button", "library-rail-button library-expand", "»");
    Object.assign(expand, { type: "button", title: "Show the library (Shift+B)" });
    expand.addEventListener("click", () => this.setCollapsed(false));
    this.rail.append(expand, ...this.railButtons);

    // Expanded: search, sections and places on the left, the items on the right.
    this.panel = html("div", "library-panel");
    const head = html("div", "library-head");
    this.search = html("input", "library-search");
    Object.assign(this.search, { type: "search", placeholder: "Search", spellcheck: false, title: "Search this section (Ctrl+F)" });
    this.search.addEventListener("input", () => { this.query = this.search.value.trim().toLowerCase(); this.renderItems(); });
    this.search.addEventListener("keydown", (event) => {
      if (event.key === "Escape") { this.search.value = ""; this.query = ""; this.renderItems(); this.search.blur(); }
      if (event.key === "ArrowDown") { event.preventDefault(); this.items.querySelector(".library-item")?.focus(); }
      event.stopPropagation();
    });
    const collapse = html("button", "mini library-collapse", "«");
    Object.assign(collapse, { type: "button", title: "Hide the library (Shift+B)" });
    collapse.addEventListener("click", () => this.setCollapsed(true));
    head.append(this.search, collapse);

    const body = html("div", "library-body");
    this.nav = html("nav", "library-nav");
    this.items = html("div", "library-items");
    this.items.setAttribute("role", "tree");
    this.items.addEventListener("keydown", (event) => this.onItemsKey(event));
    body.append(this.nav, this.items);

    this.hint = html("div", "library-hint");
    this.panel.append(head, body, this.hint);

    const resize = html("div", "library-resize");
    resize.title = "Drag to resize the library";
    resize.addEventListener("pointerdown", (event) => this.startResize(event));
    resize.addEventListener("dblclick", () => this.setWidth(300));

    root.append(this.rail, this.panel, resize);
    this.renderNav();
  }

  setCollapsed(collapsed) {
    this.collapsed = collapsed;
    this.element.classList.toggle("is-collapsed", collapsed);
    prefs.set("collapsed", collapsed);
  }

  toggle() {
    this.setCollapsed(!this.collapsed);
    if (!this.collapsed) this.search.focus();
  }

  focusSearch() {
    if (this.collapsed) this.setCollapsed(false);
    this.search.focus();
    this.search.select();
  }

  setWidth(width) {
    this.width = Math.round(Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, width)));
    this.element.style.setProperty("--library-width", `${this.width}px`);
  }

  startResize(event) {
    event.preventDefault();
    const startX = event.clientX, startWidth = this.width;
    const target = event.currentTarget;
    target.setPointerCapture(event.pointerId);
    const move = (e) => this.setWidth(startWidth + e.clientX - startX);
    const up = () => {
      target.removeEventListener("pointermove", move);
      target.removeEventListener("pointerup", up);
      target.removeEventListener("pointercancel", up);
      prefs.set("width", this.width);
    };
    target.addEventListener("pointermove", move);
    target.addEventListener("pointerup", up);
    target.addEventListener("pointercancel", up);
  }

  setSection(id, place = undefined) {
    this.section = id;
    if (place !== undefined) this.place = place;
    // Clicking a place reads it again: a take you just recorded shows up.
    if (place) for (const path of [...this.folders.keys()]) if (path.startsWith(place)) this.folders.delete(path);
    prefs.set("section", id);
    prefs.set("place", this.place);
    if (id === "instruments" && this.instruments == null) this.loadInstruments();
    this.renderNav();
    this.renderItems();
  }

  renderNav() {
    const nav = [];
    nav.push(html("div", "library-label", "Library"));
    for (const section of SECTIONS) {
      const b = html("button", "library-nav-item");
      b.type = "button";
      b.dataset.section = section.id;
      b.append(html("span", "library-icon", section.icon), html("span", "library-nav-name", section.name));
      b.classList.toggle("is-selected", this.section === section.id && (section.id !== "audio" || !this.place));
      b.addEventListener("click", () => this.setSection(section.id, section.id === "audio" ? null : this.place));
      nav.push(b);
    }

    nav.push(html("div", "library-label", "Places"));
    for (const place of this.places) {
      const b = html("button", "library-nav-item is-place");
      Object.assign(b, { type: "button", title: place.path });
      b.append(html("span", "library-icon", PLACE_ICONS[place.kind] ?? "▸"), html("span", "library-nav-name", place.name));
      b.classList.toggle("is-selected", this.section === "audio" && this.place === place.path);
      b.addEventListener("click", () => this.setSection("audio", place.path));
      if (place.removable) {
        const remove = html("span", "library-remove", "×");
        remove.title = "Remove this folder from the library (the files stay)";
        remove.addEventListener("click", async (event) => {
          event.stopPropagation();
          const result = await this.run("removePlace", { path: place.path });
          if (this.place === place.path) this.place = null;
          if (result?.places) this.setPlaces(result.places);
        });
        b.append(remove);
      }
      nav.push(b);
    }
    const add = html("button", "library-nav-item library-add", "+ Add folder…");
    Object.assign(add, { type: "button", title: "Add a folder of samples or songs to the library" });
    add.addEventListener("click", async () => {
      const result = await this.run("addPlace");
      if (result?.places) {
        this.setPlaces(result.places);
        if (result.added) this.setSection("audio", result.added);
      }
    });
    nav.push(add);

    this.nav.replaceChildren(...nav);
    this.railButtons.forEach((b) => b.classList.toggle("is-selected", b.dataset.section === this.section));
    this.hint.textContent = SECTIONS.find((s) => s.id === this.section)?.hint ?? "";
  }

  //============================================================================ data
  async loadPlaces() {
    const result = await this.call("places");
    if (result?.places) this.setPlaces(result.places);
  }

  setPlaces(places) {
    this.places = places;
    if (this.place && !places.some((p) => p.path === this.place)) this.place = null;
    this.renderNav();
    this.renderItems();
  }

  // The project changed (opened, saved as): "This project" may have come or gone.
  refresh() {
    this.folders.clear();
    this.loadPlaces();
  }

  async loadFolder(path) {
    if (this.folders.has(path)) return;
    this.folders.set(path, "loading");
    const result = await this.call("browse", { path });
    this.folders.set(path, result?.ok ? result : { error: result?.error || "Couldn't read that folder." });
    this.renderItems();
  }

  async loadInstruments() {
    const result = await this.call("instruments");
    this.instruments = result?.ok && Array.isArray(result.instruments) ? result.instruments : false;
    if (this.section === "instruments") this.renderItems();
  }

  //============================================================================ items
  renderItems() {
    const keep = this.items.scrollTop;
    const focused = document.activeElement?.closest?.(".library-item")?.dataset.key;
    let list;
    switch (this.section) {
      case "effects": list = this.effectItems(); break;
      case "presets": list = this.presetItems(); break;
      case "instruments": list = this.instrumentItems(); break;
      default: list = this.audioItems();
    }
    this.items.replaceChildren(...list);
    if (!list.some((e) => e.classList.contains("library-item") || e.querySelector?.(".library-item"))) {
      if (this.query && !this.items.querySelector(".library-empty")) this.items.append(html("div", "library-empty", "Nothing matches."));
    }
    this.items.scrollTop = keep;
    if (focused) this.items.querySelector(`.library-item[data-key="${CSS.escape(focused)}"]`)?.focus();
  }

  matches(name) {
    return !this.query || name.toLowerCase().includes(this.query);
  }

  group(title, colour, children) {
    if (children.length === 0) return [];
    const head = html("div", "library-group", title);
    if (colour) head.style.setProperty("--cat", colour);
    return [head, ...children];
  }

  item({ key, name, detail, depth = 0, colour, payload, folder, open, title }) {
    const element = html("div", folder ? "library-item is-folder" : "library-item");
    element.tabIndex = -1;
    element.dataset.key = key;
    element.setAttribute("role", "treeitem");
    if (folder) element.setAttribute("aria-expanded", String(Boolean(open)));
    element.style.setProperty("--depth", depth);
    if (colour) element.style.setProperty("--cat", colour);
    element.title = title ?? name;
    const icon = html("span", "library-item-icon", folder ? (open ? "▾" : "▸") : "");
    element.append(icon, html("span", "library-item-name", name));
    if (detail) element.append(html("span", "library-item-detail", detail));

    if (folder) {
      element.addEventListener("click", () => this.toggleFolder(folder));
    } else if (payload) {
      element.payload = payload;
      element.addEventListener("pointerdown", (event) => this.pressItem(event, element, payload));
      element.addEventListener("dblclick", () => this.activate(payload));
    }
    return element;
  }

  effectItems() {
    const list = [];
    for (const [categoryId, category] of Object.entries(CATEGORIES)) {
      const children = MODULES.filter((m) => m.category === categoryId && (this.matches(m.name) || this.matches(m.description ?? "")))
        .map((m) => this.item({ key: `fx:${m.id}`, name: m.name, colour: category.color, title: `${m.name}: ${m.description ?? ""}`,
                                payload: { kind: "effect", id: m.id, name: m.name } }));
      list.push(...this.group(category.name, category.color, children));
    }
    return list;
  }

  presetItems() {
    const presets = this.screen.presets ?? [];
    const byCategory = new Map();
    presets.forEach((preset, index) => {
      if (!this.matches(preset.name) && !this.matches(preset.category)) return;
      if (!byCategory.has(preset.category)) byCategory.set(preset.category, []);
      byCategory.get(preset.category).push(this.item({ key: `preset:${index}`, name: preset.name,
                                                       payload: { kind: "preset", index, name: preset.name } }));
    });
    const list = [];
    for (const [category, children] of byCategory) list.push(...this.group(category, null, children));
    if (presets.length === 0) list.push(html("div", "library-empty", "Loading presets…"));
    return list;
  }

  instrumentItems() {
    if (this.instruments == null) return [html("div", "library-empty", "Looking for instruments…")];
    if (this.instruments === false) {
      return [html("div", "library-empty", "Instruments arrive with MIDI tracks. Built-in synths and your VST3 instruments will be listed here.")];
    }
    const byKind = new Map();
    for (const instrument of this.instruments) {
      if (!this.matches(instrument.name) && !this.matches(instrument.vendor ?? "")) continue;
      const kind = instrument.kind === "builtin" ? "Built-in" : String(instrument.kind ?? "Plug-ins").toUpperCase();
      if (!byKind.has(kind)) byKind.set(kind, []);
      byKind.get(kind).push(this.item({ key: `inst:${instrument.id}`, name: instrument.name, detail: instrument.vendor,
                                        payload: { kind: "instrument", id: instrument.id, name: instrument.name } }));
    }
    const list = [];
    for (const [kind, children] of byKind) list.push(...this.group(kind, null, children));
    const rescan = html("button", "text-button library-rescan", "Rescan plug-ins");
    rescan.type = "button";
    rescan.addEventListener("click", () => { this.run("scanPlugins"); this.toast("Looking for plug-ins…"); });
    list.push(rescan);
    return list;
  }

  audioItems() {
    const roots = this.place ? this.places.filter((p) => p.path === this.place) : this.places;
    if (roots.length === 0) return [html("div", "library-empty", "No folders yet. Add one with “+ Add folder…”.")];

    // One place: its contents. All places: each as a folder.
    if (this.place) {
      const path = roots[0].path;
      this.loadFolder(path);
      return this.folderChildren(path, 0);
    }
    const list = [];
    for (const place of roots) list.push(...this.folderEntry({ name: place.name, path: place.path }, 0));
    return list;
  }

  // A folder row, and its contents when open. With a search, a folder shows when its
  // name matches or something loaded inside it does.
  folderEntry(folder, depth) {
    const open = this.open.has(folder.path) || (this.query && this.folders.get(folder.path)?.files);
    const children = open ? this.folderChildren(folder.path, depth + 1) : [];
    const hasMatch = children.some((e) => e.classList.contains("library-item"));
    if (this.query && !this.matches(folder.name) && !hasMatch) return [];
    const row = this.item({ key: `dir:${folder.path}`, name: folder.name, depth, folder: folder.path, open, title: folder.path });
    return [row, ...children];
  }

  folderChildren(path, depth) {
    const content = this.folders.get(path);
    if (!content) { this.loadFolder(path); return [this.note("Loading…", depth)]; }
    if (content === "loading") return [this.note("Loading…", depth)];
    if (content.error) return [this.note(content.error, depth)];
    const list = [];
    for (const folder of content.folders) list.push(...this.folderEntry(folder, depth));
    for (const file of content.files) {
      if (!this.matches(file.name)) continue;
      list.push(this.item({ key: `file:${file.path}`, name: file.name, detail: sizeText(file.size), depth, title: file.path,
                            payload: { kind: "audio", file: file.path, name: file.name } }));
    }
    if (content.truncated) list.push(this.note("Only the first 2000 entries are shown.", depth));
    if (list.length === 0 && !this.query) list.push(this.note("No audio files here.", depth));
    return list;
  }

  note(text, depth) {
    const element = html("div", "library-note", text);
    element.style.setProperty("--depth", depth);
    return element;
  }

  toggleFolder(path) {
    if (this.open.has(path)) this.open.delete(path);
    else { this.open.add(path); this.loadFolder(path); }
    prefs.set("open", [...this.open].slice(-50));
    this.renderItems();
  }

  onItemsKey(event) {
    const item = event.target.closest(".library-item");
    if (!item) return;
    const all = [...this.items.querySelectorAll(".library-item")];
    const index = all.indexOf(item);
    const go = (i) => { event.preventDefault(); all[Math.max(0, Math.min(all.length - 1, i))]?.focus(); };
    const folder = item.classList.contains("is-folder");
    const isOpen = item.getAttribute("aria-expanded") === "true";
    if (event.key === "ArrowDown") go(index + 1);
    else if (event.key === "ArrowUp") { if (index === 0) { event.preventDefault(); this.search.focus(); } else go(index - 1); }
    else if (event.key === "ArrowRight" && folder && !isOpen) { event.preventDefault(); item.click(); }
    else if (event.key === "ArrowLeft" && folder && isOpen) { event.preventDefault(); item.click(); }
    else if (event.key === "Enter") { event.preventDefault(); folder ? item.click() : item.payload && this.activate(item.payload); }
    else return;
    event.stopPropagation();
  }

  //============================================================================ adding
  // Double-click / Enter: onto the selected track (audio: at the playhead).
  activate(payload) {
    const selected = this.screen.selectedTrack;
    const hasTrack = this.screen.session?.tracks.some((t) => t.id === selected);
    if (payload.kind === "audio") return this.drop(payload, { track: hasTrack ? selected : -1, start: -1 });
    if (payload.kind === "instrument") return this.drop(payload, { track: hasTrack ? selected : -1 });
    return this.drop(payload, { track: hasTrack || selected === 0 ? selected : -1 });
  }

  // target: { track (-1: a new one, 0: master), start (seconds; -1: the playhead) }
  async drop(payload, target) {
    const screen = this.screen;
    if (payload.kind === "audio") {
      const result = await this.run("importFile", { file: payload.file, track: target.track, start: target.start ?? -1 });
      if (result?.ok && typeof result.track === "number") screen.showWhenDrawn({ track: result.track, clip: result.clip, start: result.start });
      return;
    }
    if (payload.kind === "instrument") {
      const result = await this.run("addInstrument", { track: target.track, instrument: payload.id });
      if (result?.ok && typeof result.track === "number") screen.showWhenDrawn({ track: result.track });
      return;
    }

    let track = target.track;
    if (track < 0) {
      track = await this.run("addTrack");
      if (typeof track !== "number") return;
      await screen.waitForTrack(track);
    }
    if (payload.kind === "effect") await screen.addEffect(track, payload.id);
    if (payload.kind === "preset") await this.run("loadPreset", { track, preset: payload.index });
    screen.select(track, null);
    screen.setDock(true);
  }

  //============================================================================ dragging
  pressItem(event, element, payload) {
    if (event.button !== 0) return;
    element.focus();
    const startX = event.clientX, startY = event.clientY;
    const pointerId = event.pointerId;
    const move = (e) => {
      if (e.pointerId !== pointerId) return;
      if (!this.drag) {
        if (Math.hypot(e.clientX - startX, e.clientY - startY) < DRAG_THRESHOLD) return;
        this.startDrag(payload);
      }
      this.moveDrag(e);
    };
    const up = (e) => {
      if (e.pointerId !== pointerId) return;
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", cancel);
      if (this.drag) this.endDrag(e, true);
    };
    const cancel = (e) => {
      if (e.pointerId !== pointerId) return;
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", cancel);
      if (this.drag) this.endDrag(e, false);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", cancel);
  }

  startDrag(payload) {
    const ghost = html("div", `library-ghost is-${payload.kind}`, payload.name);
    document.body.append(ghost);
    document.body.classList.add("library-dragging");
    this.drag = { payload, ghost, target: null, highlight: null, line: null };
    getSelection()?.removeAllRanges();
  }

  moveDrag(event) {
    const drag = this.drag;
    drag.ghost.style.transform = `translate(${event.clientX + 12}px, ${event.clientY + 10}px)`;
    const target = this.findTarget(event);
    drag.target = target;

    if (drag.highlight !== target?.element) {
      drag.highlight?.classList.remove("is-drop-target");
      target?.element?.classList.add("is-drop-target");
      drag.highlight = target?.element ?? null;
    }
    drag.ghost.classList.toggle("is-refused", !target);
    drag.ghost.dataset.where = target?.label ?? "";

    // Where the clip will start.
    if (target?.lane && target.start != null) {
      if (!drag.line) drag.line = html("div", "library-drop-line");
      if (drag.line.parentElement !== target.lane) target.lane.append(drag.line);
      drag.line.style.left = `${target.start * this.screen.zoom}px`;
    } else {
      drag.line?.remove();
    }
  }

  endDrag(event, dropped) {
    const { payload, ghost, highlight, line } = this.drag;
    const target = dropped ? this.findTarget(event) : null;
    ghost.remove();
    line?.remove();
    highlight?.classList.remove("is-drop-target");
    document.body.classList.remove("library-dragging");
    this.drag = null;
    if (target) this.drop(payload, target);
  }

  // What's under the pointer: { track, start?, element (to highlight), lane?, label }
  // or null when the item can't go there.
  findTarget(event) {
    const kind = this.drag.payload.kind;
    const screen = this.screen;
    const under = document.elementFromPoint(event.clientX, event.clientY);
    if (!under || !screen.root.contains(under)) return null;

    const timeIn = (lane) => screen.snapTime((event.clientX - lane.getBoundingClientRect().left) / screen.zoom, event);

    // The master takes effects and presets; a file or an instrument dropped on it goes
    // on a new track (the master row is the space below the tracks).
    const master = under.closest(".master-row, .strip.is-master");
    if (master && (kind === "effect" || kind === "preset")) return { track: 0, element: master, label: "Master" };

    const row = under.closest(".track-row[data-track], .strip[data-track]");
    if (row) {
      const track = Number(row.dataset.track);
      const name = screen.session?.tracks.find((t) => t.id === track)?.name ?? "";
      if (kind !== "audio") return { track, element: row, label: name };
      const lane = row.querySelector(".lane");
      if (!lane || row.classList.contains("strip")) return null;
      return { track, start: timeIn(lane), element: row, lane, label: name };
    }

    // The effects panel: the track it shows.
    if (under.closest(".inspector") && kind !== "audio" && kind !== "instrument") {
      const owner = screen.selectedTrack;
      if (owner === 0 || screen.session?.tracks.some((t) => t.id === owner))
        return { track: owner, element: screen.inspector, label: owner === 0 ? "Master" : "" };
      return null;
    }

    // Anywhere else in the tracks (below the last one): a new track.
    const area = under.closest(".arrange, .mixer");
    if (area) {
      const firstLane = screen.rows.querySelector(".lane") ?? screen.masterRow.querySelector(".lane");
      const start = kind === "audio" && firstLane && !screen.root.classList.contains("show-mixer") ? timeIn(firstLane) : -1;
      return { track: -1, start: kind === "audio" ? start : undefined, element: area, label: kind === "instrument" ? "New MIDI track" : "New track" };
    }
    return null;
  }
}
