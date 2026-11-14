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
  amp: [p("Sparkling clean", { ampModel: 0, ampGain: 3, ampBass: 5, ampMid: 4, ampTreble: 6.5, ampPresence: 6, ampMaster: 2 }),
        p("Edge of breakup", { ampModel: 0, ampGain: 8, ampBass: 5, ampMid: 6, ampTreble: 6, ampPresence: 5, ampMaster: 5 }),
        p("Classic crunch", { ampModel: 1, ampGain: 5, ampBass: 5, ampMid: 7, ampTreble: 6, ampPresence: 5, ampMaster: 4 }),
        p("Hard rock", { ampModel: 1, ampGain: 9, ampBass: 6, ampMid: 7, ampTreble: 6.5, ampPresence: 6, ampMaster: 5 }),
        p("Modern lead", { ampModel: 2, ampGain: 7, ampBass: 6, ampMid: 4, ampTreble: 6, ampPresence: 6, ampMaster: 4 }),
        p("Bass grit", { ampModel: 3, ampGain: 5, ampBass: 6, ampMid: 5, ampTreble: 5, ampPresence: 4, ampMaster: 3 })],
  cab: [p("Rock stack (4x12)", { cabModel: 2, cabMic: 1 }),
        p("Clean combo (1x12, centre)", { cabModel: 0, cabMic: 0 }),
        p("Blues combo (2x12)", { cabModel: 1, cabMic: 1 }),
        p("Dark and round (edge mic)", { cabModel: 2, cabMic: 2, cabHighCut: 9000 }),
        p("Bass cab, DI blended", { cabModel: 3, cabMic: 1, cabMix: 65 }),
        p("Lo-fi practice amp", { cabModel: 4, cabMic: 0, cabLowCut: 180 })],
  irverb: [p("Vocal room", { irvSpace: 1, irvMix: 18, irvPreDelay: 10, irvLowCut: 200, irvHighCut: 10000 }),
           p("Ballad hall", { irvSpace: 3, irvMix: 25, irvPreDelay: 30, irvLowCut: 180, irvHighCut: 9000 }),
           p("Bright plate", { irvSpace: 2, irvMix: 22, irvPreDelay: 15, irvLowCut: 250, irvHighCut: 14000 }),
           p("Tight booth", { irvSpace: 0, irvMix: 15, irvPreDelay: 0, irvLowCut: 150, irvHighCut: 12000 }),
           p("Big church", { irvSpace: 4, irvMix: 30, irvPreDelay: 40, irvLowCut: 200, irvHighCut: 7000 })],
  smooth: [p("Gentle (any voice)", { smoothDepth: 5, smoothThreshold: 7, smoothSharpness: 50, smoothFocus: 0, smoothLowHz: 150, smoothHighHz: 16000 }),
           p("Harsh vocal", { smoothDepth: 9, smoothThreshold: 5, smoothSharpness: 60, smoothFocus: 40, smoothLowHz: 1500, smoothHighHz: 10000 }),
           p("Boxy room tone", { smoothDepth: 8, smoothThreshold: 5, smoothSharpness: 40, smoothFocus: -50, smoothLowHz: 150, smoothHighHz: 1200, smoothRelease: 200 }),
           p("Cymbal ring", { smoothDepth: 10, smoothThreshold: 5, smoothSharpness: 75, smoothFocus: 60, smoothLowHz: 3000, smoothHighHz: 18000, smoothRelease: 250 }),
           p("Master polish", { smoothDepth: 3, smoothThreshold: 8, smoothSharpness: 50, smoothFocus: 0, smoothLowHz: 100, smoothHighHz: 16000, smoothResolution: 2 })],
  gate: [p("Quiet room", { gateThreshold: -60 }), p("Normal", { gateThreshold: -50 }), p("Noisy stage", { gateThreshold: -38 }),
         p("Tight", { gateThreshold: -30 }),
         p("Speech expander", { gateThreshold: -48, gateRange: 10, gateHold: 150, gateRelease: 250 }),
         p("Stage mic", { gateThreshold: -42, gateRange: 12, gateHold: 100, gateRelease: 200, gateFocus: 1 })],
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
         p("Radio", { compThreshold: -26, compRatio: 6 }), p("Squash", { compThreshold: -34, compRatio: 12 }),
         p("Vocal peaks (Punch)", { compStyle: 1, compThreshold: -22, compRatio: 4, compAttack: 3, compRelease: 80, compKnee: 3, compMakeup: 4 }),
         p("Parallel squash", { compStyle: 1, compThreshold: -34, compRatio: 10, compAttack: 1, compRelease: 60, compMix: 35, compAutoMakeup: 1 }),
         p("Bus glue", { compStyle: 2, compThreshold: -20, compRatio: 2, compAttack: 30, compRelease: 200, compKnee: 10, compMakeup: 1.5 })],
  opto: [p("Gentle 3 dB", { optoAmount: 30, optoOutput: 2 }), p("Level a ballad", { optoAmount: 50, optoOutput: 4 }),
         p("Firm leveller", { optoAmount: 65, optoOutput: 6, optoMode: 1 }), p("Parallel smoother", { optoAmount: 70, optoOutput: 6, optoMix: 50 })],
  deess: [p("Light", { deessThreshold: -24, deessFreq: 6500 }), p("Medium", { deessThreshold: -30, deessFreq: 6000 }),
          p("Strong", { deessThreshold: -38, deessFreq: 5500 }), p("Deep voice", { deessThreshold: -32, deessFreq: 4500 }),
          p("Smart, natural", { deessMode: 1, deessAmount: 45, deessFreq: 6000, deessRange: 8, deessBand: 1 }),
          p("Smart, firm", { deessMode: 1, deessAmount: 65, deessFreq: 5500, deessRange: 12 })],
  exciter: [p("Sparkle", { exciterFreq: 5000, exciterAmount: 30 }), p("Presence", { exciterFreq: 2500, exciterAmount: 35 }),
            p("Air", { exciterFreq: 8000, exciterAmount: 45 })],
  sat: [p("Warm tape", { satDrive: 6, satMix: 60 }), p("Tube", { satDrive: 12, satMix: 100 }),
        p("Crunch", { satDrive: 22, satMix: 100 }), p("Fuzz", { satDrive: 30, satMix: 100 }),
        p("Tape warmth", { satType: 1, satDrive: 6, satMix: 40, satTone: -10 }),
        p("Tube push", { satType: 2, satDrive: 9, satMix: 35 }),
        p("Rap edge", { satType: 3, satDrive: 12, satMix: 40, satTone: 15 })],
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
            p("Thick", { doublerMix: 70, doublerSpread: 80 }),
            p("Micro shift, subtle", { doublerMode: 1, doublerMix: 15, doublerSpread: 80, doublerDetune: 8, doublerDelay: 12 }),
            p("Wide stack", { doublerMode: 1, doublerMix: 40, doublerSpread: 100, doublerDetune: 14, doublerDelay: 15 }),
            p("Second take", { doublerMode: 2, doublerMix: 50, doublerSpread: 60, doublerDetune: 8, doublerDelay: 12 })],
  echo: [p("Slapback", { delayTime: 110, delayFeedback: 10, delayMix: 25, delayPingPong: 0 }),
         p("Quarter note (120 BPM)", { delayTime: 500, delayFeedback: 35, delayMix: 25, delayPingPong: 0 }),
         p("Ping-pong", { delayTime: 375, delayFeedback: 45, delayMix: 30, delayPingPong: 1 }),
         p("Dub", { delayTime: 600, delayFeedback: 70, delayMix: 35, delayPingPong: 0 }),
         p("Canyon", { delayTime: 900, delayFeedback: 55, delayMix: 30, delayPingPong: 1 }),
         p("Slap", { delayTime: 95, delayFeedback: 0, delayMix: 12, delayLowCut: 250, delayHighCut: 6000 }),
         p("Ducked 1/8", { delaySync: 4, delayFeedback: 20, delayMix: 15, delayDuck: 60, delayLowCut: 300, delayHighCut: 5000 }),
         p("Dotted ping-pong", { delaySync: 6, delayFeedback: 35, delayMix: 18, delayPingPong: 1, delayDuck: 65, delayLowCut: 250, delayHighCut: 4000 }),
         p("Throw", { delaySync: 7, delayFeedback: 50, delayMix: 30, delayDuck: 40, delayLowCut: 400, delayHighCut: 3500 })],
  plate: [p("Vocal plate", { plateMix: 16, plateDecay: 1.8, platePreDelay: 40, plateDamping: 40, plateLowCut: 200, plateHighCut: 9000 }),
          p("Short and bright", { plateMix: 12, plateDecay: 0.9, platePreDelay: 15, plateDamping: 20, plateLowCut: 250, plateHighCut: 12000 }),
          p("Long ballad", { plateMix: 22, plateDecay: 2.6, platePreDelay: 60, plateDamping: 55, plateLowCut: 180, plateHighCut: 8000, plateDuck: 25 }),
          p("Dark and wide", { plateMix: 20, plateDecay: 2.8, platePreDelay: 30, plateDamping: 75, plateLowCut: 150, plateHighCut: 5000, plateMod: 50 }),
          p("Ducked wash", { plateMix: 30, plateDecay: 3.2, platePreDelay: 50, plateDamping: 45, plateLowCut: 250, plateHighCut: 8000, plateDuck: 60 })],
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
// Your own presets: { effectId: [{ name, values }] }, files in the preset library
// (src/PresetLibrary.h). The ones kept in the app's settings before are copied over once.
let userPresets = {};
let loaded = null;
let version = 0; // bumped on every change, so other cards' menus catch up
const savedDefaults = {}; // DAW effect type -> whether you saved a default for new ones

function loadUserPresets(getNativeFunction) {
  const library = getNativeFunction("library");
  loaded ??= getNativeFunction("uiPref")("fxPresets")
    .then((text) => (text ? library("import", text) : 0))
    .catch(() => 0)
    .then(() => library("effects"))
    .then((presets) => { userPresets = presets ?? {}; ++version; })
    .catch(() => {});
  return loaded;
}

function saveUserPreset(getNativeFunction, key, name, values) {
  userPresets[key] = [...(userPresets[key] ?? []).filter((p) => p.name !== name), { name, values }]
    .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" }));
  ++version;
  return getNativeFunction("library")("saveEffect", key, name, values).catch(() => ({ ok: false }));
}

function deleteUserPreset(getNativeFunction, key, name) {
  userPresets[key] = (userPresets[key] ?? []).filter((p) => p.name !== name);
  ++version;
  return getNativeFunction("library")("deleteEffect", key, name).catch(() => ({ ok: false }));
}

// A preset menu for one effect card.
//   getNativeFunction: Juce.getNativeFunction
//   ids: the effect ids it covers (the DAW's pitch insert covers autotune, pitch and harmony)
//   read (): the current values to save
//   apply (values, id): set them; id is the effect a factory preset belongs to (its other
//     controls are filled in with their defaults), or null for one of yours
//   defaultOf (param): a parameter's default
//   defaultKey (optional): the DAW effect type whose new copies start from a saved default;
//     the menu then offers to save the current settings as that default, or clear it
export function createPresetPicker({ getNativeFunction, ids, read, apply, defaultOf, defaultKey }) {
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
    if (defaultKey) {
      actions.append(new Option("Use current as the default for new ones", "default"));
      if (savedDefaults[defaultKey]) actions.append(new Option("Go back to the factory default", "nodefault"));
    }
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
    if (value === "default" || value === "nodefault") {
      const values = value === "default" ? read() : null;
      savedDefaults[defaultKey] = Boolean(values);
      ++version;
      getNativeFunction("library")(values ? "setDefault" : "clearDefault", defaultKey, ...(values ? [values] : [])).catch(() => {});
      return build();
    }
    if (value.startsWith("d")) {
      const preset = (userPresets[key] ?? [])[Number(value.slice(1))];
      if (preset) deleteUserPreset(getNativeFunction, key, preset.name);
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
    saveUserPreset(getNativeFunction, key, name, read());
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
  if (defaultKey && !(defaultKey in savedDefaults))
    getNativeFunction("library")("getDefault", defaultKey)
      .then((values) => { savedDefaults[defaultKey] = Boolean(values && typeof values === "object"); ++version; build(); })
      .catch(() => {});
  return { element, refresh: build, saveAs: showName };
}

