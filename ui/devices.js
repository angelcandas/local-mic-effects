// Effect devices: one card per effect and the chain that holds them, the way every
// place that holds effects is drawn (a track's, a return's, a group's or the master's
// chain in the DAW).
//
// A card doesn't talk to the engine itself. It reads and writes values through an
// access adapter ({ get, set (param, value, first), info }) and runs its verbs through
// the chain, which owns the commands (DawBridge: setInsert, duplicateInsert, insertAB...).
// The host screen hands in its shared helpers (kit) so menus, knobs and toasts look and
// behave the same everywhere.
import { CATEGORIES, MODULE_KEYS, MODULES, isControlShown } from "./modules.js";
import { createActivity, createFxView } from "./fxviews.js";
import { createPresetPicker, presetParams } from "./fxpresets.js";
import { clamp, createFilePicker, html, IS_MAC } from "./widgets.js";
import * as Juce from "./juce/index.js";
import { buildRack } from "./racks.js";

const PITCH_PARTS = ["tune", "pitch", "harmony"].map((id) => MODULES.find((m) => m.id === id));
const MOD = IS_MAC ? "Cmd" : "Ctrl";

// The effect's own name for an insert type ("Pitch" for the shared pitch insert).
// The switches that turn a DAW insert on and off (the pitch insert has one per part;
// any of them on means it's on).
export function insertSwitches(insert) {
  if (insert.type === "plugin") return ["pluginOn"];
  if (insert.type === "rack") return ["rackOn"];
  if (insert.type === "pitch") return ["tune", "pitch", "harmony"].map((id) => MODULES.find((m) => m.id === id)?.power).filter(Boolean);
  const power = MODULES.find((m) => MODULE_KEYS[m.id] === insert.type)?.power;
  return power ? [power] : [];
}

export function effectName(insert) {
  if (insert.type === "plugin") return insert.plugin?.name || "Plug-in";
  if (insert.type === "rack") return "Rack";
  if (insert.module) return MODULES.find((m) => m.id === insert.module)?.name ?? insert.module;
  if (insert.type === "pitch") return "Pitch";
  return MODULES.find((m) => MODULE_KEYS[m.id] === insert.type)?.name ?? insert.type;
}

//==============================================================================
// The effects of one chain owner ({ id, inserts }, id 0 = the master): a card per
// effect, "+" between them and at the end. Click a title to select an effect (Shift
// for several); with the chain focused: Left/Right move the selection, Delete removes,
// Ctrl/Cmd+D duplicates, Ctrl/Cmd+C/X/V copy, cut and paste, 0 switches on or off, F2
// renames.
export class DeviceChain {
  constructor(element, owner, screen, kit) {
    this.element = element;
    this.owner = owner;
    this.screen = screen; // foldedInserts, rememberView, addEffect, addPluginEffect, library, copyEffects, deviceClipboard
    this.kit = kit;       // { daw, button, popMenu, ValueKnob, toast, parameterInfo (), dbText, reducedMotion }
    this.cards = new Map();
    this.selected = new Set(); // insert ids
    this.signature = null; // built on the first update, even for an empty chain

    this.element.tabIndex = -1;
    this.element.classList.add("device-chain");
    this.element.addEventListener("keydown", (event) => this.onKey(event));
    // A click on the empty part of the chain clears the selection.
    this.element.addEventListener("pointerdown", (event) => {
      if (event.target === this.element) { this.select(null); this.element.focus({ preventScroll: true }); }
    });
  }

  update(owner) {
    this.owner = owner;
    const signature = owner.inserts.map((insert) => `${insert.id}:${insert.type}`).join(",");

    if (signature !== this.signature) {
      const before = new Set(this.cards.keys());
      const grew = this.signature !== null && owner.inserts.length > this.cards.size;
      // Keys keep working after an edit: the chain had focus, so it keeps it.
      const hadFocus = this.element.contains(document.activeElement);
      this.signature = signature;
      for (const id of [...this.selected]) if (!owner.inserts.some((insert) => insert.id === id)) this.selected.delete(id);
      this.build();
      const added = grew ? owner.inserts.filter((insert) => !before.has(insert.id)).map((insert) => insert.id) : [];
      // A duplicate or a paste is selected, so the next key carries on from there.
      if (added.length && (hadFocus || this.selected.size)) this.select(added, false, !hadFocus);
      else if (hadFocus) (this.cards.get(this.selectedIds.at(-1))?.element ?? this.element).focus({ preventScroll: true });
      if (grew) {
        // Bring the new effect into view without scrolling the rest of the page.
        const card = this.cards.get(added[added.length - 1])?.element ?? this.element.lastElementChild;
        const dock = this.element.closest(".inspector");
        if (card && dock) {
          const right = card.offsetLeft + card.offsetWidth - dock.offsetLeft;
          if (right > dock.scrollLeft + dock.clientWidth) dock.scrollLeft = right - dock.clientWidth + 10;
        }
      }
    }

    for (const insert of owner.inserts) this.cards.get(insert.id)?.update(insert);
    this.markAutomated();
  }

  // The track's lanes for effect values ("insert:<id>:<param>"), by target.
  get laneTargets() {
    return new Set((this.owner.lanes ?? []).filter((lane) => lane.points?.length).map((lane) => lane.target));
  }

  // A dot on each control that has an automation lane.
  markAutomated() {
    const lanes = this.laneTargets;
    for (const [id, card] of this.cards)
      for (const control of card.element.querySelectorAll("[data-param]"))
        if (control.closest(".insert") === card.element)
          control.classList.toggle("is-automated", lanes.has(`insert:${id}:${control.dataset.param}`));
  }

  // Right-click on a control: its automation lane, and (on a plug-in's card) taking it off.
  openControlMenu(card, param, x, y) {
    const id = card.insert.id;
    const target = `insert:${id}:${param}`;
    const track = this.screen.session?.tracks?.find((t) => t.id === this.owner.id);
    const automation = this.screen.automation;
    const automated = this.laneTargets.has(target);
    const panelEntry = card.insert.plugin?.panel?.find((entry) => entry.param === param);
    this.kit.popMenu(x, y, ({ item, rule, heading }) => {
      heading(panelEntry?.name ?? card.controlLabel(param) ?? "Control");
      const noLanes = !track || !automation ? { disabled: true, hint: "Automation lanes are on tracks (not returns or the master)" } : {};
      item("Show automation", () => automation.showLane(track, target), { hint: automated ? "Its lane under the track" : "Opens a lane under the track to draw how it moves", ...noLanes });
      item("Delete automation", () => this.kit.daw("setLane", { track: this.owner.id, target, points: [] }),
           { danger: true, disabled: !automated || Boolean(noLanes.disabled), hint: "It goes back to staying where the knob is" });
      if (panelEntry) {
        rule();
        item("Take off the card", () => this.kit.daw("setPluginPanel", { id, remove: panelEntry.index }), { hint: "Its lane and macro mapping go too" });
      }
      rule();
      item("Effect menu…", () => {
        if (!this.selected.has(id)) this.select([id]);
        this.openCardMenu(card, x, y);
      });
    });
  }

  // From "dawStatus": live readings by insert id ({ in, out, spectrum? }, null when off).
  live(readings) {
    // With reduced motion asked for, the effect pictures move a few times a second, not 30.
    if (this.kit.reducedMotion.matches) {
      const now = performance.now();
      if (now - (this.lastLive ?? 0) < 250) return;
      this.lastLive = now;
    }
    for (const [id, card] of this.cards) card.live(readings?.[id] ?? null, readings);
  }

  build() {
    const { button } = this.kit;
    this.element.replaceChildren();
    this.cards.clear();
    const folded = this.screen.foldedInserts;

    this.owner.inserts.forEach((insert, index) => {
      const card = new DeviceCard(insert, this.insertAccess(insert), this.kit, {
        remove: () => this.remove([insert.id]),
        toggleFold: () => this.toggleFold(insert.id),
        rename: () => this.rename(insert.id),
        switchAB: () => this.kit.daw("insertAB", { id: insert.id, action: "switch" }),
        openMenu: (x, y, anchor) => this.openCardMenu(this.cards.get(insert.id), x, y, anchor),
        chain: this, // a rack builds its own chains inside, with this chain's screen and kit
      });
      this.cards.set(insert.id, card);
      card.element.classList.toggle("is-folded", folded.has(insert.id));
      card.element.classList.toggle("is-selected", this.selected.has(insert.id));
      // A thin "+" between two cards adds an effect right there.
      if (index > 0) this.element.append(this.createGap(index));
      this.element.append(card.element);
      this.enableDrag(card, insert.id);
      const head = card.element.querySelector(".insert-head");
      head.addEventListener("dblclick", (event) => { if (!event.target.closest("button, input")) this.toggleFold(insert.id); });
      card.element.querySelector(".insert-folded-name").addEventListener("click", () => this.toggleFold(insert.id));
      card.element.tabIndex = -1;
      card.element.addEventListener("contextmenu", (event) => {
        // (An effect in a rack opened its own menu already.)
        if (event.defaultPrevented || event.target.closest("input, select")) return;
        event.preventDefault();
        const control = event.target.closest("[data-param]");
        if (control && control.closest(".insert") === card.element) return this.openControlMenu(card, control.dataset.param, event.clientX, event.clientY);
        if (!this.selected.has(insert.id)) this.select([insert.id]);
        this.openCardMenu(card, event.clientX, event.clientY);
      });
    });

    // The end of the chain: add an effect (the library's effects can also be dropped here).
    const add = button("insert-add", "+", "Add an effect at the end of the chain", (event) => this.openAddMenu(this.owner.inserts.length, event.currentTarget));
    if (!this.owner.inserts.length) add.append(html("span", "insert-add-text", this.owner.id === 0 ? "Add an effect to the mix" : "Add an effect"));
    add.classList.toggle("is-empty", !this.owner.inserts.length);
    this.addButton = add;
    this.element.append(add);
  }

  // How a card reads and writes its insert: the values in the session, edits as
  // setInsert commands (one undo step per gesture).
  insertAccess(insert) {
    const access = {
      insert,
      get: (param) => access.insert.values[param] ?? this.kit.parameterInfo()[param]?.default ?? 0,
      set: (param, value, first) => {
        access.insert.values[param] = value;
        this.kit.daw("setInsert", { id: access.insert.id, param, value, continue: !first });
      },
      info: (param) => this.kit.parameterInfo()[param],
      file: {
        load: () => this.kit.daw("insertFile", { id: access.insert.id }),
        clear: () => this.kit.daw("insertFile", { id: access.insert.id, path: "" }),
      },
      openPlugin: () => this.kit.daw("showPluginInsert", { id: access.insert.id }),
      daw: (command, args) => this.kit.daw(command, args),
      rescan: () => { this.kit.daw("scanPlugins"); this.kit.toast("Looking for plug-ins…"); },
    };
    return access;
  }

  createGap(index) {
    const gap = html("div", "insert-gap");
    gap.append(this.kit.button("insert-gap-add", "+", "Add an effect here", (event) => this.openAddMenu(index, event.currentTarget)));
    return gap;
  }

  toggleFold(id) {
    const folded = this.screen.foldedInserts;
    if (folded.has(id)) folded.delete(id); else folded.add(id);
    this.cards.get(id)?.element.classList.toggle("is-folded", folded.has(id));
    this.screen.rememberView();
  }

  //============================================================================ selection
  get selectedIds() { return this.owner.inserts.map((insert) => insert.id).filter((id) => this.selected.has(id)); }

  // ids: null clears; add: Shift-click adds (or takes away) one.
  select(ids, add = false, keepFocus = false) {
    if (!ids) this.selected.clear();
    else if (add) for (const id of ids) { if (this.selected.has(id)) this.selected.delete(id); else this.selected.add(id); }
    else this.selected = new Set(ids);
    for (const [id, card] of this.cards) card.element.classList.toggle("is-selected", this.selected.has(id));
    // keepFocus: select without taking the keyboard from wherever it is.
    const last = ids?.[ids.length - 1];
    if (last != null && !keepFocus) this.cards.get(last)?.element.focus({ preventScroll: true });
  }

  moveSelection(step) {
    const ids = this.owner.inserts.map((insert) => insert.id);
    if (!ids.length) return;
    const current = this.selectedIds;
    const from = current.length ? ids.indexOf(step < 0 ? current[0] : current[current.length - 1]) : step < 0 ? ids.length : -1;
    this.select([ids[clamp(from + step, 0, ids.length - 1)]]);
  }

  onKey(event) {
    if (event.target.closest("input, select, textarea, [role=slider]")) return;
    const mod = event.ctrlKey || event.metaKey;
    const key = event.key.toLowerCase();
    const ids = this.selectedIds;
    const done = () => { event.preventDefault(); event.stopPropagation(); };

    if (!mod && !event.altKey && (event.key === "ArrowLeft" || event.key === "ArrowRight")) { done(); return this.moveSelection(event.key === "ArrowLeft" ? -1 : 1); }
    if (mod && key === "v") { done(); return this.paste(ids.length ? this.indexAfter(ids) : this.owner.inserts.length); }
    if (!ids.length) return;
    if (mod && key === "g" && this.canGroup) {
      done();
      const rack = ids.length === 1 && this.cards.get(ids[0])?.insert.type === "rack";
      return event.shiftKey ? (rack ? this.ungroup(ids[0]) : undefined) : this.group(ids);
    }
    if (!mod && (event.key === "Delete" || event.key === "Backspace")) { done(); return this.remove(ids); }
    if (mod && !event.shiftKey && key === "d") { done(); return this.duplicate(ids); }
    if (mod && key === "c") { done(); return this.copy(ids); }
    if (mod && key === "x") { done(); return this.cut(ids); }
    if (!mod && !event.altKey && event.key === "0") { done(); return this.toggleOn(ids); }
    if (event.key === "F2" && ids.length === 1) { done(); return this.rename(ids[0]); }
    if (!mod && !event.altKey && key === "q" && ids.length === 1 && this.canSwap) { done(); return this.swap(ids[0]); }
    if (event.key === "Escape" && this.selected.size) { done(); return this.select(null); }
  }

  //============================================================================ verbs
  // Hot-swap (DAW): the library browses effects to put in this one's place.
  get canSwap() { return typeof this.screen.library?.startSwap === "function"; }
  get canGroup() { return this.kit.racks !== false; }
  swap(id) {
    const card = this.cards.get(id);
    if (card) this.screen.library.startSwap({ insert: card.insert, track: this.owner.id, name: card.effectName,
                                              trackName: this.owner.id === 0 ? "Master" : this.owner.name ?? "" });
  }

  indexAfter(ids) {
    return Math.max(...ids.map((id) => this.owner.inserts.findIndex((insert) => insert.id === id))) + 1;
  }

  remove(ids) {
    // One command per effect, one undo step for all of them.
    ids.forEach((id, i) => this.kit.daw("removeInsert", { id, continue: i > 0 }));
  }

  // Copies right after the last of them (in order, one undo step).
  async duplicate(ids) {
    if (ids.length === 1) return this.kit.daw("duplicateInsert", { id: ids[0] });
    return this.kit.daw("pasteInserts", { ids, ...this.target, index: this.indexAfter(ids) });
  }

  // Where pastes go: the track's chain, or (inside a rack) one of the rack's chains.
  get target() { return this.owner.rack != null ? { rack: this.owner.rack, chain: this.owner.chain } : { to: this.owner.id }; }

  // Racks (DAW): the effects into one rack (Ctrl/Cmd+G), a rack back into the chain.
  group(ids) {
    return this.kit.daw("groupInserts", { ids }).then((result) => {
      if (result?.ok) this.kit.toast(`Grouped ${ids.length === 1 ? "it" : `${ids.length} effects`} into a rack: add chains beside the first to play in parallel.`, "", "undo");
    });
  }
  ungroup(id) { return this.kit.daw("ungroupInsert", { id }); }

  async copy(ids, quiet = false) {
    const result = await this.kit.daw("copyDevices", { ids });
    if (!result?.ok) return false;
    const names = ids.map((id) => this.cards.get(id)?.title).filter(Boolean);
    this.screen.deviceClipboard = { count: result.count, name: names.length === 1 ? names[0] : `${names.length} effects` };
    if (!quiet) this.kit.toast(`Copied ${this.screen.deviceClipboard.name}. Paste it into any chain (${MOD}+V).`);
    return true;
  }

  async cut(ids) {
    if (await this.copy(ids, true)) this.remove(ids);
  }

  paste(index) {
    if (!this.screen.deviceClipboard) return this.kit.toast("Copy an effect first (select it, then " + MOD + "+C).");
    this.element.focus({ preventScroll: true });
    return this.kit.daw("pasteInserts", { ...this.target, index, clipboard: true });
  }

  toggleOn(ids) {
    const on = !ids.every((id) => this.cards.get(id)?.isOn);
    ids.forEach((id) => this.cards.get(id)?.setOn(on));
  }

  // Types a name in place of the title. Empty goes back to the effect's own name.
  rename(id) {
    const card = this.cards.get(id);
    if (!card) return;
    if (this.screen.foldedInserts.has(id)) this.toggleFold(id);
    card.editName((name) => this.kit.daw("renameInsert", { id, name }));
  }

  //============================================================================ menus
  // The effects, by category, to add at `index` in the chain.
  openAddMenu(index, anchor) {
    const box = anchor.getBoundingClientRect();
    this.kit.popMenu(box.left, box.bottom + 4, ({ item, heading, menu }) => {
      menu.classList.add("is-columns");
      for (const [categoryId, category] of Object.entries(CATEGORIES)) {
        const modules = MODULES.filter((m) => m.category === categoryId);
        if (!modules.length) continue;
        heading(category.name);
        for (const module of modules) item(module.name, () => this.screen.addEffect(this.owner.id, module.id, index), { indent: true, hint: module.description ?? "" });
      }
      // Effect plug-ins (VST3, AU on a Mac) found by a scan.
      heading("Plug-ins");
      const plugins = this.screen.library?.effectPlugins ?? [];
      // (A long list stays short here; the library lists them all, with a search.)
      for (const plugin of plugins.slice(0, 24))
        item(plugin.name, () => this.screen.addPluginEffect(this.owner.id, plugin.id, index), { indent: true, hint: [plugin.vendor, plugin.format].filter(Boolean).join(" · ") });
      if (plugins.length > 24) item(`${plugins.length - 24} more in the library…`, () => { this.screen.library?.setSection("effects"); this.screen.library?.setCollapsed(false); }, { indent: true, hint: "Effects, under Plug-ins" });
      if (!plugins.length) item("Look for plug-ins…", () => { this.kit.daw("scanPlugins"); this.kit.toast("Looking for plug-ins…"); }, { indent: true, hint: "Scans the usual VST3 (and AU) folders" });
    }, anchor);
  }

  // The same menu from a right-click and from the card's ⋯ button. With several
  // effects selected, the verbs act on all of them.
  openCardMenu(card, x, y, anchor = null) {
    const { daw } = this.kit;
    const count = this.owner.inserts.length, id = card.insert.id;
    const index = this.owner.inserts.findIndex((insert) => insert.id === id);
    const ids = this.selected.has(id) && this.selected.size > 1 ? this.selectedIds : [id];
    const several = ids.length > 1;
    const isPlugin = card.insert.type === "plugin", isRack = card.insert.type === "rack";
    const clipboard = this.screen.deviceClipboard;
    this.kit.popMenu(x, y, ({ item, rule, heading }) => {
      if (several) heading(`${ids.length} effects`);
      item(card.isOn ? "Switch off" : "Switch on", () => (several ? this.toggleOn(ids) : card.setOn(!card.isOn)), { shortcut: "0" });
      if (!several) {
        item(this.screen.foldedInserts.has(id) ? "Unfold" : "Fold", () => this.toggleFold(id), { hint: "Or double-click its title" });
        item("Show trims", () => card.setTrimsShown(!card.trimsShown), { checked: !card.gains.hidden });
      }
      if (!several && !isPlugin && !isRack) {
        rule();
        const onB = card.insert.ab === "b";
        item(onB ? "Switch to A" : "Switch to B", () => daw("insertAB", { id, action: "switch" }),
             { hint: card.insert.ab ? "Compare two settings of this effect" : "Keep these settings as A and try others as B" });
        item(onB ? "Copy B to A" : "Copy A to B", () => daw("insertAB", { id, action: "copy" }),
             { disabled: !card.insert.ab, hint: "Write the settings you hear over the other set" });
      }
      rule();
      item("Duplicate", () => this.duplicate(ids), { shortcut: `${MOD}+D` });
      item("Copy", () => this.copy(ids), { shortcut: `${MOD}+C` });
      item("Cut", () => this.cut(ids), { shortcut: `${MOD}+X` });
      item(clipboard ? `Paste ${clipboard.name} after` : "Paste after", () => this.paste(this.indexAfter(ids)), { disabled: !clipboard, shortcut: `${MOD}+V` });
      item(clipboard ? `Paste ${clipboard.name} before` : "Paste before", () => this.paste(index), { disabled: !clipboard });
      if (!several) item("Rename", () => this.rename(id), { shortcut: "F2", hint: card.insert.name ? `Back to "${card.effectName}" by leaving it empty` : "" });
      if (!several && this.canSwap && !isRack) item("Swap from the library…", () => this.swap(id), { shortcut: "Q", hint: "Try other effects and presets in its place: ↑ ↓ to hear each, Enter to keep, Esc to put it back" });
      if (this.canGroup) {
        rule();
        item(several ? `Group ${ids.length} effects into a rack` : "Group into a rack", () => this.group(ids),
             { shortcut: `${MOD}+G`, hint: "A rack plays chains of effects side by side (in parallel)" });
        if (!several && isRack) item("Ungroup", () => this.ungroup(id), { shortcut: `${MOD}+Shift+G`, hint: "Its chains go back into this chain, one after another" });
        if (!several && isRack) item("Add a chain", () => this.kit.daw("addRackChain", { id }));
      }
      rule();
      if (!several) {
        item("Move earlier", () => daw("moveInsert", { id, index: index - 1 }), { disabled: index === 0 });
        item("Move later", () => daw("moveInsert", { id, index: index + 1 }), { disabled: index >= count - 1 });
        item("Add an effect before…", () => this.openAddMenu(index, card.element));
        item("Add an effect after…", () => this.openAddMenu(index + 1, card.element));
        rule();
      }
      item("Copy the chain", () => this.screen.copyEffects(this.owner.id));
      item(several ? `Remove ${ids.length} effects` : "Remove", () => this.remove(ids), { danger: true, shortcut: "Delete" });
    }, anchor);
  }

  // A card dragged out of the chain and let go over `target` (see deviceDropTarget in daw.js).
  async dropOutside(card, id, target, copy) {
    if (target.library) {
      if (!card.presetPicker) return this.kit.toast("Plug-ins keep their presets in their own window.");
      card.element.scrollIntoView({ block: "nearest", inline: "nearest" });
      return card.presetPicker.saveAs();
    }
    if (target.track === this.owner.id && !copy) return;
    const ids = this.selected.has(id) ? this.selectedIds : [id];
    let n = 0;
    for (const insertId of ids)
      await this.kit.daw(copy ? "copyInsert" : "moveInsert", { id: insertId, track: target.track, index: -1, continue: n++ > 0 });
    this.kit.toast(`${copy ? "Copied" : "Moved"} ${ids.length === 1 ? card.effectName ?? "the effect" : `${ids.length} effects`} to ${target.label}.`, "", "undo");
  }

  // Drag a card by its header to reorder the chain (pointer events, so it works in the
  // WebView). A press without a drag selects it (Shift: adds it to the selection).
  enableDrag(card, id) {
    const head = card.element.querySelector(".insert-head");
    head.classList.add("is-draggable");

    head.addEventListener("pointerdown", (event) => {
      if (event.button !== 0 || event.detail > 1 || event.target.closest("button, input, .switch, [role=switch]")) return;
      const startX = event.clientX;
      const cards = [...this.element.querySelectorAll(":scope > .insert")];
      const from = cards.indexOf(card.element);
      const rects = cards.map((c) => c.getBoundingClientRect());
      let dragging = false, to = from;
      head.setPointerCapture(event.pointerId);

      const place = () => {
        // Cards between the old and new slot slide aside to open the gap.
        const width = rects[from].width + 14;
        cards.forEach((c, i) => {
          if (i === from) return;
          const shift = from < to && i > from && i <= to ? -width : from > to && i < from && i >= to ? width : 0;
          c.style.transform = shift ? `translateX(${shift}px)` : "";
        });
      };
      // Out of the chain (up or down, DAW only): onto another track, return or the master
      // to move it there (Alt: a copy), or into the library to save it as a preset.
      const chainBox = this.element.getBoundingClientRect();
      const canLeave = typeof this.screen.deviceDropTarget === "function";
      let out = null; // { ghost, target, highlight } while outside
      const leave = (e) => {
        if (!out) {
          cards.forEach((c) => { c.style.transform = ""; });
          to = from;
          out = { ghost: html("div", "library-ghost is-effect", card.effectName ?? ""), target: null, highlight: null };
          document.body.append(out.ghost);
        }
        out.ghost.style.transform = `translate(${e.clientX + 12}px, ${e.clientY + 10}px)`;
        out.target = this.screen.deviceDropTarget(e.clientX, e.clientY, this.owner.id);
        const highlight = out.target?.element ?? null;
        if (out.highlight !== highlight) { out.highlight?.classList.remove("is-drop-target"); highlight?.classList.add("is-drop-target"); out.highlight = highlight; }
        out.ghost.classList.toggle("is-refused", !out.target);
        out.ghost.dataset.where = out.target ? (out.target.library ? out.target.label : `${e.altKey ? "Copy to" : "Move to"} ${out.target.label}`) : "";
      };
      const comeBack = () => {
        if (!out) return;
        out.ghost.remove();
        out.highlight?.classList.remove("is-drop-target");
        out = null;
      };
      const move = (e) => {
        const dx = e.clientX - startX;
        if (!dragging) {
          if (Math.abs(dx) < 5 && Math.abs(e.clientY - event.clientY) < 5) return;
          dragging = true;
          this.element.classList.add("is-reordering");
          card.element.classList.add("is-dragging");
        }
        if (canLeave && (e.clientY < chainBox.top - 24 || e.clientY > chainBox.bottom + 24)) {
          card.element.style.transform = "";
          card.element.classList.add("is-leaving");
          return leave(e);
        }
        card.element.classList.remove("is-leaving");
        comeBack();
        card.element.style.transform = `translateX(${dx}px)`;
        const centre = rects[from].left + rects[from].width / 2 + dx;
        let target = from;
        rects.forEach((r, i) => { if (i !== from && (i < from ? centre < r.left + r.width / 2 : centre > r.left + r.width / 2)) target = i < from ? Math.min(target, i) : Math.max(target, i); });
        if (target !== to) { to = target; place(); }
      };
      const finish = (e) => {
        head.removeEventListener("pointermove", move);
        head.removeEventListener("pointerup", finish);
        head.removeEventListener("pointercancel", finish);
        try { head.releasePointerCapture(event.pointerId); } catch {}
        if (!dragging) {
          if (e.type === "pointerup") this.select([id], e.shiftKey);
          return;
        }
        this.element.classList.remove("is-reordering");
        card.element.classList.remove("is-dragging", "is-leaving");
        cards.forEach((c) => { c.style.transform = ""; });
        const target = out?.target;
        comeBack();
        if (target && e.type === "pointerup") return this.dropOutside(card, id, target, e.altKey);
        if (e.type === "pointerup" && to !== from) this.kit.daw("moveInsert", { id, index: to });
      };
      head.addEventListener("pointermove", move);
      head.addEventListener("pointerup", finish);
      head.addEventListener("pointercancel", finish);
    });
  }
}

//==============================================================================
// One effect. Title bar, left to right: switch, name (its own or the effect's), A/B,
// the delay it adds, trims, ⋯ (the menu), remove. Body: the picture, the controls, the
// presets and the trims (behind their switch).
export class DeviceCard {
  constructor(insert, access, kit, verbs) {
    this.insert = insert;
    this.access = access;
    this.kit = kit;
    this.verbs = verbs;
    this.controls = []; // [{ param, widget }]
    this.switches = []; // [{ param, element }]
    this.parts = [];    // pitch: [{ power, row }], so parts that are off fold away
    this.conditional = []; // [control, element]: shown for some values of other controls
    this.view = null;
    this.trimsShown = false;
    this.effectName = effectName(insert);

    const isPlugin = insert.type === "plugin", isRack = insert.type === "rack";
    // The Live screen shows autotune, pitch shift and harmonizer as cards of their own
    // (insert.module); the DAW's pitch insert holds all three.
    const parts = isPlugin || isRack ? [] : insert.module ? MODULES.filter((m) => m.id === insert.module)
      : insert.type === "pitch" ? PITCH_PARTS : MODULES.filter((m) => MODULE_KEYS[m.id] === insert.type).slice(0, 1);
    this.single = !isPlugin && !isRack && (insert.type !== "pitch" || Boolean(insert.module));
    const main = parts[0];
    const category = CATEGORIES[main?.category] ?? CATEGORIES.cleanup;

    const wide = insert.module ? main?.wide : ["pitch", "paraEq", "multiband", "masterEq"].includes(insert.type);
    this.element = html("section", isPlugin ? "insert is-plugin" : isRack ? "insert is-rack" : wide ? "insert is-wide" : "insert");
    if (!isPlugin && !isRack) this.element.style.setProperty("--cat", category.color);
    this.element.dataset.insert = insert.id;

    // The Live screen has no activator (an effect that's off leaves the chain) and no A/B.
    const activator = kit.activator === false ? null : isPlugin ? "pluginOn" : isRack ? "rackOn" : this.single ? main.power : null;
    this.buildHead(activator, !isPlugin && !isRack && kit.ab !== false);
    this.body = html("div", "insert-body");
    this.element.append(this.head, this.body);

    if (isPlugin) this.buildPlugin();
    else if (isRack) this.rack = buildRack(this, verbs.chain);
    else this.buildEffect(parts);
    kit.decorate?.(this);

    this.element.append(this.foldedName = html("span", "insert-folded-name", this.effectName));
    this.update(insert);
  }

  get title() { return this.insert.name || this.effectName; }

  // A control's name on this card (for menus).
  controlLabel(param) {
    const control = this.element.querySelector(`[data-param="${CSS.escape(param)}"]`);
    return control?.querySelector(".name")?.textContent || control?.getAttribute("aria-label") || null;
  }

  buildHead(power, withAB) {
    const { button } = this.kit;
    const head = html("header", "insert-head");
    if (power) head.append(this.addSwitch(power, `Switch ${this.effectName} on or off (0)`));
    this.nameElement = html("span", "insert-name", this.effectName);
    this.activity = createActivity();
    head.append(this.nameElement, this.activity.element);

    // A/B: two settings of this effect to compare. Hidden until it's used (hover shows it).
    if (withAB) {
      this.abButton = button("insert-ab", "A", "", () => this.verbs.switchAB());
      head.append(this.abButton);
    }
    // The delay this effect adds (look-ahead, pitch, a plug-in's own), when it adds one.
    this.latencyBadge = html("span", "insert-latency");
    this.latencyBadge.hidden = true;
    head.append(this.latencyBadge);

    // The trims (this effect's own in and out gain) hide behind a switch; they show by
    // themselves while either is not 0 dB.
    this.trimSwitch = button("insert-trim-switch", "trim", "Show this effect's input and output gain", () => this.setTrimsShown(!this.trimsShown));
    const more = button("mini insert-more", "⋯", "More: A/B, duplicate, copy, rename…", (event) => {
      const box = event.currentTarget.getBoundingClientRect();
      this.verbs.openMenu(box.left, box.bottom + 4, event.currentTarget);
    });
    head.append(this.trimSwitch, more,
      button("mini is-danger insert-remove", "×", "Remove this effect (Delete)", () => this.verbs.remove()));
    head.title = "Click to select · Drag to move it in the chain · Double-click to fold · Right-click for more";
    this.head = head;
  }

  buildEffect(parts) {
    const insert = this.insert;
    // A picture of what the effect does; some have handles to drag.
    this.view = insert.type === "pitch" || this.kit.ownPicture?.(insert.type) ? null : createFxView(insert.type, {
      get: (param) => this.access.get(param),
      set: (param, value, first) => {
        this.access.set(param, value, first);
        this.update(this.insert);
      },
      range: (param) => this.access.info(param),
    });
    if (this.view) this.body.append(this.view.element);

    for (const part of parts) {
      const row = html("div", "insert-controls");

      if (insert.type === "pitch" && !insert.module) {
        const sub = html("div", "insert-sub");
        sub.append(this.addSwitch(part.power, `Switch ${part.name} on or off`), html("span", "", part.name));
        this.body.append(sub);
        this.parts.push({ power: part.power, row });
      }

      for (const control of part.controls) {
        if (control.liveOnly && !this.kit.live) continue; // the DAW listens to the other tracks for chords
        const widget = this.createControl(control);
        if (!widget) continue;
        if (control.param) widget.dataset.param = control.param; // macros map controls by it
        this.kit.decorateControl?.(control, widget);
        row.append(widget);
        if (control.when) this.conditional.push([control, widget]);
      }
      this.body.append(row);
    }

    this.body.append(this.createFooter(parts));
    // As wide as its controls need: a one-knob effect is a narrow card.
    const widest = Math.max(1, ...parts.map((part) => part.controls.filter((c) => ["knob", "choice", "toggle", "keys"].includes(c.type)).length));
    if (!this.element.classList.contains("is-wide")) this.element.style.setProperty("--card", `${clamp(widest * 62 + 28, 190, 290)}px`);
  }

  // An effect plug-in (VST3/AU): its name and maker, a button for its own window, its
  // switch and trims. One that isn't installed here says so; its settings are kept.
  buildPlugin() {
    const { button } = this.kit;
    const face = html("div", "plugin-face");
    this.pluginBadge = html("span", "plugin-badge");
    this.pluginMaker = html("div", "plugin-maker");
    this.pluginStatus = html("div", "plugin-status");
    this.pluginButton = button("text-button plugin-open", "Open plug-in", "", () => {
      if (this.insert.plugin?.error) this.access.rescan();
      else this.access.openPlugin();
    });
    // Configure (as in Ableton): while it's on, each control moved in the plug-in's
    // window comes onto the card as a knob. "Choose" picks them from a list instead.
    const configure = html("div", "plugin-configure");
    this.configureButton = button("text-button plugin-configure-toggle", "Configure", "Put the plug-in's controls on this card: switch on, then move them in its window", () => this.setConfiguring(!this.configuring));
    this.chooseButton = button("text-button plugin-choose", "Choose\u2026", "Pick plug-in parameters to show as knobs on this card", (event) => this.openParameterPicker(event.currentTarget));
    configure.append(this.configureButton, this.chooseButton);
    face.append(this.pluginBadge, this.pluginMaker, this.pluginStatus, this.pluginButton, configure);
    face.addEventListener("dblclick", (event) => { if (!event.target.closest("button") && !this.insert.plugin?.error) this.access.openPlugin(); });
    this.body.append(face);
    this.panelRow = html("div", "insert-controls plugin-panel");
    this.panelKey = null;
    this.body.append(this.panelRow);

    const footer = html("div", "insert-footer");
    const gainInfo = (suffix) => this.access.info(`eq${suffix}`) ?? { min: -24, max: 24, skew: 1, step: 0, label: "dB", default: 0 };
    this.gains = this.createGains("plugin", gainInfo);
    footer.append(this.gains);
    this.body.append(footer);
    this.element.style.setProperty("--card", "240px");
  }

  updatePlugin(insert) {
    const plugin = insert.plugin ?? {};
    const missing = Boolean(plugin.error);
    this.element.classList.toggle("is-missing", missing);
    this.pluginBadge.textContent = plugin.format || "Plug-in";
    this.pluginMaker.textContent = plugin.vendor || "";
    const reason = String(plugin.error ?? "").trim().replace(/([^.!?])$/, "$1.");
    this.pluginStatus.textContent = missing ? `⚠ ${reason} Its settings are kept: it plays again once it's installed and scanned.`
                                            : plugin.loaded ? "" : "Loading…";
    this.pluginStatus.hidden = !this.pluginStatus.textContent;
    this.pluginButton.textContent = missing ? "Rescan plug-ins" : plugin.windowOpen ? "Show its window" : "Open plug-in";
    this.pluginButton.disabled = !missing && !plugin.loaded;
    this.pluginButton.title = missing ? "Look for plug-ins again (after installing it)" : `Open ${plugin.name || "the plug-in"}'s own window`;
    this.configureButton.disabled = this.chooseButton.disabled = !plugin.loaded;
    if (!plugin.loaded && this.configuring) this.setConfiguring(false);
    this.configureButton.setAttribute("aria-pressed", String(Boolean(this.configuring)));
    this.element.classList.toggle("is-configuring", Boolean(this.configuring));
    if (this.configuring) {
      this.pluginStatus.textContent = "Move a control in the plug-in's window to put it here. Configure again when you're done.";
      this.pluginStatus.hidden = false;
      // A control moved since Configure was switched on: onto the card.
      const touched = plugin.lastTouched;
      if (touched && touched.index !== this.configureSeen) {
        this.configureSeen = touched.index;
        if (!(plugin.panel ?? []).some((entry) => entry.index === touched.index))
          this.access.daw("setPluginPanel", { id: insert.id, add: touched.index }).then((r) => { if (r && r.ok === false && r.error) this.kit.toast(r.error); });
      }
    }
    this.renderPanel(insert);
  }

  setConfiguring(on) {
    this.configuring = on;
    this.configureSeen = this.insert.plugin?.lastTouched?.index ?? -1;
    if (on && !this.insert.plugin?.windowOpen) this.access.openPlugin();
    this.updatePlugin(this.insert);
  }

  // The panel: the plug-in parameters chosen to show as knobs (insert values p<index>, so
  // undo, macros and automation work on them as on any knob).
  renderPanel(insert) {
    const panel = insert.plugin?.panel ?? [];
    const key = panel.map((entry) => `${entry.param}=${entry.name}`).join("|");
    if (key !== this.panelKey) {
      this.panelKey = key;
      this.panelRow.replaceChildren();
      this.controls = this.controls.filter((c) => !c.panel);
      for (const entry of panel) {
        const value = insert.values[entry.param] ?? 0;
        const knob = new this.kit.ValueKnob({
          label: entry.name.length > 12 ? `${entry.name.slice(0, 11)}\u2026` : entry.name, min: 0, max: 1, value, defaultValue: value,
          format: (v) => this.panelText(entry.param, v),
          parse: (text) => Number.parseFloat(text) / 100,
          onChange: (v, first) => this.access.set(entry.param, v, first),
        });
        knob.element.dataset.param = entry.param;
        knob.element.dataset.min = 0; // macros map it over this range
        knob.element.dataset.max = 1;
        knob.element.title = `${entry.name}. Drag or scroll \u00B7 Right-click for automation or to take it off the card`;
        this.controls.push({ param: entry.param, widget: knob, panel: true });
        this.panelRow.append(knob.element);
      }
      this.panelRow.hidden = !panel.length;
      this.element.style.setProperty("--card", `${clamp(Math.min(panel.length, 4) * 62 + 28, 240, 290)}px`);
    }
    for (const control of this.controls) if (control.panel) control.widget.update(insert.values[control.param]);
  }

  // The plug-in's own text for a panel value while it's the one in the session; else a percentage.
  panelText(param, value) {
    const entry = this.insert.plugin?.panel?.find((e) => e.param === param);
    const current = this.insert.values[param];
    if (entry?.text && Math.abs((current ?? -1) - value) < 1e-4) return entry.text;
    return `${Math.round(value * 100)}%`;
  }

  // A searchable list of the plug-in's parameters; a click puts one on the card.
  async openParameterPicker(anchor) {
    document.querySelector(".plugin-picker")?.remove();
    const list = await this.access.daw("pluginParameters", { id: this.insert.id });
    if (!Array.isArray(list) || !list.length) return this.kit.toast("This plug-in has no parameters to show.");
    const popup = html("div", "plugin-picker");
    const search = Object.assign(html("input", "plugin-picker-search"), { type: "search", placeholder: `Search ${list.length} parameters`, spellcheck: false });
    const rows = html("div", "plugin-picker-list");
    popup.append(search, rows);
    const onPanel = () => new Set((this.insert.plugin?.panel ?? []).map((entry) => entry.index));
    const render = () => {
      const query = search.value.trim().toLowerCase();
      const shown = onPanel();
      rows.replaceChildren();
      for (const parameter of list.filter((p) => !query || p.name.toLowerCase().includes(query)).slice(0, 300)) {
        const row = html("button", "plugin-picker-row");
        row.type = "button";
        row.append(html("span", "", parameter.name), html("span", "plugin-picker-text", parameter.text));
        row.classList.toggle("is-on", shown.has(parameter.index));
        row.title = shown.has(parameter.index) ? "On the card: click to take it off" : "Click to put it on the card";
        row.addEventListener("click", async () => {
          const r = await this.access.daw("setPluginPanel", shown.has(parameter.index) ? { id: this.insert.id, remove: parameter.index } : { id: this.insert.id, add: parameter.index });
          if (r && r.ok === false && r.error) this.kit.toast(r.error);
          setTimeout(render, 120);
        });
        rows.append(row);
      }
    };
    const close = () => {
      popup.remove();
      document.removeEventListener("pointerdown", outside, true);
    };
    const outside = (event) => { if (!popup.contains(event.target)) close(); };
    search.addEventListener("input", render);
    search.addEventListener("keydown", (event) => { event.stopPropagation(); if (event.key === "Escape") close(); });
    document.body.append(popup);
    const box = anchor.getBoundingClientRect();
    popup.style.left = `${clamp(box.left, 8, window.innerWidth - 300)}px`;
    popup.style.top = `${clamp(box.bottom + 4, 8, window.innerHeight - 340)}px`;
    render();
    search.focus();
    setTimeout(() => document.addEventListener("pointerdown", outside, true));
  }

  createGains(prefix, infoOf = () => undefined) {
    const gains = html("div", "insert-gains");
    for (const [suffix, label, title] of [["InGain", "In", "Input gain: the level going into this effect"],
                                          ["OutGain", "Out", "Output gain: the level coming out of this effect"]]) {
      const widget = this.createControl({ type: "knob", param: `${prefix}${suffix}`, label, bipolar: true, info: infoOf(suffix) });
      widget.title = `${title}. Drag or scroll · Double-click to reset`;
      gains.append(widget);
    }
    return gains;
  }

  setTrimsShown(shown) {
    this.trimsShown = shown;
    this.renderTrims();
  }

  renderTrims() {
    const { dbText } = this.kit;
    const prefix = this.insert.type;
    const values = ["InGain", "OutGain"].map((suffix) => this.insert.values[`${prefix}${suffix}`] ?? 0);
    const set = values.some((v) => Math.abs(v) > 0.05);
    this.gains.hidden = !(this.trimsShown || set);
    this.trimSwitch.setAttribute("aria-pressed", String(!this.gains.hidden));
    this.trimSwitch.classList.toggle("is-set", set);
    this.trimSwitch.title = set ? `Trim: in ${dbText(values[0])} dB, out ${dbText(values[1])} dB (shown while not 0 dB)`
                                : "Show this effect's input and output gain";
  }

  // Presets for this effect, and its own input and output gain.
  createFooter(parts) {
    const footer = html("div", "insert-footer");
    const ids = parts.map((part) => part.id);
    const isPitch = this.insert.type === "pitch" && !this.insert.module;
    // The pitch insert's presets also remember which of its parts are on.
    const params = () => [...ids.flatMap((id) => presetParams(id)), ...(isPitch ? parts.map((part) => part.power) : [])];

    const picker = createPresetPicker({
      getNativeFunction: Juce.getNativeFunction,
      ids,
      read: () => Object.fromEntries(params().map((param) => [param, this.access.get(param)])),
      apply: (values, fromId) => {
        // A factory preset for one part of the pitch insert switches that part on.
        if (isPitch && fromId) values[parts.find((part) => part.id === fromId).power] = 1;
        Object.entries(values).forEach(([param, value], i) => this.access.set(param, value, i === 0));
        this.update(this.insert);
      },
      defaultOf: (param) => this.access.info(param)?.default,
      defaultKey: this.kit.live ? undefined : this.insert.type,
    });

    this.presetPicker = picker;
    this.gains = this.createGains(this.insert.type);
    footer.append(picker.element, this.gains);
    return footer;
  }

  // Animates the picture and the level bars from the signal (see fxviews.js).
  live(reading, all) {
    this.rack?.live(all);
    this.view?.live(reading);
    const level = this.activity.push(reading);
    this.element.classList.toggle("is-live", Boolean(reading));
    this.element.style.setProperty("--act", level.toFixed(2));
  }

  addSwitch(param, title) {
    const element = this.kit.button("led-switch", "", title, () => {
      const on = element.getAttribute("aria-pressed") !== "true";
      element.setAttribute("aria-pressed", String(on));
      this.access.set(param, on ? 1 : 0, true);
      this.update(this.insert);
    });
    this.switches.push({ param, element });
    return element;
  }

  createControl(control) {
    // The autotune key picker and tuner, and the reverb picture, are Live-screen extras
    // (the Live screen builds them itself).
    const extra = this.kit.createExtra?.(control, this);
    if (extra !== undefined) return extra;
    if (control.type === "keys") control = { type: "choice", param: control.param, label: "Key" };
    if (control.type === "file") {
      const picker = createFilePicker({ label: control.label, load: () => this.access.file.load(), clear: () => this.access.file.clear() });
      this.filePicker = picker;
      picker.update(this.insert.file);
      return picker.element;
    }
    if (control.type !== "knob" && control.type !== "choice" && control.type !== "toggle") return null;

    const info = control.info ?? this.access.info(control.param) ?? { min: 0, max: 1, skew: 1, step: 0, label: "" };
    const value = this.insert.values[control.param] ?? info.default ?? info.min;

    if (control.type === "choice") {
      const choices = info.choices ?? [];
      // A few short options: buttons side by side; more: a dropdown.
      if (choices.length >= 2 && choices.length <= 3 && choices.join("").length <= 24) {
        const wrapper = html("div", "insert-choice");
        const group = html("div", "segmented");
        group.setAttribute("role", "radiogroup");
        group.setAttribute("aria-label", control.label);
        const buttons = choices.map((choice, index) => {
          const b = html("button", "", choice);
          b.type = "button";
          b.setAttribute("role", "radio");
          b.addEventListener("click", () => { this.access.set(control.param, index, true); this.update(this.insert); });
          group.append(b);
          return b;
        });
        const show = (v) => buttons.forEach((b, i) => {
          b.classList.toggle("is-selected", i === Math.round(v));
          b.setAttribute("aria-checked", String(i === Math.round(v)));
        });
        show(value);
        wrapper.append(html("span", "name", control.label), group);
        this.controls.push({ param: control.param, widget: { update: show } });
        return wrapper;
      }
      const wrapper = html("label", "insert-choice");
      const select = html("select", "dropdown");
      choices.forEach((choice) => select.append(new Option(choice)));
      select.addEventListener("change", () => { this.access.set(control.param, select.selectedIndex, true); this.update(this.insert); });
      wrapper.append(html("span", "name", control.label), select);
      select.selectedIndex = Math.round(value);
      this.controls.push({ param: control.param, widget: { update: (v) => { select.selectedIndex = Math.round(v); } } });
      return wrapper;
    }

    if (control.type === "toggle") {
      const pill = html("button", "pill");
      pill.append(html("span", "switch"), html("span", "", control.label));
      pill.addEventListener("click", () => {
        const on = pill.getAttribute("aria-pressed") !== "true";
        pill.setAttribute("aria-pressed", String(on));
        this.access.set(control.param, on ? 1 : 0, true);
        this.update(this.insert);
      });
      this.controls.push({ param: control.param, widget: { update: (v) => pill.setAttribute("aria-pressed", String(v >= 0.5)) } });
      return pill;
    }

    const knob = new this.kit.ValueKnob({
      label: control.label, min: info.min, max: info.max, skew: info.skew || 1, step: info.step || 0,
      unit: info.label, value, defaultValue: info.default, bipolar: control.bipolar,
      onChange: (v, first) => {
        this.access.set(control.param, v, first);
        this.view?.update();
      },
    });
    this.controls.push({ param: control.param, widget: knob });
    return knob.element;
  }

  // Its name typed in place; `commit` gets the text (Enter or leaving it; Escape cancels).
  editName(commit) {
    if (this.head.querySelector(".insert-name-entry")) return;
    const input = html("input", "insert-name-entry");
    Object.assign(input, { value: this.title, spellcheck: false, maxLength: 60 });
    input.setAttribute("aria-label", `Name for ${this.effectName}`);
    input.placeholder = this.effectName;
    let done = false;
    const finish = (save) => {
      if (done) return;
      done = true;
      input.remove();
      this.nameElement.hidden = false;
      const name = input.value.trim();
      if (save && name !== this.title) commit(name === this.effectName ? "" : name);
      this.element.focus({ preventScroll: true });
    };
    input.addEventListener("keydown", (event) => {
      event.stopPropagation();
      if (event.key === "Enter") finish(true);
      else if (event.key === "Escape") finish(false);
    });
    input.addEventListener("blur", () => finish(true));
    for (const type of ["pointerdown", "dblclick", "click"]) input.addEventListener(type, (event) => event.stopPropagation());
    this.nameElement.hidden = true;
    this.nameElement.after(input);
    input.focus();
    input.select();
  }

  update(insert) {
    this.insert = insert;
    this.access.insert = insert;
    for (const { param, element } of this.switches)
      element.setAttribute("aria-pressed", String((insert.values[param] ?? 0) >= 0.5));
    for (const { param, widget, panel } of this.controls) if (!panel) widget.update(insert.values[param]); // (the panel's: renderPanel)
    this.filePicker?.update(insert.file);
    this.view?.update();
    for (const { power, row } of this.parts) row.hidden = (insert.values[power] ?? 0) < 0.5;
    for (const [control, element] of this.conditional)
      element.hidden = !isControlShown(control, (param) => this.access.get(param));
    this.element.classList.toggle("is-off", !this.isOn);
    this.renderTrims();
    if (insert.type === "plugin") this.updatePlugin(insert);
    this.rack?.update(insert);

    // Title bar: its own name, A/B, the delay it adds.
    const title = this.title;
    if (this.nameElement.textContent !== title) this.nameElement.textContent = title;
    this.nameElement.title = insert.name ? `${insert.name} (${this.effectName})` : "";
    this.foldedName.textContent = title;
    this.element.classList.toggle("is-renamed", Boolean(insert.name));
    if (this.abButton) {
      const ab = insert.ab || "";
      this.abButton.textContent = ab === "b" ? "B" : "A";
      this.abButton.classList.toggle("is-used", Boolean(ab));
      this.abButton.setAttribute("aria-pressed", String(ab === "b"));
      this.abButton.dataset.tip = ab ? `Playing ${ab.toUpperCase()}: click to hear ${ab === "b" ? "A" : "B"} · ⋯ to copy one over the other`
                                     : "A/B: keep these settings as A and try others as B";
      this.abButton.setAttribute("aria-label", ab ? `Playing setting ${ab.toUpperCase()}, switch` : "Compare two settings (A/B)");
    }
    const latency = Number(insert.latencyMs) || 0;
    this.latencyBadge.hidden = latency < 0.05;
    this.latencyBadge.textContent = latency >= 10 ? `${Math.round(latency)} ms` : `${latency.toFixed(1)} ms`;
    this.latencyBadge.dataset.tip = `Adds ${latency.toFixed(1)} ms of delay. The other tracks are delayed to match, so it stays in time.`;
  }

  get isOn() {
    if (this.kit.activator === false) return true; // on the Live screen, a card that shows is on
    return this.switches.some(({ param }) => (this.insert.values[param] ?? 0) >= 0.5);
  }

  // On or off: every part of the pitch insert, or the effect's one switch.
  setOn(on) {
    const params = this.insert.type === "pitch" && on ? [this.switches[0].param] : this.switches.map(({ param }) => param);
    params.forEach((param, i) => this.access.set(param, on ? 1 : 0, i === 0));
    this.update(this.insert);
  }
}
