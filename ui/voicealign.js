// Align to lead: line clips (a double, a harmony, a re-sung line) up in time with a lead
// track, syllable by syllable, and optionally tune their notes to it. The clip editor's
// Align section holds every control; the track menu and the Assist menu run it with the
// same settings (remembered). The engine side is daw::voicealign (command "alignVoice").
import { html } from "./widgets.js";
import { prefs } from "./rhythm.js";

// The tracks that can lead, other than `trackId`'s: audio tracks with clips.
export function leadCandidates(session, trackId) {
  return (session?.tracks ?? []).filter((t) => t.id !== trackId && t.kind !== "midi" && t.clips?.length);
}

// The lead to offer first: the one picked last time, one called "lead", one called "vocal"
// that isn't a double or a harmony, else the first one.
export function defaultLead(session, trackId) {
  const candidates = leadCandidates(session, trackId);
  const saved = prefs.get("alignLead", null);
  const backing = /double|dbl|harm|backing|bv|choir/i;
  return candidates.find((t) => t.id === saved)
    ?? candidates.find((t) => /lead/i.test(t.name))
    ?? candidates.find((t) => /vocal|voice|vox|sing/i.test(t.name) && !backing.test(t.name))
    ?? candidates.find((t) => !backing.test(t.name))
    ?? candidates[0] ?? null;
}

// The remembered settings as the command's arguments.
export function alignArgs() {
  return {
    strength: prefs.get("alignStrength", 100) / 100,
    maxShift: prefs.get("alignMaxShift", 250) / 1000,
    maxStretch: prefs.get("alignStretch", 150) / 100,
    timing: prefs.get("alignTiming", true),
    pitch: prefs.get("alignPitch", false),
  };
}

// Runs it: `target` is { clips: [...] } or { track }. Toasts what happened.
export async function alignToLead(ui, target, guideId) {
  const args = alignArgs();
  if (!args.timing && !args.pitch) {
    ui.toast("Tick Timing, Pitch or both in the clip editor's Align section first.");
    return null;
  }
  prefs.set("alignLead", guideId);
  const result = await ui.daw("alignVoice", { ...target, guide: guideId, ...args });
  if (result?.message) ui.toast(result.message);
  return result;
}

// The clip editor's Align section. options: target() (the clips), trackId() (the clip's track).
export function createAlignControls(screen, ui, options) {
  const { ValueKnob, button } = ui;

  const lead = html("select", "dropdown voice-align-lead");
  lead.title = "The track that sings the lead: the clips are lined up with it as it plays";
  const fillLeads = () => {
    const keep = Number(lead.value || -1);
    const candidates = leadCandidates(screen.session, options.trackId());
    lead.replaceChildren(...candidates.map((t) => new Option(t.name, t.id)));
    const pick = candidates.find((t) => t.id === keep) ?? defaultLead(screen.session, options.trackId());
    if (pick) lead.value = String(pick.id);
    lead.disabled = !candidates.length;
    go.disabled = !candidates.length;
  };
  lead.addEventListener("change", () => prefs.set("alignLead", Number(lead.value)));
  const leadLabel = html("label", "voice-align-row");
  leadLabel.append(html("span", "panel-caption", "Lead"), lead);

  const knob = (key, label, min, max, fallback, format, title) => {
    const k = new ValueKnob({ label, min, max, step: 1, value: prefs.get(key, fallback), defaultValue: fallback, format,
                              onChange: (v) => prefs.set(key, Math.round(v)) });
    k.element.title = title;
    return k.element;
  };
  const knobs = html("div", "insert-controls rhythm-knobs");
  knobs.append(
    knob("alignStrength", "Strength", 0, 100, 100, (v) => `${Math.round(v)}%`, "How far the clips move towards the lead (less keeps some of their own feel)"),
    knob("alignMaxShift", "Max move", 20, 1000, 250, (v) => `${Math.round(v)} ms`, "The farthest a syllable moves, earlier or later"),
    knob("alignStretch", "Max stretch", 105, 400, 150, (v) => `${(v / 100).toFixed(2)}×`,
         "While they sing, no stretch plays faster or slower than this (lower sounds more natural, higher follows more closely)"));

  const check = (key, text, fallback, title) => {
    const label = html("label", "rhythm-check");
    const box = html("input");
    Object.assign(box, { type: "checkbox", checked: prefs.get(key, fallback) });
    box.addEventListener("change", () => prefs.set(key, box.checked));
    label.append(box, document.createTextNode(` ${text}`));
    label.title = title;
    return label;
  };
  const checks = html("div", "panel-actions");
  checks.append(
    check("alignTiming", "Timing", true, "Warp the clips so every syllable starts and ends with the lead's"),
    check("alignPitch", "Pitch", false, "Tune each sung note to the lead's note there (in its own octave; notes more than 2 semitones off, like a harmony, stay)"));

  const buttons = html("div", "panel-actions");
  const go = button("tool is-accent", "Align", "Line the clip up with the lead, as one undo step", async () => {
    if (!lead.value) return;
    go.disabled = true;
    go.textContent = "Aligning…";
    try { await alignToLead(ui, options.target(), Number(lead.value)); } finally { go.textContent = "Align"; go.disabled = false; }
  });
  const reset = button("tool", "Reset timing", "Put the clip's timing back as recorded: every warp tab goes (its pitch stays)",
    () => ui.daw("clearWarp", options.target()));
  buttons.append(go, reset);

  const caption = html("p", "panel-caption",
    "For doubles, harmonies and re-sung lines: each syllable moves to where the lead sings it. Gaps between phrases take up the slack; the take itself stays as recorded.");
  fillLeads();
  return { elements: [leadLabel, knobs, checks, buttons, caption], reset, update: fillLeads };
}
