// Every effect in signal-chain order (matches PluginProcessor::processBlock).
// `power` and every control `param` are parameter IDs from src/Parameters.cpp.

export const CATEGORIES = {
  cleanup:    { name: "Clean-up",   color: "#3fd9c6" },
  pitch:      { name: "Pitch",      color: "#9d8bff" },
  character:  { name: "Character",  color: "#ffb547" },
  modulation: { name: "Modulation", color: "#ff6fb1" },
  space:      { name: "Space",      color: "#5aafff" },
};

const knob = (param, label, options = {}) => ({ type: "knob", param, label, ...options });
const bipolar = (param, label) => knob(param, label, { bipolar: true });
const choice = (param, label) => ({ type: "choice", param, label });
const toggle = (param, label) => ({ type: "toggle", param, label });

export const MODULES = [
  { id: "hpf", name: "High-pass", category: "cleanup", power: "hpfOn",
    description: "Cuts rumble and handling noise",
    controls: [knob("hpfFreq", "Cutoff")] },

  { id: "gate", name: "Noise Gate", category: "cleanup", power: "gateOn",
    description: "Mutes background noise between words",
    controls: [knob("gateThreshold", "Threshold")] },

  { id: "tune", name: "Autotune", category: "pitch", power: "tuneOn", wide: true,
    description: "Snaps your voice to the nearest note in the key",
    controls: [{ type: "tuner" }, { type: "keys", param: "tuneKey", scaleParam: "tuneScale" },
               choice("tuneScale", "Scale"), knob("tuneSpeed", "Speed"), knob("tuneAmount", "Amount")] },

  { id: "pitch", name: "Pitch Shift", category: "pitch", power: "pitchOn",
    description: "Deeper or higher voice; formant sets the character",
    controls: [bipolar("pitchShift", "Pitch"), bipolar("pitchFormant", "Formant")] },

  { id: "harmony", name: "Harmonizer", category: "pitch", power: "harmonyOn",
    description: "Two extra voices singing along",
    controls: [bipolar("harmonyVoice1", "Voice 1"), bipolar("harmonyVoice2", "Voice 2"), knob("harmonyMix", "Mix")] },

  { id: "eq", name: "EQ", category: "cleanup", power: "eqOn", wide: true,
    description: "Warmth, presence and air",
    controls: [bipolar("eqLow", "Low"), knob("eqMidFreq", "Mid Freq"), bipolar("eqMidGain", "Mid"), bipolar("eqHigh", "High")] },

  { id: "comp", name: "Compressor", category: "cleanup", power: "compOn",
    description: "Evens out loud and quiet moments",
    controls: [knob("compThreshold", "Threshold"), knob("compRatio", "Ratio")] },

  { id: "deess", name: "De-esser", category: "cleanup", power: "deessOn",
    description: "Tames harsh S and T sounds",
    controls: [knob("deessThreshold", "Threshold"), knob("deessFreq", "Freq")] },

  { id: "exciter", name: "Exciter", category: "character", power: "exciterOn",
    description: "Adds sparkle and presence",
    controls: [knob("exciterFreq", "Freq"), knob("exciterAmount", "Amount")] },

  { id: "sat", name: "Saturation", category: "character", power: "satOn",
    description: "Warm analog-style drive",
    controls: [knob("satDrive", "Drive"), knob("satMix", "Mix")] },

  { id: "radio", name: "Radio", category: "character", power: "radioOn",
    description: "Telephone and walkie-talkie band",
    controls: [knob("radioLow", "Low Cut"), knob("radioHigh", "High Cut")] },

  { id: "crush", name: "Bitcrusher", category: "character", power: "crushOn",
    description: "Lo-fi digital grit",
    controls: [knob("crushBits", "Bits"), knob("crushRate", "Rate"), knob("crushMix", "Mix")] },

  { id: "robot", name: "Robot", category: "character", power: "robotOn",
    description: "Ring-modulated metallic voice",
    controls: [knob("robotFreq", "Freq"), knob("robotMix", "Mix")] },

  { id: "filter", name: "Filter", category: "character", power: "filterOn",
    description: "Resonant ladder filter",
    controls: [choice("filterType", "Type"), knob("filterCutoff", "Cutoff"), knob("filterResonance", "Resonance")] },

  { id: "wah", name: "Auto-wah", category: "character", power: "wahOn",
    description: "A filter that follows your voice",
    controls: [knob("wahSensitivity", "Sensitivity"), knob("wahFreq", "Freq"), knob("wahMix", "Mix")] },

  { id: "tremolo", name: "Tremolo", category: "modulation", power: "tremoloOn",
    description: "Pulsing volume",
    controls: [knob("tremoloRate", "Rate"), knob("tremoloDepth", "Depth")] },

  { id: "vibrato", name: "Vibrato", category: "modulation", power: "vibratoOn",
    description: "Wobbling pitch",
    controls: [knob("vibratoRate", "Rate"), knob("vibratoDepth", "Depth")] },

  { id: "chorus", name: "Chorus", category: "modulation", power: "chorusOn",
    description: "Lush, shimmering thickness",
    controls: [knob("chorusRate", "Rate"), knob("chorusDepth", "Depth"), knob("chorusMix", "Mix")] },

  { id: "flanger", name: "Flanger", category: "modulation", power: "flangerOn", wide: true,
    description: "Jet-plane sweep",
    controls: [knob("flangerRate", "Rate"), knob("flangerDepth", "Depth"), knob("flangerFeedback", "Feedback"), knob("flangerMix", "Mix")] },

  { id: "phaser", name: "Phaser", category: "modulation", power: "phaserOn", wide: true,
    description: "Swirling notches",
    controls: [knob("phaserRate", "Rate"), knob("phaserDepth", "Depth"), knob("phaserFeedback", "Feedback"), knob("phaserMix", "Mix")] },

  { id: "doubler", name: "Doubler", category: "space", power: "doublerOn",
    description: "Double-tracked vocal, spread wide",
    controls: [knob("doublerMix", "Mix"), knob("doublerSpread", "Spread")] },

  { id: "echo", name: "Echo", category: "space", power: "delayOn", wide: true,
    description: "Repeats that darken as they fade",
    controls: [knob("delayTime", "Time"), knob("delayFeedback", "Feedback"), knob("delayMix", "Mix"), toggle("delayPingPong", "Ping-pong")] },

  { id: "reverb", name: "Reverb", category: "space", power: "reverbOn", wide: true,
    description: "Puts your voice in a room",
    controls: [{ type: "reverb-graphic" }, knob("reverbMix", "Mix"), knob("reverbSize", "Size"),
               knob("reverbDamping", "Damping"), knob("reverbWidth", "Width")] },

  { id: "width", name: "Stereo Width", category: "space", power: "widthOn",
    description: "Spreads stereo effects wider",
    controls: [knob("widthAmount", "Width")] },
];

// Module key (C++ fx::getModuleKey) for each entry above, used by the DAW inserts and the
// effect pictures (fxviews.js). The pitch module holds three entries (autotune, pitch shift,
// harmonizer).
export const MODULE_KEYS = {
  hpf: "highPass", gate: "gate", tune: "pitch", pitch: "pitch", harmony: "pitch", eq: "eq",
  comp: "compressor", deess: "deEsser", exciter: "exciter", sat: "saturation", radio: "radio",
  crush: "bitcrusher", robot: "robot", filter: "filter", wah: "autoWah", tremolo: "tremolo",
  vibrato: "vibrato", chorus: "chorus", flanger: "flanger", phaser: "phaser", doubler: "doubler",
  echo: "echo", reverb: "reverb", width: "width",
};

export const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

// Semitones above the key, same order as PitchEngine::getScaleNames().
export const SCALES = [
  [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
  [0, 2, 4, 5, 7, 9, 11],
  [0, 2, 3, 5, 7, 8, 10],
  [0, 2, 3, 5, 7, 8, 11],
  [0, 2, 4, 7, 9],
  [0, 3, 5, 7, 10],
  [0, 3, 5, 6, 7, 10],
];
