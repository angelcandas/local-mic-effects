// Live macros: the same eight-knob panel as the DAW's (macros.js), above the Live cards.
// A macro is a host parameter (macro1..macro8), so hosts can automate it and scenes and
// setlist songs keep it; the mappings and names live in the plug-in state.
//
// Each Live card is one effect (data-insert is its module id) and every value id is
// unique, so a mapping only needs the value; the card is worked out from MODULES.

import * as Juce from "./juce/index.js";
import { MODULES } from "./modules.js";
import { MacroPanel } from "./macros.js";
import { html } from "./widgets.js";

const moduleOf = (param) => MODULES.find((m) => !m.dawOnly && (m.controls ?? []).some((c) => c.param === param));

export function mountLiveMacros({ parent, before, grid, kit, parameterInfo, setLive }) {
  const native = Juce.getNativeFunction("liveMacros");
  const owner = { id: 0, macros: [], inserts: MODULES.filter((m) => !m.dawOnly).map((m) => ({ id: m.id, name: m.name })) };
  const wrap = html("div", "live-macros");

  let panel = null;
  const show = (macros) => {
    if (!Array.isArray(macros)) return;
    owner.macros = macros.map((macro) => ({ ...macro, maps: macro.maps.map((map) => ({ ...map, insert: moduleOf(map.param)?.id ?? "" })) }));
    panel.update(owner);
    if (owner.macros.some((m) => m.maps.length)) setOpen(true);
  };
  const call = (...args) => native(...args).then((result) => {
    if (Array.isArray(result)) show(result);
    return result;
  });
  const refresh = () => call("get").catch(() => {});

  // The DAW's commands, done the Live way.
  const daw = (command, args) => {
    if (command === "setMacro") return Promise.resolve(setLive(`macro${args.index + 1}`, args.value * 100, !args.continue));
    if (command === "mapMacro") {
      return native("map", args.index, args.param, args.min, args.max)
        .then((ok) => { refresh(); return ok ? { ok: true } : { ok: false, error: "That control can't follow a macro." }; });
    }
    if (command === "unmapMacro") return native("unmap", args.param).then(refresh);
    if (command === "nameMacro") return call("name", args.index, args.name);
    return Promise.resolve(null);
  };

  panel = new MacroPanel(owner, { ...kit, daw, parameterInfo: () => parameterInfo }, () => grid);

  // The panel folds to its title until it's opened (or something is mapped).
  const setOpen = (open) => {
    wrap.classList.toggle("is-open", open);
    toggle.setAttribute("aria-expanded", String(open));
    try { localStorage.setItem("micfx.liveMacros", open ? "1" : "0"); } catch {}
    if (!open && panel.mapping) panel.setMapping(false);
  };
  const toggle = html("button", "live-macros-toggle", "Macros");
  toggle.type = "button";
  toggle.title = "Eight knobs that each turn several of your effects' controls at once";
  toggle.addEventListener("click", () => setOpen(!wrap.classList.contains("is-open")));
  panel.element.querySelector(".macro-title")?.replaceWith(toggle);
  wrap.append(panel.element);
  parent.insertBefore(wrap, before);
  let saved = null;
  try { saved = localStorage.getItem("micfx.liveMacros"); } catch {}
  setOpen(saved === "1");

  // The host, a scene or undo moves the macros too.
  for (let index = 0; index < 8; ++index) {
    Juce.getSliderState(`macro${index + 1}`).valueChangedEvent.addListener(() => {
      const macro = owner.macros[index];
      if (!macro) return;
      macro.value = Juce.getSliderState(`macro${index + 1}`).getScaledValue() / 100;
      panel.update(owner);
    });
  }
  // A card that shows again redraws its controls: put the dots back.
  new MutationObserver(() => panel.markControls()).observe(grid, { subtree: true, attributes: true, attributeFilter: ["hidden"] });
  refresh();
  return { refresh, panel };
}
