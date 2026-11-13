// Presets for single effects, used on every effect card (Live) and insert (DAW).
//
// FX_PRESETS: factory settings by effect id (ui/modules.js MODULES[].id), each
// { name, values: { parameterId: value in real units } }. A preset sets the effect's
// controls; anything it doesn't list goes back to its default. It never touches the
// effect's power or its in/out gain.
//
// Your own presets are saved with the "uiPref" native function (in the app's settings),
// so Live and DAW share them.
import { MODULES } from "./modules.js";
import { html } from "./widgets.js";

const p = (name, values) => ({ name, values });

export const FX_PRESETS = {
  hpf: [p("Gentle (60 Hz)", { hpfFreq: 60 }), p("Vocal (100 Hz)", { hpfFreq: 100 }), p("Thin (180 Hz)", { hpfFreq: 180 }),
        p("Handheld mic (140 Hz)", { hpfFreq: 140 })],
  denoise: [p("Light (natural)", { denoiseReduction: 8, denoiseThreshold: 0, denoiseAdaptive: 1 }),
            p("Podcast", { denoiseReduction: 15, denoiseThreshold: 0, denoiseAdaptive: 1 }),
            p("Noisy room", { denoiseReduction: 22, denoiseThreshold: 3, denoiseAdaptive: 1 }),
            p("Hum and hiss only (gentle)", { denoiseReduction: 12, denoiseThreshold: -4, denoiseAdaptive: 1 })],
  smooth: [p("Gentle (any voice)", { smoothDepth: 5, smoothThreshold: 7, smoothSharpness: 50, smoothFocus: 0, smoothLowHz: 150, smoothHighHz: 16000 }),
           p("Harsh vocal", { smoothDepth: 9, smoothThreshold: 5, smoothSharpness: 60, smoothFocus: 40, smoothLowHz: 1500, smoothHighHz: 10000 }),
           p("Boxy room tone", { smoothDepth: 8, smoothThreshold: 5, smoothSharpness: 40, smoothFocus: -50, smoothLowHz: 150, smoothHighHz: 1200, smoothRelease: 200 }),
           p("Cymbal ring", { smoothDepth: 10, smoothThreshold: 5, smoothSharpness: 75, smoothFocus: 60, smoothLowHz: 3000, smoothHighHz: 18000, smoothRelease: 250 }),
           p("Master polish", { smoothDepth: 3, smoothThreshold: 8, smoothSharpness: 50, smoothFocus: 0, smoothLowHz: 100, smoothHighHz: 16000, smoothResolution: 2 })],
  gate: [p("Quiet room", { gateThreshold: -60 }), p("Normal", { gateThreshold: -50 }), p("Noisy stage", { gateThreshold: -38 }),
         p("Tight", { gateThreshold: -30 })],
  tune: [p("Natural", { tuneSpeed: 60, tuneAmount: 60 }), p("Pop polish", { tuneSpeed: 25, tuneAmount: 90 }),
         p("Hard tune", { tuneSpeed: 0, tuneAmount: 100 }), p("Slow drift", { tuneSpeed: 140, tuneAmount: 80 })],
  pitch: [p("Deep voice", { pitchShift: -5, pitchFormant: -3 }), p("Chipmunk", { pitchShift: 7, pitchFormant: 5 }),
          p("Monster", { pitchShift: -12, pitchFormant: -6 }), p("Octave up", { pitchShift: 12, pitchFormant: 0 }),
          p("Formant only (younger)", { pitchShift: 0, pitchFormant: 3 })],
  harmony: [p("Major third + fifth", { harmonyVoice1: 4, harmonyVoice2: 7, harmonyMix: 50 }),
            p("Minor third + fifth", { harmonyVoice1: 3, harmonyVoice2: 7, harmonyMix: 50 }),
            p("Octaves", { harmonyVoice1: -12, harmonyVoice2: 12, harmonyMix: 40 }),
            p("Power fifths", { harmonyVoice1: -5, harmonyVoice2: 7, harmonyMix: 45 }),
            // Following the song's chords (mode 2); voices 0-based (3 = four voices).
            p("Chord choir above", { harmonyMode: 2, harmonyVoices: 1, harmonyStyle: 0, harmonyMix: 50, harmonyHumanize: 40, harmonySpread: 60 }),
            p("Chord choir around", { harmonyMode: 2, harmonyVoices: 2, harmonyStyle: 2, harmonyMix: 50, harmonyHumanize: 45, harmonySpread: 70 }),
            p("Gospel stack (4 voices)", { harmonyMode: 2, harmonyVoices: 3, harmonyStyle: 3, harmonyMix: 55, harmonyHumanize: 55, harmonySpread: 80 }),
            p("Close harmony below", { harmonyMode: 2, harmonyVoices: 1, harmonyStyle: 1, harmonyMix: 45, harmonyHumanize: 30, harmonySpread: 40 }),
            // In the autotune key (mode 1): a third and a fifth that fit the scale.
            p("Thirds in the key", { harmonyMode: 1, harmonyVoices: 0, harmonyVoice1: 4, harmonyMix: 50, harmonyHumanize: 30 }),
            p("Third + fifth in the key", { harmonyMode: 1, harmonyVoices: 1, harmonyVoice1: 4, harmonyVoice2: 7, harmonyMix: 50, harmonyHumanize: 35 })],
  eq: [p("Warm", { eqLow: 3, eqMidFreq: 2500, eqMidGain: -1, eqHigh: -1 }),
       p("Presence", { eqLow: -1, eqMidFreq: 3000, eqMidGain: 4, eqHigh: 2 }),
       p("Air", { eqLow: 0, eqMidFreq: 2500, eqMidGain: 0, eqHigh: 5 }),
       p("Less boxy", { eqLow: -2, eqMidFreq: 500, eqMidGain: -4, eqHigh: 1 }),
       p("Radio voice", { eqLow: 4, eqMidFreq: 1500, eqMidGain: 2, eqHigh: 3 })],
  peq: [p("Clean vocal", { peqLowCutOn: 1, peqLowCutFreq: 90, peqBell1Freq: 300, peqBell1Gain: -3, peqBell1Q: 1.2,
                           peqBell2Freq: 3000, peqBell2Gain: 2, peqBell2Q: 1, peqHighFreq: 10000, peqHighGain: 2 }),
        p("De-mud", { peqLowCutOn: 1, peqLowCutFreq: 100, peqBell1Freq: 250, peqBell1Gain: -5, peqBell1Q: 1.4 }),
        p("Bright pop", { peqLowCutOn: 1, peqLowCutFreq: 120, peqBell2Freq: 4000, peqBell2Gain: 3, peqBell2Q: 0.8,
                          peqHighFreq: 12000, peqHighGain: 4 }),
        p("Telephone", { peqLowCutOn: 1, peqLowCutFreq: 400, peqHighCutOn: 1, peqHighCutFreq: 3400,
                         peqBell2Freq: 1500, peqBell2Gain: 6, peqBell2Q: 1 }),
        p("Notch the ring (2.5k)", { peqBell2Freq: 2500, peqBell2Gain: -9, peqBell2Q: 8 })],
  comp: [p("Gentle", { compThreshold: -16, compRatio: 2 }), p("Vocal", { compThreshold: -20, compRatio: 3.5 }),
         p("Radio", { compThreshold: -26, compRatio: 6 }), p("Squash", { compThreshold: -34, compRatio: 12 })],
  deess: [p("Light", { deessThreshold: -24, deessFreq: 6500 }), p("Medium", { deessThreshold: -30, deessFreq: 6000 }),
          p("Strong", { deessThreshold: -38, deessFreq: 5500 }), p("Deep voice", { deessThreshold: -32, deessFreq: 4500 })],
  exciter: [p("Sparkle", { exciterFreq: 5000, exciterAmount: 30 }), p("Presence", { exciterFreq: 2500, exciterAmount: 35 }),
            p("Air", { exciterFreq: 8000, exciterAmount: 45 })],
  sat: [p("Warm tape", { satDrive: 6, satMix: 60 }), p("Tube", { satDrive: 12, satMix: 100 }),
        p("Crunch", { satDrive: 22, satMix: 100 }), p("Fuzz", { satDrive: 30, satMix: 100 })],
  radio: [p("Telephone", { radioLow: 400, radioHigh: 3400 }), p("Walkie-talkie", { radioLow: 700, radioHigh: 2600 }),
          p("AM radio", { radioLow: 200, radioHigh: 4500 }), p("Megaphone", { radioLow: 900, radioHigh: 2200 })],
  crush: [p("8-bit", { crushBits: 8, crushRate: 8000, crushMix: 100 }), p("Retro console", { crushBits: 5, crushRate: 11000, crushMix: 100 }),
          p("Grit", { crushBits: 10, crushRate: 16000, crushMix: 40 }), p("Broken", { crushBits: 3, crushRate: 3000, crushMix: 100 })],
  robot: [p("Dalek", { robotFreq: 30, robotMix: 100 }), p("Android", { robotFreq: 80, robotMix: 80 }),
          p("Metallic", { robotFreq: 300, robotMix: 60 }), p("Subtle buzz", { robotFreq: 50, robotMix: 30 })],
  filter: [p("Dark", { filterType: 0, filterCutoff: 900, filterResonance: 20 }),
           p("Thin", { filterType: 1, filterCutoff: 700, filterResonance: 15 }),
           p("Telephone band", { filterType: 2, filterCutoff: 1500, filterResonance: 40 }),
           p("Squelch", { filterType: 0, filterCutoff: 1400, filterResonance: 80 })],
  wah: [p("Funky", { wahSensitivity: 70, wahFreq: 400, wahMix: 100 }), p("Subtle", { wahSensitivity: 40, wahFreq: 500, wahMix: 50 }),
        p("Quack", { wahSensitivity: 90, wahFreq: 300, wahMix: 100 })],
  tremolo: [p("Slow pulse", { tremoloRate: 3, tremoloDepth: 50 }), p("Classic", { tremoloRate: 6, tremoloDepth: 60 }),
            p("Helicopter", { tremoloRate: 14, tremoloDepth: 90 })],
  vibrato: [p("Gentle", { vibratoRate: 5, vibratoDepth: 20 }), p("Opera", { vibratoRate: 6, vibratoDepth: 45 }),
            p("Warble", { vibratoRate: 9, vibratoDepth: 70 })],
  chorus: [p("Subtle", { chorusRate: 0.5, chorusDepth: 25, chorusMix: 30 }), p("Lush", { chorusRate: 0.8, chorusDepth: 45, chorusMix: 50 }),
           p("Wide wobble", { chorusRate: 2, chorusDepth: 70, chorusMix: 60 })],
  flanger: [p("Jet", { flangerRate: 0.2, flangerDepth: 80, flangerFeedback: 75, flangerMix: 50 }),
            p("Gentle sweep", { flangerRate: 0.15, flangerDepth: 50, flangerFeedback: 40, flangerMix: 35 }),
            p("Metal tube", { flangerRate: 1.5, flangerDepth: 60, flangerFeedback: 90, flangerMix: 60 })],
  phaser: [p("Slow swirl", { phaserRate: 0.25, phaserDepth: 70, phaserFeedback: 40, phaserMix: 50 }),
           p("Space", { phaserRate: 0.6, phaserDepth: 90, phaserFeedback: 70, phaserMix: 60 }),
           p("Fast shimmer", { phaserRate: 3, phaserDepth: 50, phaserFeedback: 30, phaserMix: 40 })],
  doubler: [p("Natural double", { doublerMix: 40, doublerSpread: 60 }), p("Wide", { doublerMix: 55, doublerSpread: 100 }),
            p("Thick", { doublerMix: 70, doublerSpread: 80 })],
  echo: [p("Slapback", { delayTime: 110, delayFeedback: 10, delayMix: 25, delayPingPong: 0 }),
         p("Quarter note (120 BPM)", { delayTime: 500, delayFeedback: 35, delayMix: 25, delayPingPong: 0 }),
         p("Ping-pong", { delayTime: 375, delayFeedback: 45, delayMix: 30, delayPingPong: 1 }),
         p("Dub", { delayTime: 600, delayFeedback: 70, delayMix: 35, delayPingPong: 0 }),
         p("Canyon", { delayTime: 900, delayFeedback: 55, delayMix: 30, delayPingPong: 1 })],
  reverb: [p("Small room", { reverbMix: 15, reverbSize: 25, reverbDamping: 60, reverbWidth: 80 }),
           p("Plate", { reverbMix: 25, reverbSize: 50, reverbDamping: 35, reverbWidth: 100 }),
           p("Hall", { reverbMix: 30, reverbSize: 80, reverbDamping: 45, reverbWidth: 100 }),
           p("Cathedral", { reverbMix: 40, reverbSize: 100, reverbDamping: 30, reverbWidth: 100 }),
           p("Dark ambience", { reverbMix: 35, reverbSize: 70, reverbDamping: 85, reverbWidth: 90 })],
  width: [p("A little wider", { widthAmount: 130 }), p("Wide", { widthAmount: 170 }), p("Mono", { widthAmount: 0 })],
};

// The parameters a preset of this effect sets: its knobs, switches and lists (not the
// autotune key, which follows the song, and not the power or the in/out gain).
export function presetParams(id) {
  const module = MODULES.find((m) => m.id === id);
  return (module?.controls ?? [])
    .filter((c) => ["knob", "toggle", "choice"].includes(c.type) && c.param !== "tuneScale")
    .map((c) => c.param);
}

//==============================================================================
// Your own presets: { effectId: [{ name, values }] }, kept in the app's settings.
let userPresets = {};
let loaded = null;
let version = 0; // bumped on every change, so other cards' menus catch up

function loadUserPresets(getNativeFunction) {
  loaded ??= getNativeFunction("uiPref")("fxPresets")
    .then((text) => { try { userPresets = JSON.parse(text || "{}") ?? {}; } catch { userPresets = {}; } ++version; })
    .catch(() => {});
  return loaded;
}

function saveUserPresets(getNativeFunction) {
  ++version;
  getNativeFunction("uiPref")("fxPresets", JSON.stringify(userPresets)).catch(() => {});
}

// A preset menu for one effect card.
//   getNativeFunction: Juce.getNativeFunction
//   ids: the effect ids it covers (the DAW's pitch insert covers autotune, pitch and harmony)
//   read (): the current values to save
//   apply (values, id): set them; id is the effect a factory preset belongs to (its other
//     controls are filled in with their defaults), or null for one of yours
//   defaultOf (param): a parameter's default
export function createPresetPicker({ getNativeFunction, ids, read, apply, defaultOf }) {
  const element = html("div", "fx-presets");
  const select = html("select", "dropdown fx-preset-select");
  select.title = "Presets for this effect";
  const nameRow = html("div", "fx-preset-save");
  const nameInput = Object.assign(html("input", "fx-preset-name"), { placeholder: "Preset name", maxLength: 40, spellcheck: false });
  const saveButton = html("button", "tool", "Save");
  const cancelButton = html("button", "tool", "×");
  saveButton.type = cancelButton.type = "button";
  cancelButton.title = "Cancel";
  nameRow.append(nameInput, saveButton, cancelButton);
  nameRow.hidden = true;
  element.append(select, nameRow);

  const key = ids.join("+");
  const factory = ids.flatMap((id) => (FX_PRESETS[id] ?? []).map((preset) => ({ ...preset, id })));

  let builtVersion = -1;
  const build = () => {
    builtVersion = version;
    select.replaceChildren(new Option("Presets…", ""));
    const group = (label, presets, prefix) => {
      if (!presets.length) return;
      const g = document.createElement("optgroup");
      g.label = label;
      presets.forEach((preset, i) => g.append(new Option(preset.name, `${prefix}${i}`)));
      select.append(g);
    };
    if (ids.length > 1) {
      // One group per part (the DAW's pitch insert: autotune, pitch shift, harmonizer).
      for (const id of ids) {
        const g = document.createElement("optgroup");
        g.label = MODULES.find((m) => m.id === id)?.name ?? id;
        factory.forEach((preset, i) => { if (preset.id === id) g.append(new Option(preset.name, `f${i}`)); });
        select.append(g);
      }
    } else {
      group("Factory", factory, "f");
    }
    group("Yours", userPresets[key] ?? [], "u");
    const actions = document.createElement("optgroup");
    actions.label = "Manage";
    actions.append(new Option("Save current as…", "save"));
    (userPresets[key] ?? []).forEach((preset, i) => actions.append(new Option(`Delete “${preset.name}”`, `d${i}`)));
    select.append(actions);
  };

  const showName = () => {
    nameRow.hidden = false;
    select.hidden = true;
    nameInput.value = "";
    nameInput.focus();
  };
  const hideName = () => { nameRow.hidden = true; select.hidden = false; };

  // Presets saved on another card (or the other screen) show up when the menu opens.
  for (const type of ["pointerdown", "focus"]) select.addEventListener(type, () => { if (builtVersion !== version) build(); });

  select.addEventListener("change", () => {
    const value = select.value;
    select.value = "";
    if (value === "save") return showName();
    if (value.startsWith("d")) {
      userPresets[key] = (userPresets[key] ?? []).filter((_, i) => i !== Number(value.slice(1)));
      saveUserPresets(getNativeFunction);
      return build();
    }
    const preset = value.startsWith("f") ? factory[Number(value.slice(1))] : (userPresets[key] ?? [])[Number(value.slice(1))];
    if (!preset) return;
    if (!value.startsWith("f")) return apply({ ...preset.values }, null);
    const defaults = Object.fromEntries(presetParams(preset.id).map((param) => [param, defaultOf(param)]).filter(([, v]) => v !== undefined));
    apply({ ...defaults, ...preset.values }, preset.id);
  });

  const commit = () => {
    const name = nameInput.value.trim();
    if (!name) return hideName();
    userPresets[key] = [...(userPresets[key] ?? []).filter((p) => p.name !== name), { name, values: read() }];
    saveUserPresets(getNativeFunction);
    build();
    hideName();
  };
  saveButton.addEventListener("click", commit);
  cancelButton.addEventListener("click", hideName);
  nameInput.addEventListener("keydown", (event) => {
    event.stopPropagation(); // typing a name mustn't fire the screen's shortcuts
    if (event.key === "Enter") commit();
    else if (event.key === "Escape") hideName();
  });

  build();
  loadUserPresets(getNativeFunction).then(build);
  return { element, refresh: build };
}

