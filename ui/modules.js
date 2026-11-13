// Every effect in signal-chain order (matches PluginProcessor::processBlock).
// `power` and every control `param` are parameter IDs from src/Parameters.cpp.

export const CATEGORIES = {
  cleanup:    { name: "Clean-up",   color: "#3fd9c6" },
  pitch:      { name: "Pitch",      color: "#9d8bff" },
  character:  { name: "Character",  color: "#ffb547" },
  modulation: { name: "Modulation", color: "#ff6fb1" },
  space:      { name: "Space",      color: "#5aafff" },
  master:     { name: "Mastering",  color: "#7ee0a1" },
};

const knob = (param, label, options = {}) => ({ type: "knob", param, label, ...options });
const bipolar = (param, label) => knob(param, label, { bipolar: true });
const choice = (param, label) => ({ type: "choice", param, label });
const toggle = (param, label) => ({ type: "toggle", param, label });

export const MODULES = [
  { id: "hpf", name: "High-pass", category: "cleanup", power: "hpfOn",
    description: "Cuts rumble and handling noise",
    controls: [knob("hpfFreq", "Cutoff")] },

  { id: "denoise", name: "Denoise", category: "cleanup", power: "denoiseOn", wide: true,
    description: "Takes hiss, hum and room noise out band by band, even under your voice. Adds 11 ms",
    controls: [knob("denoiseReduction", "Reduction"), bipolar("denoiseThreshold", "Threshold"),
               toggle("denoiseAdaptive", "Adaptive"), toggle("denoiseLearn", "Learn noise")] },

  { id: "gate", name: "Noise Gate", category: "cleanup", power: "gateOn",
    description: "Mutes background noise between words. Lower the range for a gentle expander; Focus ignores drum and cymbal bleed",
    controls: [knob("gateThreshold", "Threshold"), knob("gateRange", "Range"), knob("gateHold", "Hold"),
               knob("gateRelease", "Release"), toggle("gateFocus", "Focus")] },

  { id: "tune", name: "Autotune", category: "pitch", power: "tuneOn", wide: true,
    description: "Snaps your voice to the nearest note in the key",
    controls: [{ type: "tuner" }, { type: "keys", param: "tuneKey", scaleParam: "tuneScale" },
               choice("tuneScale", "Scale"), knob("tuneSpeed", "Speed"), knob("tuneAmount", "Amount")] },

  { id: "pitch", name: "Pitch Shift", category: "pitch", power: "pitchOn",
    description: "Deeper or higher voice; formant sets the character",
    controls: [bipolar("pitchShift", "Pitch"), bipolar("pitchFormant", "Formant")] },

  // Mode: fixed intervals, steps of the autotune key's scale, or notes of the song's
  // chords. `when` shows a control only for some values of others (see isControlShown).
  { id: "harmony", name: "Harmonizer", category: "pitch", power: "harmonyOn", wide: true,
    description: "Up to four extra voices: at fixed intervals, in the key, or following the song's chords",
    controls: [{ type: "chord" },
               choice("harmonyMode", "Mode"), choice("harmonyVoices", "Voices"),
               { ...choice("harmonyStyle", "Style"), when: { harmonyMode: [2] } },
               { ...choice("harmonySource", "Chords from"), when: { harmonyMode: [2] }, liveOnly: true },
               { ...bipolar("harmonyVoice1", "Voice 1"), when: { harmonyMode: [0, 1] } },
               { ...bipolar("harmonyVoice2", "Voice 2"), when: { harmonyMode: [0, 1], harmonyVoices: [1, 2, 3] } },
               { ...bipolar("harmonyVoice3", "Voice 3"), when: { harmonyMode: [0, 1], harmonyVoices: [2, 3] } },
               { ...bipolar("harmonyVoice4", "Voice 4"), when: { harmonyMode: [0, 1], harmonyVoices: [3] } },
               knob("harmonyMix", "Mix"), knob("harmonyHumanize", "Humanize"), knob("harmonySpread", "Spread"),
               bipolar("harmonyFormant", "Formant")] },

  { id: "eq", name: "EQ", category: "cleanup", power: "eqOn", wide: true,
    description: "Warmth, presence and air",
    controls: [bipolar("eqLow", "Low"), knob("eqMidFreq", "Mid Freq"), bipolar("eqMidGain", "Mid"), bipolar("eqHigh", "High")] },

  { id: "peq", name: "Parametric EQ", category: "cleanup", power: "peqOn", wide: true,
    description: "Seven precise bands for mixing. Drag the points; scroll on a bell to change its width",
    controls: [toggle("peqLowCutOn", "Low cut"), knob("peqLowCutFreq", "Low cut"),
               knob("peqLowFreq", "Low freq"), bipolar("peqLowGain", "Low"),
               knob("peqBell1Freq", "Bell 1"), bipolar("peqBell1Gain", "Gain 1"), knob("peqBell1Q", "Q 1"),
               knob("peqBell2Freq", "Bell 2"), bipolar("peqBell2Gain", "Gain 2"), knob("peqBell2Q", "Q 2"),
               knob("peqBell3Freq", "Bell 3"), bipolar("peqBell3Gain", "Gain 3"), knob("peqBell3Q", "Q 3"),
               knob("peqHighFreq", "High freq"), bipolar("peqHighGain", "High"),
               toggle("peqHighCutOn", "High cut"), knob("peqHighCutFreq", "High cut")] },

  { id: "smooth", name: "Smooth", category: "cleanup", power: "smoothOn", wide: true,
    description: "Finds the narrow frequencies that stick out (harsh notes, boxy tones, whistly s, ringing cymbals) and turns them down only while they do. Adds 5-21 ms",
    controls: [knob("smoothDepth", "Depth"), knob("smoothThreshold", "Threshold"), knob("smoothSharpness", "Sharpness"),
               bipolar("smoothFocus", "Focus"), knob("smoothLowHz", "From"), knob("smoothHighHz", "To"),
               knob("smoothAttack", "Attack"), knob("smoothRelease", "Release"), knob("smoothMix", "Mix"),
               choice("smoothStereo", "Stereo"), choice("smoothResolution", "Resolution"), toggle("smoothDelta", "Delta")] },

  { id: "comp", name: "Compressor", category: "cleanup", power: "compOn",
    description: "Evens out loud and quiet moments. Clean, Punch (fast, grabs peaks) or Glue (slow, even); Mix below 100 is parallel compression",
    controls: [choice("compStyle", "Style"), knob("compThreshold", "Threshold"), knob("compRatio", "Ratio"),
               knob("compAttack", "Attack"), knob("compRelease", "Release"), knob("compKnee", "Knee"),
               knob("compMakeup", "Makeup"), toggle("compAutoMakeup", "Auto makeup"), knob("compMix", "Mix")] },

  { id: "opto", name: "Opto Compressor", category: "cleanup", power: "optoOn",
    description: "A slow, smooth leveller for after the Compressor: evens out whole lines without grabbing the peaks",
    controls: [knob("optoAmount", "Amount"), knob("optoOutput", "Output"), choice("optoMode", "Mode"), knob("optoMix", "Mix")] },

  { id: "autolevel", name: "Auto Level", category: "cleanup", power: "alOn", wide: true,
    description: "Rides your level like an engineer on the fader: a steady loudness, or a few LU over the backing track (Live)",
    controls: [choice("alMode", "Aim"), { ...knob("alTarget", "Target"), when: { alMode: [0] } },
               { ...bipolar("alOverBacking", "Over backing"), when: { alMode: [1] } },
               knob("alRange", "Range"), knob("alSpeed", "Speed")] },

  { id: "deess", name: "De-esser", category: "cleanup", power: "deessOn",
    description: "Tames harsh S and T sounds. Smart works the same on a whisper and a belt; Listen plays only what it takes out",
    controls: [choice("deessMode", "Mode"), { ...knob("deessThreshold", "Threshold"), when: { deessMode: [0] } },
               { ...knob("deessAmount", "Amount"), when: { deessMode: [1] } }, knob("deessFreq", "Freq"),
               knob("deessRange", "Range"), choice("deessBand", "Band"), toggle("deessListen", "Listen")] },

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

  { id: "amp", name: "Amp", category: "character", power: "ampOn", wide: true,
    description: "A guitar or bass amp: gain, the classic bass / mid / treble tone stack, presence and a power stage. Put a Cabinet after it",
    controls: [choice("ampModel", "Model"), knob("ampGain", "Gain"), knob("ampBass", "Bass"), knob("ampMid", "Mid"),
               knob("ampTreble", "Treble"), knob("ampPresence", "Presence"), knob("ampMaster", "Master"), bipolar("ampOutput", "Output")] },

  { id: "cab", name: "Cabinet", category: "character", power: "cabOn", wide: true,
    description: "A guitar or bass speaker cabinet and its mic, built in or from your own impulse response file",
    controls: [{ type: "file", module: "cab", label: "IR file" }, choice("cabModel", "Cabinet"), choice("cabMic", "Mic"),
               knob("cabLowCut", "Low cut"), knob("cabHighCut", "High cut"), knob("cabMix", "Mix")] },

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
    description: "Repeats that darken as they fade. Sync follows the song's tempo; Duck keeps the repeats out of the way while you sing",
    controls: [choice("delaySync", "Sync"), { ...knob("delayTime", "Time"), when: { delaySync: [0] } },
               knob("delayFeedback", "Feedback"), knob("delayMix", "Mix"), toggle("delayPingPong", "Ping-pong"),
               knob("delayDuck", "Duck"), knob("delayLowCut", "Low cut"), knob("delayHighCut", "High cut")] },

  { id: "reverb", name: "Reverb", category: "space", power: "reverbOn", wide: true,
    description: "Puts your voice in a room",
    controls: [{ type: "reverb-graphic" }, knob("reverbMix", "Mix"), knob("reverbSize", "Size"),
               knob("reverbDamping", "Damping"), knob("reverbWidth", "Width")] },

  { id: "plate", name: "Plate Reverb", category: "space", power: "plateOn", wide: true,
    description: "The classic vocal reverb: smooth, bright and dense. Pre-delay keeps the words clear; Duck lets it bloom in the gaps",
    controls: [knob("plateMix", "Mix"), knob("plateDecay", "Decay"), knob("platePreDelay", "Pre-delay"),
               knob("plateDamping", "Damping"), knob("plateLowCut", "Low cut"), knob("plateHighCut", "High cut"),
               knob("plateMod", "Movement"), knob("plateWidth", "Width"), knob("plateDuck", "Duck")] },

  { id: "irverb", name: "IR Reverb", category: "space", power: "irvOn", wide: true,
    description: "A real-sounding room from an impulse response: a built-in space or your own file",
    controls: [{ type: "file", module: "irReverb", label: "IR file" }, choice("irvSpace", "Space"), knob("irvMix", "Mix"),
               knob("irvPreDelay", "Pre-delay"), knob("irvLowCut", "Low cut"), knob("irvHighCut", "High cut")] },

  { id: "width", name: "Stereo Width", category: "space", power: "widthOn",
    description: "Spreads stereo effects wider",
    controls: [knob("widthAmount", "Width")] },

  // Mastering: for the master bus (or a return or track) in DAW mode. `dawOnly` keeps them
  // out of the Live screen, which is about the voice.
  { id: "meq", name: "Mastering EQ", category: "master", power: "meqOn", wide: true, dawOnly: true,
    description: "Broad tone shaping for the whole mix: low cut, shelves, one wide bell and air",
    controls: [knob("meqLowCut", "Low cut"), knob("meqLowFreq", "Low freq"), bipolar("meqLowGain", "Low"),
               knob("meqMidFreq", "Mid freq"), bipolar("meqMidGain", "Mid"),
               knob("meqHighFreq", "High freq"), bipolar("meqHighGain", "High"), bipolar("meqAirGain", "Air")] },

  { id: "mb", name: "Multiband Compressor", category: "master", power: "mbOn", wide: true, dawOnly: true,
    description: "Three bands compressed on their own, so a loud bass doesn't pump the vocals",
    controls: [knob("mbLowFreq", "Low / mid"), knob("mbHighFreq", "Mid / high"),
               knob("mbLowThreshold", "Low"), knob("mbMidThreshold", "Mid"), knob("mbHighThreshold", "High"),
               knob("mbRatio", "Ratio"), knob("mbAttack", "Attack"), knob("mbRelease", "Release"), bipolar("mbMakeup", "Makeup")] },

  { id: "imager", name: "Stereo Imager", category: "master", power: "imgOn", dawOnly: true,
    description: "Wider or narrower mix, with the bass kept mono",
    controls: [knob("imgWidth", "Width"), knob("imgMonoBelow", "Mono below")] },

  { id: "limiter", name: "Limiter", category: "master", power: "limOn", dawOnly: true,
    description: "Look-ahead brickwall: louder without ever passing the ceiling. Last in the chain",
    controls: [knob("limDrive", "Drive"), knob("limCeiling", "Ceiling"), knob("limRelease", "Release")] },

  { id: "loudness", name: "Loudness Meter", category: "master", power: "loudOn", dawOnly: true,
    description: "LUFS like streaming services measure it. Switch off and on to restart",
    controls: [{ type: "loudness" }, knob("loudTarget", "Target")] },

  // Dynamics on part of the sound (DAW mode).
  { id: "transient", name: "Transient Shaper", category: "cleanup", power: "tsOn", dawOnly: true,
    description: "More or less snap at the start of each hit, and more or less ring after it",
    controls: [bipolar("tsAttack", "Attack"), bipolar("tsSustain", "Sustain")] },

  { id: "deq", name: "Dynamic EQ", category: "cleanup", power: "deqOn", dawOnly: true,
    description: "A bell that cuts only while its band gets loud: harsh notes, a booming bass note",
    controls: [knob("deqFreq", "Freq"), knob("deqQ", "Q"), knob("deqThreshold", "Threshold"), knob("deqRange", "Range")] },
];

// Module key (C++ fx::getModuleKey) for each entry above, used by the DAW inserts and the
// effect pictures (fxviews.js). The pitch module holds three entries (autotune, pitch shift,
// harmonizer).
export const MODULE_KEYS = {
  hpf: "highPass", denoise: "denoise", gate: "gate", tune: "pitch", pitch: "pitch", harmony: "pitch", eq: "eq", peq: "paraEq", smooth: "smooth",
  comp: "compressor", opto: "opto", autolevel: "autoLevel", deess: "deEsser", exciter: "exciter", sat: "saturation", radio: "radio",
  crush: "bitcrusher", robot: "robot", filter: "filter", wah: "autoWah", amp: "amp", cab: "cab", tremolo: "tremolo",
  vibrato: "vibrato", chorus: "chorus", flanger: "flanger", phaser: "phaser", doubler: "doubler",
  echo: "echo", reverb: "reverb", plate: "plate", irverb: "irReverb", width: "width",
  meq: "masterEq", mb: "multiband", imager: "imager", limiter: "limiter", loudness: "loudness",
  transient: "transient", deq: "dynamicEq",
};

// Whether a control with a `when` ({ param: [values] }) shows, given a way to read values.
export function isControlShown(control, get) {
  return Object.entries(control.when ?? {}).every(([param, values]) => values.includes(Math.round(get(param) ?? 0)));
}

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
