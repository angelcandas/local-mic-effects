// The library: a collapsible sidebar on the left of the DAW screen. It lists audio
// files (by place: this project, recordings, Music, folders you add), the effects,
// instruments and presets, and lets you drag any of them onto a track:
//   audio file  -> a clip on that track where you drop it (below the tracks: a new track)
//   effect      -> an insert at the end of that track's chain (or the master's)
//   preset      -> replaces that track's chain with the preset's effects
//   instrument  -> that track's instrument (below the tracks: a new MIDI track)
//   plug-in     -> an effect plug-in (VST3/AU) at the end of that track's chain
// Double-click (or Enter) does the same on the selected track, at the playhead.
// The folders come from the native side (DawBridge: places, browse, addPlace,
// removePlace, importFile); instruments from "instruments" / "addInstrument"; the
// preview from "preview". Favourites and Recent are kept in the app's settings (pref).
import { CATEGORIES, MODULES } from "./modules.js";
import { FX_PRESETS } from "./fxpresets.js";
import { html, keyText } from "./widgets.js";

const MIN_WIDTH = 200, MAX_WIDTH = 460, DEFAULT_WIDTH = 340;
const RECENT = 20;
const DRAG_THRESHOLD = 4; // px before a press becomes a drag

const SECTIONS = [
  { id: "project",     name: "Project",     icon: "◆", hint: "Click a pattern or a sound to draw it with the Draw tool (P); drag it onto the song to place it" },
  { id: "favourites",  name: "Favourites",  icon: "♥", hint: "Star anything in the library (☆ on its row) to keep it here" },
  { id: "recent",      name: "Recent",      icon: "↺", hint: "The last 20 things you added from the library" },
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

// "98 → 120 BPM", or "120 BPM" when the loop is already in tempo.
const tempoText = (from, to) => (Math.abs(from - to) < 0.05 ? `${+to.toFixed(1)} BPM` : `${+from.toFixed(1)} → ${+to.toFixed(1)} BPM`);

// The toast for a loop placed in tempo (importFile's `loop`).
export const loopText = (loop) => (Math.abs(loop.from - loop.to) < 0.05
  ? `Placed ${loop.bars} bar${loop.bars === 1 ? "" : "s"} in tempo on the bar`
  : `Stretched ${+loop.from.toFixed(1)} to ${+loop.to.toFixed(1)} BPM (${loop.bars} bar${loop.bars === 1 ? "" : "s"}), on the bar`);

export class LibraryBrowser {
  // screen: the DawScreen. call(command, args): the native daw function, without the
  // error toast (instruments may not exist yet). run(command, args): with it.
  constructor(screen, { call, run, toast, onEvent, pref }) {
    this.screen = screen;
    this.call = call;
    this.run = run;
    this.toast = toast;
    this.pref = pref;
    this.favourites = [];                    // payloads, newest first
    this.recent = [];                        // payloads, newest first
    this.openFx = new Set();                 // effect ids whose presets are unfolded
    this.preview = { file: null, playing: false, auto: prefs.get("previewAuto", false), volume: prefs.get("previewVolume", -6),
                     match: prefs.get("matchLoops", true) };
    call("matchLoops", { on: this.preview.match }); // dropped and imported loops follow the switch

    this.section = prefs.get("section", "audio");
    this.place = prefs.get("place", null);   // the selected place's path (audio section)
    this.query = "";
    this.places = [];
    this.folders = new Map();                // path -> { folders, files, truncated } | "loading" | { error }
    this.open = new Set(prefs.get("open", []));
    this.instruments = null;                 // null: not asked yet; false: not available
    this.effectPlugins = [];                 // effect plug-ins found by a scan ("instruments"' effects)
    this.drag = null;

    this.build();
    this.setWidth(prefs.get("width", DEFAULT_WIDTH));
    this.setCollapsed(prefs.get("collapsed", false));
    this.loadPlaces();
    this.loadInstruments(); // the built-in ones are there at once; plug-ins join as a scan finds them
    this.loadSaved();

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
    Object.assign(expand, { type: "button", title: keyText("Show the library (Shift+B)") });
    expand.addEventListener("click", () => this.setCollapsed(false));
    this.rail.append(expand, ...this.railButtons);

    // Expanded: search, sections and places on the left, the items on the right.
    this.panel = html("div", "library-panel");
    const head = html("div", "library-head");
    this.search = html("input", "library-search");
    Object.assign(this.search, { type: "search", placeholder: "Search everything", spellcheck: false, title: keyText("Search the whole library (Ctrl+F)") });
    this.search.addEventListener("input", () => { this.query = this.search.value.trim().toLowerCase(); this.renderItems(); });
    this.search.addEventListener("keydown", (event) => {
      if (event.key === "Escape") { this.search.value = ""; this.query = ""; this.renderItems(); this.search.blur(); }
      if (event.key === "ArrowDown") { event.preventDefault(); this.items.querySelector(".library-item")?.focus(); }
      event.stopPropagation();
    });
    const collapse = html("button", "mini library-collapse", "«");
    Object.assign(collapse, { type: "button", title: keyText("Hide the library (Shift+B)") });
    collapse.addEventListener("click", () => this.setCollapsed(true));
    head.append(this.search, collapse);

    const body = html("div", "library-body");
    this.nav = html("nav", "library-nav");
    this.items = html("div", "library-items");
    this.items.setAttribute("role", "tree");
    this.items.addEventListener("keydown", (event) => this.onItemsKey(event));
    body.append(this.nav, this.items);

    this.hint = html("div", "library-hint");
    this.panel.append(head, body, this.buildPreview(), this.hint);

    const resize = html("div", "library-resize");
    resize.title = "Drag to resize the library";
    resize.addEventListener("pointerdown", (event) => this.startResize(event));
    resize.addEventListener("dblclick", () => { this.setWidth(DEFAULT_WIDTH); prefs.set("width", this.width); });

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
    this.effectPlugins = result?.ok && Array.isArray(result.effects) ? result.effects : [];
    if (this.section === "instruments" || this.section === "effects") this.renderItems();
  }

  //============================================================================ items
  renderItems() {
    const keep = this.items.scrollTop;
    const focused = document.activeElement?.closest?.(".library-item")?.dataset.key;
    let list;
    if (this.query) list = this.searchItems();
    else switch (this.section) {
      case "favourites": list = this.savedItems(this.favourites, "Nothing starred yet. Hover any effect, preset, instrument or file and click its ☆."); break;
      case "recent": list = this.savedItems(this.recent, "Nothing added yet. What you add from the library shows up here."); break;
      case "effects": list = this.effectItems(); break;
      case "presets": list = this.presetItems(); break;
      case "instruments": list = this.instrumentItems(); break;
      case "project": list = this.projectItems(); break;
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

  item({ key, name, detail, depth = 0, colour, payload, folder, open, title, unfold = null }) {
    const element = html("div", folder ? "library-item is-folder" : "library-item");
    element.tabIndex = -1;
    element.dataset.key = key;
    element.setAttribute("role", "treeitem");
    if (folder) element.setAttribute("aria-expanded", String(Boolean(open)));
    element.style.setProperty("--depth", depth);
    if (colour) element.style.setProperty("--cat", colour);
    element.title = title ?? name;
    const icon = html("span", "library-item-icon", folder ? (open ? "▾" : "▸") : unfold ? (unfold.open ? "▾" : "▸") : "");
    element.append(icon, html("span", "library-item-name", name));
    if (detail) element.append(html("span", "library-item-detail", detail));

    if (folder) {
      element.addEventListener("click", () => this.toggleFolder(folder));
    } else if (payload) {
      element.payload = payload;
      element.addEventListener("pointerdown", (event) => this.pressItem(event, element, payload));
      element.addEventListener("dblclick", () => this.activate(payload));
      if (payload.kind === "audio") element.addEventListener("focus", () => this.pickPreview(payload));
      // What the Draw tool places (FL's picked clip).
      if (payload.kind === "audio" || payload.kind === "pattern") element.addEventListener("focus", () => this.screen.drawTool?.pick(payload));
      // A star keeps it in Favourites (not a pattern: it belongs to this project).
      const starred = this.isFavourite(payload);
      const star = html("button", `library-star${starred ? " is-on" : ""}`, starred ? "★" : "☆");
      Object.assign(star, { type: "button", title: starred ? "Remove from Favourites" : "Add to Favourites" });
      star.addEventListener("pointerdown", (event) => event.stopPropagation());
      star.addEventListener("click", (event) => { event.stopPropagation(); this.toggleFavourite(payload); });
      if (payload.kind !== "pattern") element.append(star);
    }
    if (unfold) {
      // The arrow unfolds the effect's presets.
      icon.classList.add("is-unfold");
      icon.title = unfold.open ? "Hide its presets" : "Show its presets";
      icon.addEventListener("pointerdown", (event) => event.stopPropagation());
      icon.addEventListener("click", (event) => { event.stopPropagation(); unfold.toggle(); });
      if (unfold.open) element.setAttribute("aria-expanded", "true");
    }
    return element;
  }

  //============================================================================ favourites and recent
  async loadSaved() {
    const read = async (key) => {
      try { const list = JSON.parse((await this.pref?.(key)) || "[]"); return Array.isArray(list) ? list.filter((p) => p && p.kind) : []; }
      catch { return []; }
    };
    [this.favourites, this.recent] = await Promise.all([read("libraryFavourites"), read("libraryRecent")]);
    this.renderItems();
  }

  // What identifies a library item, whatever list it shows in.
  keyOf(payload) {
    switch (payload.kind) {
      case "effect": return payload.preset ? `fxp:${payload.id}:${payload.preset}` : `fx:${payload.id}`;
      case "preset": return `preset:${payload.name}`;
      case "instrument": return `inst:${payload.id}`;
      case "audio": return `file:${payload.file}`;
      case "pattern": return `pattern:${payload.id}`;
      default: return `${payload.kind}:${payload.name}`;
    }
  }

  isFavourite(payload) {
    const key = this.keyOf(payload);
    return this.favourites.some((p) => this.keyOf(p) === key);
  }

  toggleFavourite(payload) {
    const key = this.keyOf(payload);
    const had = this.isFavourite(payload);
    this.favourites = had ? this.favourites.filter((p) => this.keyOf(p) !== key) : [{ ...payload }, ...this.favourites];
    this.pref?.("libraryFavourites", JSON.stringify(this.favourites));
    this.renderItems();
  }

  addRecent(payload) {
    const key = this.keyOf(payload);
    this.recent = [{ ...payload }, ...this.recent.filter((p) => this.keyOf(p) !== key)].slice(0, RECENT);
    this.pref?.("libraryRecent", JSON.stringify(this.recent));
    if (this.section === "recent") this.renderItems();
  }

  // Favourites or Recent: grouped by kind, in their own order.
  savedItems(list, empty) {
    if (!list.length) return [html("div", "library-empty", empty)];
    const groups = [["effect", "Effects"], ["preset", "Chain presets"], ["instrument", "Instruments"], ["audio", "Audio"]];
    const out = [];
    for (const [kind, title] of groups) {
      const children = list.filter((p) => p.kind === kind).map((p) => this.item({
        key: `saved:${this.keyOf(p)}`, name: p.name, title: p.file ?? p.name,
        detail: p.kind === "audio" ? p.file.split(/[\\/]/).slice(-2, -1)[0] : p.kind === "instrument" || p.plugin ? p.vendor : "",
        payload: p, colour: p.kind === "effect" ? CATEGORIES[MODULES.find((m) => m.id === p.id)?.category]?.color : null }));
      out.push(...this.group(title, null, children));
    }
    return out;
  }

  //============================================================================ search
  // A search looks in every section at once, and says where each result lives.
  searchItems() {
    const out = [];
    const effects = [];
    for (const m of MODULES) {
      const colour = CATEGORIES[m.category]?.color;
      if (this.matches(m.name) || this.matches(m.description ?? ""))
        effects.push(this.item({ key: `fx:${m.id}`, name: m.name, detail: CATEGORIES[m.category]?.name, colour, title: `${m.name}: ${m.description ?? ""}`,
                                 payload: { kind: "effect", id: m.id, name: m.name } }));
      for (const preset of FX_PRESETS[m.id] ?? [])
        if (this.matches(preset.name) || (this.matches(m.name) && this.query.length > 2))
          effects.push(this.item({ key: `fxp:${m.id}:${preset.name}`, name: `${m.name}: ${preset.name}`, detail: "preset", colour,
                                   payload: { kind: "effect", id: m.id, name: `${m.name}: ${preset.name}`, preset: preset.name } }));
    }
    out.push(...this.group("Effects", null, effects.slice(0, 60)));
    out.push(...this.group("Plug-ins", null, this.pluginItems()));

    const presets = (this.screen.presets ?? []).map((preset, index) => ({ preset, index }))
      .filter(({ preset }) => this.matches(preset.name) || this.matches(preset.category))
      .map(({ preset, index }) => this.item({ key: `preset:${index}`, name: preset.name, detail: preset.category, payload: { kind: "preset", index, name: preset.name } }));
    out.push(...this.group("Chain presets", null, presets));

    const instruments = (this.instruments || []).filter((i) => this.matches(i.name) || this.matches(i.vendor ?? ""))
      .map((i) => this.item({ key: `inst:${i.id}`, name: i.name, detail: i.vendor, payload: { kind: "instrument", id: i.id, name: i.name, vendor: i.vendor } }));
    out.push(...this.group("Instruments", null, instruments));

    // Audio: every folder read so far (the places' top folders are read for this).
    for (const place of this.places) this.loadFolder(place.path);
    const seen = new Set(), files = [];
    for (const [path, content] of this.folders) {
      if (!content?.files) continue;
      const where = this.places.find((p) => path.startsWith(p.path))?.name ?? "";
      for (const file of content.files) {
        if (seen.has(file.path) || !this.matches(file.name)) continue;
        seen.add(file.path);
        files.push(this.item({ key: `file:${file.path}`, name: file.name, detail: where, title: file.path, payload: { kind: "audio", file: file.path, name: file.name } }));
      }
    }
    out.push(...this.group("Audio", null, files.slice(0, 200)));
    return out;
  }

  //============================================================================ project
  // The project's own patterns and sounds (FL's picker): click picks one for the Draw tool,
  // double-click opens a pattern in the rack, drag places either on the song.
  projectItems() {
    const session = this.screen.session;
    if (!session) return [];
    const patterns = (session.patterns ?? []).filter((p) => this.matches(p.name)).map((p) => {
      const bars = p.length / (this.screen.beatsPerBar || 4);
      const element = this.item({ key: `pattern:${p.id}`, name: p.name, detail: Number.isInteger(bars) ? `${bars} bar${bars === 1 ? "" : "s"}` : `${+p.length.toFixed(2)} beats`,
                                  colour: /^#[0-9a-f]{6}$/i.test(p.colour ?? "") ? p.colour : null,
                                  payload: { kind: "pattern", id: p.id, name: p.name }, title: `${p.name}: click to draw it, double-click to edit it, drag onto the song` });
      element.classList.add("is-pattern");
      element.classList.toggle("is-picked", this.screen.selectedPattern === p.id && this.screen.brush?.kind !== "audio");
      return element;
    });
    const seen = new Map();
    for (const track of session.tracks ?? [])
      for (const clip of track.clips ?? [])
        if (!clip.midi && clip.file && !seen.has(clip.file)) seen.set(clip.file, clip);
    const sounds = [...seen.values()].map((clip) => {
      const name = clip.file.split(/[\\/]/).pop();
      const tempo = clip.musical && clip.clipTempo ? ` · ${+Number(clip.clipTempo).toFixed(1)} BPM` : "";
      const element = this.item({ key: `file:${clip.file}`, name, detail: `${Number(clip.fileLength || 0).toFixed(1)} s${tempo}`,
                                  payload: { kind: "audio", file: clip.file, name }, title: `${clip.file}: click to draw it, drag onto a track` });
      element.classList.toggle("is-picked", this.screen.brush?.kind === "audio" && this.screen.brush.file === clip.file);
      return element;
    }).filter((e) => this.matches(e.payload.name));
    const out = [...this.group("Patterns", null, patterns), ...this.group("Audio in this project", null, sounds)];
    if (!out.length) out.push(html("div", "library-empty", "No patterns or audio in this project yet."));
    return out;
  }

  // The session changed: the Project list follows it.
  sessionChanged() {
    if (this.section !== "project" || this.query || this.drag) return;
    const key = JSON.stringify([(this.screen.session?.patterns ?? []).map((p) => [p.id, p.name, p.colour, p.length]),
      (this.screen.session?.tracks ?? []).flatMap((t) => (t.clips ?? []).map((c) => c.file)), this.screen.selectedPattern, this.screen.brush?.file]);
    if (key === this.projectKey) return;
    this.projectKey = key;
    this.renderItems();
  }

  //============================================================================ preview
  buildPreview() {
    const bar = html("div", "library-preview");
    this.previewButton = html("button", "library-preview-play", "▶");
    Object.assign(this.previewButton, { type: "button", title: "Listen to the selected file (it plays once, on top of the song)" });
    this.previewButton.addEventListener("click", () => (this.preview.playing ? this.stopPreview() : this.playPreview()));
    this.previewName = html("span", "library-preview-name", "Select a file to listen");
    const volume = html("input", "library-preview-volume");
    Object.assign(volume, { type: "range", min: -40, max: 0, step: 1, value: this.preview.volume, title: "Preview volume" });
    volume.addEventListener("input", () => {
      this.preview.volume = Number(volume.value);
      prefs.set("previewVolume", this.preview.volume);
      if (this.preview.playing) this.call("preview", { volume: this.preview.volume });
    });
    const auto = html("button", "library-preview-auto", "Auto");
    Object.assign(auto, { type: "button", title: "Play each file as you select it" });
    auto.setAttribute("aria-pressed", String(this.preview.auto));
    auto.addEventListener("click", () => {
      this.preview.auto = !this.preview.auto;
      auto.setAttribute("aria-pressed", String(this.preview.auto));
      prefs.set("previewAuto", this.preview.auto);
      if (!this.preview.auto) this.stopPreview();
    });
    // Loops in tempo: a loop (tempo in its name, its WAV chunk or its beat) previews at the
    // project's tempo from the next bar and repeats; dropped or imported, it plays in tempo.
    const match = html("button", "library-preview-auto library-preview-match", "Tempo");
    Object.assign(match, { type: "button", title: "Match loops to the tempo: loops preview, drop and import at the project's tempo, starting on the bar" });
    match.setAttribute("aria-pressed", String(this.preview.match));
    match.addEventListener("click", () => {
      this.preview.match = !this.preview.match;
      match.setAttribute("aria-pressed", String(this.preview.match));
      prefs.set("matchLoops", this.preview.match);
      this.call("matchLoops", { on: this.preview.match });
      if (this.preview.playing) this.playPreview();
    });
    bar.append(this.previewButton, this.previewName, volume, match, auto);
    this.previewBar = bar;
    this.renderPreview();
    return bar;
  }

  pickPreview(payload) {
    if (this.preview.file === payload.file) return;
    this.preview.file = payload.file;
    this.preview.name = payload.name;
    if (this.preview.auto) this.playPreview();
    else this.renderPreview();
  }

  async playPreview() {
    const { file } = this.preview;
    if (!file) return;
    clearTimeout(this.preview.timer);
    this.preview.playing = true;
    this.renderPreview();
    this.preview.loop = null;
    const result = await this.call("preview", { file, volume: this.preview.volume, sync: this.preview.match });
    if (this.preview.file !== file) return;
    if (!result?.ok) {
      this.preview.playing = false;
      this.renderPreview();
      return this.toast(result?.error || "Couldn't play that file.", "is-error");
    }
    // A loop repeats until stopped.
    if (result.loop) {
      this.preview.loop = result;
      this.renderPreview();
      return;
    }
    this.preview.timer = setTimeout(() => { this.preview.playing = false; this.renderPreview(); }, result.seconds * 1000 + 100);
  }

  stopPreview() {
    clearTimeout(this.preview.timer);
    if (this.preview.playing) this.call("preview", { stop: true });
    this.preview.playing = false;
    this.renderPreview();
  }

  renderPreview() {
    this.previewBar.classList.toggle("has-file", Boolean(this.preview.file));
    this.previewButton.disabled = !this.preview.file;
    this.previewButton.textContent = this.preview.playing ? "■" : "▶";
    this.previewButton.setAttribute("aria-label", this.preview.playing ? "Stop the preview" : "Play the preview");
    const loop = this.preview.playing && this.preview.loop;
    this.previewName.textContent = (this.preview.name ?? "Select a file to listen") + (loop ? ` · ${tempoText(loop.from, loop.to)}` : "");
  }

  effectItems() {
    const list = [];
    for (const [categoryId, category] of Object.entries(CATEGORIES)) {
      const children = MODULES.filter((m) => m.category === categoryId && (this.matches(m.name) || this.matches(m.description ?? "")))
        .flatMap((m) => {
          const presets = FX_PRESETS[m.id] ?? [];
          const open = this.openFx.has(m.id);
          const row = this.item({ key: `fx:${m.id}`, name: m.name, colour: category.color, title: `${m.name}: ${m.description ?? ""}`,
                                  payload: { kind: "effect", id: m.id, name: m.name },
                                  unfold: presets.length ? { open, toggle: () => { open ? this.openFx.delete(m.id) : this.openFx.add(m.id); this.renderItems(); } } : null });
          // Its presets: drag one to add the effect with it loaded.
          const rows = open ? presets.map((preset) => this.item({ key: `fxp:${m.id}:${preset.name}`, name: preset.name, depth: 1, colour: category.color,
                                                                 title: `${m.name} with the "${preset.name}" preset`,
                                                                 payload: { kind: "effect", id: m.id, name: `${m.name}: ${preset.name}`, preset: preset.name } })) : [];
          return [row, ...rows];
        });
      list.push(...this.group(category.name, category.color, children));
    }
    // Effect plug-ins (VST3, and AU on a Mac), after the built-in effects.
    list.push(...this.group("Plug-ins", null, this.pluginItems()));
    if (!this.query) {
      if (!this.effectPlugins.length) list.push(html("div", "library-note", "No effect plug-ins found yet. Rescan after installing some, or add their folder in the Instruments section."));
      list.push(this.rescanButton());
    }
    return list;
  }

  pluginItems() {
    return this.effectPlugins.filter((p) => this.matches(p.name) || this.matches(p.vendor ?? ""))
      .map((p) => this.item({ key: `fx:plugin:${p.id}`, name: p.name, detail: p.vendor || p.format,
                              title: `${p.name}${p.vendor ? ` by ${p.vendor}` : ""} (${p.format} plug-in)`,
                              payload: { kind: "effect", id: `plugin:${p.id}`, plugin: p.id, name: p.name, vendor: p.vendor } }));
  }

  rescanButton() {
    const rescan = html("button", "text-button library-rescan", "Rescan plug-ins");
    rescan.type = "button";
    rescan.addEventListener("click", () => { this.run("scanPlugins"); this.toast("Looking for plug-ins…"); });
    return rescan;
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
                                        payload: { kind: "instrument", id: instrument.id, name: instrument.name, vendor: instrument.vendor } }));
    }
    const list = [];
    for (const [kind, children] of byKind) list.push(...this.group(kind, null, children));
    list.push(this.rescanButton());
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
    if (payload.kind === "pattern") { this.screen.selectedPattern = payload.id; return this.screen.openPatterns(); }
    return this.drop(payload, { track: hasTrack || selected === 0 ? selected : -1 });
  }

  // target: { track (-1: a new one, 0: master), start (seconds; -1: the playhead) }
  async drop(payload, target) {
    const screen = this.screen;
    if (payload.kind === "pattern") {
      const pattern = screen.session?.patterns?.find((p) => p.id === payload.id);
      if (!pattern) return;
      const id = await this.run("addPatternClip", { pattern: pattern.id, start: target.beat, length: pattern.length, row: target.row ?? 0 });
      if (typeof id === "number") screen.selectedPatternClip = id;
      return;
    }
    if (payload.kind === "audio" && target.pattern) {
      await screen.patternsPanel.dropSample(payload.file, target);
      this.addRecent(payload);
      return;
    }
    if (payload.kind === "audio") {
      const result = await this.run("importFile", { file: payload.file, track: target.track, start: target.start ?? -1, match: this.preview.match });
      if (result?.ok && typeof result.track === "number") screen.showWhenDrawn({ track: result.track, clip: result.clip, start: result.start });
      if (result?.loop) this.toast(loopText(result.loop), "", "undo");
      if (result?.ok) this.addRecent(payload);
      return;
    }
    if (payload.kind === "instrument") {
      const result = await this.run("addInstrument", { track: target.track, instrument: payload.id });
      if (result?.ok && typeof result.track === "number") screen.showWhenDrawn({ track: result.track });
      if (result?.ok) this.addRecent(payload);
      return;
    }
    // A chain preset saved in Favourites or Recent is found again by its name.
    if (payload.kind === "preset") {
      const index = (screen.presets ?? []).findIndex((p) => p.name === payload.name);
      if (index < 0) return this.toast(`The "${payload.name}" preset isn't there any more.`, "is-error");
      payload = { ...payload, index };
    }

    let track = target.track;
    if (track < 0) {
      track = await this.run("addTrack");
      if (typeof track !== "number") return;
      await screen.waitForTrack(track);
    }
    if (payload.kind === "effect" && payload.plugin) {
      if (await screen.addPluginEffect(track, payload.plugin) < 0) return;
    } else if (payload.kind === "effect") {
      const id = await screen.addEffect(track, payload.id);
      if (id >= 0 && payload.preset) await screen.applyEffectPreset(id, payload.id, payload.preset);
    }
    if (payload.kind === "preset") await this.run("loadPreset", { track, preset: payload.index });
    this.addRecent(payload);
    screen.select(track, null);
    screen.setDockTab("effects"); // where the new effect shows
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

    // A pattern lands in the Patterns lane at the bar under the pointer, wherever it is dropped
    // over the song (a pattern plays several tracks, so its blocks live in that lane).
    if (kind === "pattern") {
      const area = under.closest(".arrange");
      const lane = screen.patternLane?.element;
      if (!area || !lane || screen.patternRow?.hidden) return null;
      const perBar = screen.beatsPerBar || 4;
      const seconds = Math.max(0, (event.clientX - lane.getBoundingClientRect().left) / screen.zoom);
      const beat = Math.floor(screen.tempoMap.toBeats(seconds) / perBar + 1e-6) * perBar;
      return { beat, element: lane, lane, start: screen.tempoMap.toSeconds(beat), label: `Patterns lane, bar ${beat / perBar + 1}` };
    }

    // The Patterns tab takes an audio file as a sample: on a row of a Kit, or as a new row.
    const rack = kind === "audio" ? under.closest(".patterns-panel") : null;
    if (rack && screen.patternsPanel?.dropSample) {
      const label = under.closest(".pat-label[data-track]");
      return { pattern: true, track: label ? Number(label.dataset.track) : null, pitch: label ? Number(label.dataset.pitch) : null,
               element: label ?? rack, label: label ? `Kit pad ${label.textContent}` : "Pattern rack: new row" };
    }

    // The master takes effects and presets; a file or an instrument dropped on it goes
    // on a new track (the master row is the space below the tracks).
    const master = under.closest(".master-row, .strip.is-master");
    if (master && (kind === "effect" || kind === "preset")) return { track: 0, element: master, label: "Master" };

    // A return takes effects and presets; anything else dropped on it goes on a new track.
    const bus = under.closest(".return-row, .strip.is-return");
    if (bus && (kind === "effect" || kind === "preset"))
      return { track: Number(bus.dataset.track), element: bus, label: bus.querySelector(".track-name, .strip-name")?.textContent ?? "" };

    const row = under.closest(".track-row[data-track]:not(.bus-row), .strip[data-track]:not(.is-return)");
    if (row) {
      const track = Number(row.dataset.track);
      const name = screen.session?.tracks.find((t) => t.id === track)?.name ?? screen.session?.groups?.find((g) => g.id === track)?.name ?? "";
      if (row.classList.contains("group-row") && (kind === "audio" || kind === "instrument")) return null;
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
      const start = kind === "audio" && firstLane && area.classList.contains("arrange") ? timeIn(firstLane) : -1;
      return { track: -1, start: kind === "audio" ? start : undefined, element: area, label: kind === "instrument" ? "New MIDI track" : "New track" };
    }
    return null;
  }
}
