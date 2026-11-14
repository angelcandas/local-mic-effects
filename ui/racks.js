// Racks (DAW): parallel chains of effects as one card in a chain, the way Ableton's
// Audio Effect Rack works. The engine side is src/daw/RackInsert (each chain gets the
// rack's input; their outputs are aligned, panned, given their gain and summed).
//
// The card: the chain list on the left (name, on, solo, gain, pan; click one to show it,
// double-click its name to rename, right-click for more), the shown chain's effects on
// the right, as a chain of their own (the same cards, keys and menus as any chain).
// The rack's values are chain<n>Gain / Pan / On / Solo on the rack insert, so undo,
// macros and A/B treat them like any effect value.

import { DeviceChain } from "./devices.js";
import { MODULE_KEYS, MODULES } from "./modules.js";
import { html } from "./widgets.js";

const GAIN = { min: -60, max: 12, skew: 1, step: 0.1, label: "dB", default: 0 };
const PAN = { min: -100, max: 100, skew: 1, step: 1, label: "", default: 0 };

// The screen as a chain inside a rack sees it: effects are added into that rack's chain.
function chainScreen(screen, kit, rackId, chainIndex) {
  const inner = Object.create(screen);
  inner.addEffect = async (trackId, moduleId, index = -1) => {
    const module = MODULES.find((m) => m.id === moduleId);
    if (!module) return -1;
    const id = await kit.daw("addInsert", { rack: rackId, chain: chainIndex, type: MODULE_KEYS[module.id], index });
    if (id >= 0 && MODULE_KEYS[module.id] === "pitch" && module.id !== "tune") {
      await kit.daw("setInsert", { id, param: "tuneOn", value: 0, continue: true });
      await kit.daw("setInsert", { id, param: module.power, value: 1, continue: true });
    }
    return id;
  };
  inner.addPluginEffect = async () => {
    kit.toast("Add the plug-in to the track first, then drag it into the rack (or group it).");
    return -1;
  };
  inner.copyEffects = () => kit.toast("Copy the effects (select them, Ctrl/Cmd+C) and paste them into another chain.");
  return inner;
}

// Fills a DeviceCard for a rack insert. `outer`: the chain the rack is in (its screen,
// kit and owner). Returns { update (insert), live (readings) }.
export function buildRack(card, outer) {
  const { kit } = card;
  const screen = outer?.screen ?? {};
  const shown = (screen.rackChains ??= new Map()); // rack id -> the chain it shows
  const rackId = card.insert.id;

  const body = html("div", "rack-body");
  const list = html("div", "rack-chains");
  const devices = html("div", "rack-devices");
  body.append(list, devices);
  card.body.append(body);

  // The rack's own trims, like any effect's.
  const footer = html("div", "insert-footer");
  card.gains = card.createGains("rack", () => GAIN);
  footer.append(card.gains);
  card.body.append(footer);

  let rows = [];
  let inner = null, innerIndex = -1;
  const chainOf = () => Math.min(shown.get(rackId) ?? 0, Math.max(0, (card.insert.chains?.length ?? 1) - 1));

  const show = (index) => {
    shown.set(rackId, index);
    update(card.insert);
  };

  const renameChain = (index, nameElement) => {
    const input = Object.assign(html("input", "rack-chain-name-entry"), { value: card.insert.chains[index]?.name ?? "", maxLength: 40 });
    let done = false;
    const finish = (save) => {
      if (done) return;
      done = true;
      input.replaceWith(nameElement);
      if (save) kit.daw("renameRackChain", { id: rackId, chain: index, name: input.value });
    };
    input.addEventListener("keydown", (event) => {
      event.stopPropagation();
      if (event.key === "Enter") finish(true);
      else if (event.key === "Escape") finish(false);
    });
    input.addEventListener("blur", () => finish(true));
    for (const type of ["pointerdown", "dblclick", "click"]) input.addEventListener(type, (event) => event.stopPropagation());
    nameElement.replaceWith(input);
    input.focus();
    input.select();
  };

  const chainMenu = (index, x, y, nameElement) => {
    kit.popMenu(x, y, ({ item, rule }) => {
      item("Rename", () => renameChain(index, nameElement));
      item("Add a chain", () => kit.daw("addRackChain", { id: rackId }).then((r) => r?.ok && show(r.chain)));
      rule();
      item("Remove this chain", () => kit.daw("removeRackChain", { id: rackId, chain: index }),
           { danger: true, disabled: (card.insert.chains?.length ?? 0) <= 1, hint: "With its effects" });
    });
  };

  // One row per chain: built again when the number of chains changes.
  const buildRows = (count) => {
    list.replaceChildren();
    rows = [];
    card.controls = card.controls.filter((c) => !c.rackRow);
    for (let index = 0; index < count; ++index) {
      const n = index + 1;
      const row = html("div", "rack-chain");
      const name = html("span", "rack-chain-name");
      name.title = "Click to show its effects · Double-click to rename · Right-click for more";
      const on = kit.button("led-switch rack-chain-on", "", "Switch this chain on or off", () => {
        card.access.set(`chain${n}On`, (card.insert.values[`chain${n}On`] ?? 1) >= 0.5 ? 0 : 1, true);
        update(card.insert);
      });
      const solo = kit.button("mini rack-chain-solo", "S", "Solo: hear only the soloed chains", () => {
        card.access.set(`chain${n}Solo`, (card.insert.values[`chain${n}Solo`] ?? 0) >= 0.5 ? 0 : 1, true);
        update(card.insert);
      });
      const before = card.controls.length;
      const gain = card.createControl({ type: "knob", param: `chain${n}Gain`, label: "Gain", info: GAIN });
      const pan = card.createControl({ type: "knob", param: `chain${n}Pan`, label: "Pan", bipolar: true, info: PAN });
      card.controls.slice(before).forEach((c) => { c.rackRow = true; });
      for (const [knob, range] of [[gain, GAIN], [pan, PAN]]) {
        knob.classList.add("rack-chain-knob");
        knob.dataset.min = range.min; // macros map them over this range
        knob.dataset.max = range.max;
      }
      gain.dataset.param = `chain${n}Gain`;
      pan.dataset.param = `chain${n}Pan`;
      row.append(on, name, solo, gain, pan);
      row.addEventListener("click", (event) => { if (!event.target.closest("button, .knob, input")) show(index); });
      name.addEventListener("dblclick", (event) => { event.stopPropagation(); renameChain(index, name); });
      row.addEventListener("contextmenu", (event) => {
        event.preventDefault();
        event.stopPropagation();
        chainMenu(index, event.clientX, event.clientY, name);
      });
      list.append(row);
      rows.push({ row, name, on, solo });
    }
    const add = kit.button("text-button rack-chain-add", "+ Chain", "Add a chain beside these (it gets the rack's input too)",
                           () => kit.daw("addRackChain", { id: rackId }).then((r) => r?.ok && show(r.chain)));
    list.append(add);
  };

  const update = (insert) => {
    const chains = insert.chains ?? [];
    if (rows.length !== chains.length) buildRows(chains.length);
    const current = chainOf();
    const anySolo = chains.some((_, i) => (insert.values[`chain${i + 1}Solo`] ?? 0) >= 0.5);
    chains.forEach((chain, i) => {
      const { row, name, on, solo } = rows[i];
      const isOn = (insert.values[`chain${i + 1}On`] ?? 1) >= 0.5, isSolo = (insert.values[`chain${i + 1}Solo`] ?? 0) >= 0.5;
      const label = chain.name || `Chain ${i + 1}`;
      if (name.textContent !== label) name.textContent = label;
      on.setAttribute("aria-pressed", String(isOn));
      solo.setAttribute("aria-pressed", String(isSolo));
      row.classList.toggle("is-shown", i === current);
      row.classList.toggle("is-silent", !isOn || (anySolo && !isSolo));
      name.dataset.count = chain.inserts.length ? `${chain.inserts.length} fx` : "empty";
    });

    // The shown chain's effects, as a chain of their own.
    const owner = { id: outer?.owner?.id ?? 0, name: outer?.owner?.name, rack: rackId, chain: current, inserts: chains[current]?.inserts ?? [] };
    if (!inner || innerIndex !== current) {
      innerIndex = current;
      devices.replaceChildren();
      const element = html("div", "rack-device-chain");
      devices.append(element);
      inner = new DeviceChain(element, owner, chainScreen(screen, kit, rackId, current), kit);
    }
    inner.update(owner);
  };

  return {
    update,
    live: (readings) => inner?.live(readings),
  };
}
