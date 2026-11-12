// Plugin-style chrome for the AI tools (Auto Mix, Stem splitter): a segmented mode switch
// built from the panel's own <select> (which stays the source of truth), grouped effect
// chips, and progress mirrored onto the panel so CSS can animate the stem lanes and meters.
const MODES = [
  ["mix", "Mix", "Mix the whole song"],
  ["fit", "Fit", "Fit one track into the backing"],
  ["clean", "Clean", "Clean & polish one track"],
  ["master", "Master", "Master the song"],
];

export function decorateAiPanels(root) {
  const mix = root.querySelector(".automix-panel");
  const stems = root.querySelector(".stems-panel");
  if (mix) decorateAutoMix(mix);
  for (const panel of [mix, stems]) if (panel) mirrorProgress(panel);
}

function mirrorProgress(panel) {
  const bar = panel.querySelector(".export-bar");
  if (!bar) return;
  const copy = () => panel.style.setProperty("--progress", bar.style.getPropertyValue("--progress") || "0");
  new MutationObserver(copy).observe(bar, { attributes: true, attributeFilter: ["style"] });
  copy();
}

function decorateAutoMix(panel) {
  const select = panel.querySelector("[name=mode]");
  const strip = panel.querySelector(".ai-modes");
  if (!select || !strip) return;
  select.closest("label").classList.add("ai-hidden");

  for (const [value, label, title] of MODES) {
    const tab = document.createElement("button");
    tab.type = "button";
    tab.className = "ai-mode";
    tab.dataset.mode = value;
    tab.title = title;
    tab.setAttribute("role", "tab");
    tab.textContent = label;
    tab.addEventListener("click", () => {
      if (select.value === value) return;
      select.value = value;
      select.dispatchEvent(new Event("change", { bubbles: true }));
      sync();
    });
    strip.append(tab);
  }

  const sync = () => {
    for (const tab of strip.children) {
      const on = tab.dataset.mode === select.value;
      tab.classList.toggle("is-on", on);
      tab.setAttribute("aria-selected", String(on));
    }
    panel.dataset.mode = select.value;
  };
  // The bridge sets the select from a plan (and renderAutoMixMode toggles classes): follow it.
  new MutationObserver(sync).observe(panel, { attributes: true, attributeFilter: ["class", "hidden"] });
  select.addEventListener("change", sync);
  sync();

  // Group the "May add" effects into labelled chips; the first group row is a heading.
  for (const group of panel.querySelectorAll(".automix-effects")) group.classList.add("ai-chips");
}
