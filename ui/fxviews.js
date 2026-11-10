// Pictures of what an effect does, drawn from its current values: EQ and filter
// response curves, compressor and gate transfer curves, echo taps, reverb decay,
// LFO shapes, the saturation and bitcrusher curves. Some have handles you can drag
// (EQ bands, high-pass, compressor and gate thresholds).
//
//   createFxView (moduleKey, { get (param), set (param, value, first), range (param) })
//     -> { element, update() } or null when the effect has no picture.
//
// moduleKey is the C++ fx::getModuleKey name ("eq", "compressor"...). The curves
// follow the DSP in src/dsp (same filter types, frequencies and Qs).
import { clamp, svg } from "./widgets.js";

const W = 240, H = 92;               // viewBox
const FS = 48000;                    // sample rate the curves are drawn at
const F_MIN = 20, F_MAX = 20000;
const xOfF = (f) => (Math.log(clamp(f || F_MIN, F_MIN, F_MAX) / F_MIN) / Math.log(F_MAX / F_MIN)) * W;
const fOfX = (x) => F_MIN * Math.pow(F_MAX / F_MIN, clamp(x / W));

//============================================================================== filters
// RBJ cookbook biquads, as in src/dsp/Biquad.h.
function biquad(type, f0, q, gainDb = 0) {
  const A = Math.pow(10, gainDb / 40);
  const w0 = (2 * Math.PI * Math.min(f0, FS * 0.49)) / FS;
  const cos = Math.cos(w0), alpha = Math.sin(w0) / (2 * q);
  const sq = 2 * Math.sqrt(A) * alpha;
  let b, a;

  switch (type) {
    case "lowPass":   b = [(1 - cos) / 2, 1 - cos, (1 - cos) / 2]; a = [1 + alpha, -2 * cos, 1 - alpha]; break;
    case "highPass":  b = [(1 + cos) / 2, -(1 + cos), (1 + cos) / 2]; a = [1 + alpha, -2 * cos, 1 - alpha]; break;
    case "bandPass":  b = [alpha, 0, -alpha]; a = [1 + alpha, -2 * cos, 1 - alpha]; break;
    case "peak":      b = [1 + alpha * A, -2 * cos, 1 - alpha * A]; a = [1 + alpha / A, -2 * cos, 1 - alpha / A]; break;
    case "lowShelf":
      b = [A * ((A + 1) - (A - 1) * cos + sq), 2 * A * ((A - 1) - (A + 1) * cos), A * ((A + 1) - (A - 1) * cos - sq)];
      a = [(A + 1) + (A - 1) * cos + sq, -2 * ((A - 1) + (A + 1) * cos), (A + 1) + (A - 1) * cos - sq];
      break;
    case "highShelf":
      b = [A * ((A + 1) + (A - 1) * cos + sq), -2 * A * ((A - 1) + (A + 1) * cos), A * ((A + 1) + (A - 1) * cos - sq)];
      a = [(A + 1) - (A - 1) * cos + sq, 2 * ((A - 1) - (A + 1) * cos), (A + 1) - (A - 1) * cos - sq];
      break;
  }
  return { b, a };
}

function magnitudeDb({ b, a }, f) {
  const w = (2 * Math.PI * f) / FS;
  const c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
  const nr = b[0] + b[1] * c1 + b[2] * c2, ni = -(b[1] * s1 + b[2] * s2);
  const dr = a[0] + a[1] * c1 + a[2] * c2, di = -(a[1] * s1 + a[2] * s2);
  return 10 * Math.log10((nr * nr + ni * ni) / Math.max(1e-30, dr * dr + di * di));
}

// Path of the summed response of a filter cascade, dB range ±range.
function responsePath(filters, range, offset = 0) {
  const points = [];
  for (let x = 0; x <= W; x += 2) {
    const f = fOfX(x);
    const db = filters.reduce((sum, filter) => sum + magnitudeDb(filter, f), 0) + offset;
    points.push(`${x.toFixed(1)},${(H / 2 - (clamp(db, -range * 1.2, range * 1.2) / range) * (H / 2 - 6)).toFixed(1)}`);
  }
  return `M${points.join(" L")}`;
}

//============================================================================== drawing helpers
// The picture stretches to its card, so anything that mustn't distort (labels,
// handles) is drawn in HTML or with non-scaling strokes.
function frame(className) {
  const element = svg("svg", { class: `fx-graph ${className}`, viewBox: `0 0 ${W} ${H}`, preserveAspectRatio: "none" });
  element.labels = [];
  return element;
}

const label = (element, x, y, text) => element.labels.push({ x, y, text });

function frequencyGrid(element, range) {
  for (const f of [100, 1000, 10000]) element.append(svg("line", { class: "grid", x1: xOfF(f), x2: xOfF(f), y1: 0, y2: H }));
  for (const [f, text] of [[100, "100"], [1000, "1k"], [10000, "10k"]]) label(element, xOfF(f) + 3, H - 3, text);
  element.append(svg("line", { class: "grid zero", x1: 0, x2: W, y1: H / 2, y2: H / 2 }));
  if (range) label(element, 3, 11, `+${range} dB`);
}

const yOfDb = (db, range) => H / 2 - (db / range) * (H / 2 - 6);
const dbOfY = (y, range) => ((H / 2 - y) / (H / 2 - 6)) * range;

// A draggable point. onDrag (x, y, first) gets viewBox coordinates; onWheel (deltaY),
// when given, makes it respond to the scroll wheel too.
function handle(element, onDrag, title, onWheel) {
  // Zero-length round-capped strokes: round dots whatever the stretch.
  const point = svg("path", { class: "fx-handle" });
  const hit = svg("path", { class: "fx-hit" });
  const group = svg("g");
  group.append(Object.assign(svg("title"), { textContent: title }), hit, point);
  group.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    event.stopPropagation();
    group.setPointerCapture(event.pointerId);
    let first = true;
    const move = (e) => {
      const box = element.getBoundingClientRect();
      onDrag(((e.clientX - box.left) / box.width) * W, ((e.clientY - box.top) / box.height) * H, first);
      first = false;
    };
    const up = () => {
      group.removeEventListener("pointermove", move);
      group.removeEventListener("pointerup", up);
      group.removeEventListener("pointercancel", up);
    };
    group.addEventListener("pointermove", move);
    group.addEventListener("pointerup", up);
    group.addEventListener("pointercancel", up);
  });
  if (onWheel) {
    group.addEventListener("wheel", (event) => {
      event.preventDefault();
      event.stopPropagation();
      onWheel(event.deltaY);
    }, { passive: false });
  }
  element.append(group);
  return {
    move(x, y) {
      for (const c of [point, hit]) c.setAttribute("d", `M${x.toFixed(1)} ${y.toFixed(1)} l0.01 0`);
    },
  };
}

const path = (element, className) => {
  const p = svg("path", { class: className });
  element.append(p);
  return p;
};

//============================================================================== views
const views = {
  eq({ get, set, range }) {
    const RANGE = 15;
    const element = frame("is-eq");
    frequencyGrid(element, RANGE);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const gainRange = (p) => range(p) ?? { min: -12, max: 12 };
    const setGain = (p, y, first) => set(p, Math.round(clamp(dbOfY(y, RANGE), gainRange(p).min, gainRange(p).max) * 10) / 10, first);
    const freqRange = () => range("eqMidFreq") ?? { min: 200, max: 8000 };

    const low = handle(element, (x, y, first) => setGain("eqLow", y, first), "Low shelf (150 Hz): drag up or down");
    const mid = handle(element, (x, y, first) => {
      set("eqMidFreq", Math.round(clamp(fOfX(x), freqRange().min, freqRange().max)), first);
      setGain("eqMidGain", y, false);
    }, "Mid: drag to set frequency and gain");
    const high = handle(element, (x, y, first) => setGain("eqHigh", y, first), "High shelf (7 kHz): drag up or down");

    return {
      element,
      update() {
        const filters = [biquad("lowShelf", 150, 0.707, get("eqLow")), biquad("peak", get("eqMidFreq"), 1, get("eqMidGain")),
                         biquad("highShelf", 7000, 0.707, get("eqHigh"))];
        const d = responsePath(filters, RANGE);
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H / 2} L0,${H / 2} Z`);
        low.move(xOfF(150) - 8, yOfDb(get("eqLow"), RANGE));
        mid.move(xOfF(get("eqMidFreq")), yOfDb(get("eqMidGain"), RANGE));
        high.move(xOfF(7000) + 8, yOfDb(get("eqHigh"), RANGE));
      },
    };
  },

  // The parametric EQ (src/dsp/ParametricEq.h): 24 dB/oct cuts (two Butterworth
  // stages), 0.707-Q shelves and three bells. Drag a point for frequency and gain;
  // scroll on a bell to change its Q.
  paraEq({ get, set, range }) {
    const RANGE = 18;
    const element = frame("is-eq");
    frequencyGrid(element, RANGE);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const r = (p, fallback) => range(p) ?? fallback;
    const setFreq = (p, x, first) => set(p, Math.round(clamp(fOfX(x), r(p, { min: 20 }).min, r(p, { max: 20000 }).max)), first);
    const setGain = (p, y, first) => set(p, Math.round(clamp(dbOfY(y, RANGE), r(p, { min: -18 }).min, r(p, { max: 18 }).max) * 10) / 10, first);
    const BELLS = [1, 2, 3].map((n) => ({ freq: `peqBell${n}Freq`, gain: `peqBell${n}Gain`, q: `peqBell${n}Q` }));

    const lowCut = handle(element, (x, y, first) => setFreq("peqLowCutFreq", x, first), "Low cut: drag sideways (switch it on below)");
    const low = handle(element, (x, y, first) => { setFreq("peqLowFreq", x, first); setGain("peqLowGain", y, false); },
                       "Low shelf: drag to set frequency and gain");
    const bells = BELLS.map((bell, i) => handle(element, (x, y, first) => { setFreq(bell.freq, x, first); setGain(bell.gain, y, false); },
      `Bell ${i + 1}: drag to set frequency and gain, scroll to change Q`,
      (deltaY) => {
        const qRange = r(bell.q, { min: 0.1, max: 18 });
        set(bell.q, Math.round(clamp(get(bell.q) * Math.pow(1.1, -Math.sign(deltaY)), qRange.min, qRange.max) * 100) / 100, true);
      }));
    const high = handle(element, (x, y, first) => { setFreq("peqHighFreq", x, first); setGain("peqHighGain", y, false); },
                        "High shelf: drag to set frequency and gain");
    const highCut = handle(element, (x, y, first) => setFreq("peqHighCutFreq", x, first), "High cut: drag sideways (switch it on below)");

    return {
      element,
      update() {
        const filters = [biquad("lowShelf", get("peqLowFreq"), 0.707, get("peqLowGain")),
                         ...BELLS.map((bell) => biquad("peak", get(bell.freq), Math.max(0.1, get(bell.q)), get(bell.gain))),
                         biquad("highShelf", get("peqHighFreq"), 0.707, get("peqHighGain"))];
        if (get("peqLowCutOn") >= 0.5) filters.push(biquad("highPass", get("peqLowCutFreq"), 0.5412), biquad("highPass", get("peqLowCutFreq"), 1.3066));
        if (get("peqHighCutOn") >= 0.5) filters.push(biquad("lowPass", get("peqHighCutFreq"), 0.5412), biquad("lowPass", get("peqHighCutFreq"), 1.3066));
        const d = responsePath(filters, RANGE);
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H / 2} L0,${H / 2} Z`);
        // Cuts sit on the curve at their corner (-3 dB, or below the top when off).
        lowCut.move(xOfF(get("peqLowCutFreq")), yOfDb(get("peqLowCutOn") >= 0.5 ? -3 : -RANGE + 2, RANGE));
        low.move(xOfF(get("peqLowFreq")), yOfDb(get("peqLowGain"), RANGE));
        BELLS.forEach((bell, i) => bells[i].move(xOfF(get(bell.freq)), yOfDb(get(bell.gain), RANGE)));
        high.move(xOfF(get("peqHighFreq")), yOfDb(get("peqHighGain"), RANGE));
        highCut.move(xOfF(get("peqHighCutFreq")), yOfDb(get("peqHighCutOn") >= 0.5 ? -3 : -RANGE + 2, RANGE));
      },
    };
  },

  highPass({ get, set, range }) {
    const RANGE = 24;
    const element = frame("is-filter");
    frequencyGrid(element);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const r = () => range("hpfFreq") ?? { min: 20, max: 400 };
    const point = handle(element, (x, y, first) => set("hpfFreq", Math.round(clamp(fOfX(x), r().min, r().max)), first), "Cutoff: drag sideways");
    return {
      element,
      update() {
        const d = responsePath([biquad("highPass", get("hpfFreq"), 0.707)], RANGE, 0);
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H} L0,${H} Z`);
        point.move(xOfF(get("hpfFreq")), yOfDb(-3, RANGE));
      },
    };
  },

  radio({ get }) {
    const RANGE = 24;
    const element = frame("is-filter");
    frequencyGrid(element);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    return {
      element,
      update() {
        const low = get("radioLow"), high = Math.max(get("radioHigh"), low * 1.5);
        const d = responsePath([biquad("highPass", low, 0.54), biquad("highPass", low, 1.31), biquad("lowPass", high, 0.54),
                                biquad("lowPass", high, 1.31), biquad("peak", Math.sqrt(low * high), 1.2, 5)], RANGE, -6);
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H} L0,${H} Z`);
      },
    };
  },

  // A 24 dB/oct ladder, drawn as two biquads; resonance raises the peak.
  filter({ get }) {
    const RANGE = 24;
    const element = frame("is-filter");
    frequencyGrid(element);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    return {
      element,
      update() {
        const type = ["lowPass", "highPass", "bandPass"][Math.round(get("filterType"))] ?? "lowPass";
        const f = get("filterCutoff"), q = 0.6 + Math.pow(clamp(get("filterResonance") / 100, 0, 0.95), 2) * 9;
        const d = responsePath([biquad(type, f, 0.54), biquad(type, f, q)], RANGE, -6);
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H} L0,${H} Z`);
      },
    };
  },

  deEsser({ get }) {
    const element = frame("is-filter");
    frequencyGrid(element);
    const band = svg("rect", { class: "fx-band", y: 0, height: H });
    const line = svg("line", { class: "fx-threshold", x1: 0, x2: W });
    element.append(band, line);
    return {
      element,
      update() {
        const f = get("deessFreq");
        const x0 = xOfF(f), x1 = xOfF(Math.min(F_MAX, f * 2.5));
        band.setAttribute("x", x0);
        band.setAttribute("width", Math.max(2, x1 - x0));
        const y = clamp((-get("deessThreshold") / 60) * H, 4, H - 4);
        line.setAttribute("y1", y);
        line.setAttribute("y2", y);
      },
    };
  },

  // Input level (x) against output level (y), -60..0 dB.
  compressor({ get, set, range }) {
    return dynamics({ get, set, range, threshold: "compThreshold", ratio: () => get("compRatio"), above: true });
  },

  gate({ get, set, range }) {
    return dynamics({ get, set, range, threshold: "gateThreshold", ratio: () => 10, above: false });
  },

  saturation({ get }) {
    const element = frame("is-curve");
    element.append(svg("line", { class: "grid", x1: 0, x2: W, y1: H / 2, y2: H / 2 }),
                   svg("line", { class: "grid", x1: W / 2, x2: W / 2, y1: 0, y2: H }),
                   svg("line", { class: "grid dashed", x1: W / 2 - H / 2, y1: H, x2: W / 2 + H / 2, y2: 0 }));
    const curve = path(element, "fx-curve");
    return {
      element,
      update() {
        const drive = Math.pow(10, get("satDrive") / 20), makeup = 1 / Math.pow(drive, 0.7), mix = get("satMix") / 100;
        const points = [];
        for (let i = 0; i <= 60; ++i) {
          const x = -1 + (2 * i) / 60;
          const y = x + mix * (Math.tanh(drive * x) * makeup - x);
          points.push(`${(W / 2 + x * (H / 2 - 4)).toFixed(1)},${(H / 2 - clamp(y, -1.1, 1.1) * (H / 2 - 4)).toFixed(1)}`);
        }
        curve.setAttribute("d", `M${points.join(" L")}`);
      },
    };
  },

  bitcrusher({ get }) {
    const element = frame("is-curve");
    element.append(svg("line", { class: "grid", x1: 0, x2: W, y1: H / 2, y2: H / 2 }));
    const ghost = path(element, "fx-ghost"), curve = path(element, "fx-curve");
    return {
      element,
      update() {
        const levels = Math.pow(2, Math.max(1, get("crushBits")) - 1);
        const hold = Math.max(1, Math.round(48000 / Math.max(100, get("crushRate")) / 6)); // drawn 6x slower so steps show
        const mix = get("crushMix") / 100;
        const clean = [], crushed = [];
        let held = 0;
        for (let i = 0; i <= W; ++i) {
          const x = Math.sin((i / W) * Math.PI * 4) * 0.9;
          if (i % hold === 0) held = Math.round(x * levels) / levels;
          const y = x + mix * (held - x);
          clean.push(`${i},${(H / 2 - x * (H / 2 - 4)).toFixed(1)}`);
          crushed.push(`${i},${(H / 2 - y * (H / 2 - 4)).toFixed(1)}`);
        }
        ghost.setAttribute("d", `M${clean.join(" L")}`);
        curve.setAttribute("d", `M${crushed.join(" L")}`);
      },
    };
  },

  echo({ get }) {
    const element = frame("is-taps");
    const taps = svg("g");
    element.append(svg("line", { class: "grid", x1: 0, x2: W, y1: H / 2, y2: H / 2 }), taps);
    return {
      element,
      update() {
        const seconds = 2, time = Math.max(0.01, get("delayTime") / 1000), feedback = get("delayFeedback") / 100;
        const mix = get("delayMix") / 100, pingPong = get("delayPingPong") >= 0.5;
        taps.replaceChildren(svg("rect", { class: "fx-tap is-dry", x: 2, width: 4, y: 6, height: H / 2 - 6 }));
        let level = mix;
        for (let n = 1, t = time; t < seconds && level > 0.01; ++n, t += time, level *= feedback) {
          const x = 2 + (t / seconds) * (W - 8), h = level * (H / 2 - 6);
          const up = !pingPong || n % 2 === 1;
          taps.append(svg("rect", { class: "fx-tap", x, width: 4, y: up ? H / 2 - h : H / 2, height: Math.max(1, h) }));
        }
      },
    };
  },

  reverb({ get }) {
    const element = frame("is-decay");
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    return {
      element,
      update() {
        const size = get("reverbSize") / 100, damping = get("reverbDamping") / 100, mix = get("reverbMix") / 100;
        const decay = 0.3 + size * 4; // seconds to fade out, roughly
        const points = [];
        for (let x = 0; x <= W; x += 3) {
          const t = (x / W) * 5;
          const env = Math.exp((-t * 6.9) / decay) * (1 - Math.exp(-t * 40)) * (0.35 + 0.65 * mix);
          const shimmer = 1 - damping * 0.35 * (1 + Math.sin(x * 1.7)) * 0.5;
          points.push(`${x},${(H - 4 - env * shimmer * (H - 10)).toFixed(1)}`);
        }
        const d = `M${points.join(" L")}`;
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H} L0,${H} Z`);
      },
    };
  },
};

// Modulation effects: one second of the LFO.
for (const [key, rate, depth] of [["tremolo", "tremoloRate", "tremoloDepth"], ["vibrato", "vibratoRate", "vibratoDepth"],
                                  ["chorus", "chorusRate", "chorusDepth"], ["flanger", "flangerRate", "flangerDepth"],
                                  ["phaser", "phaserRate", "phaserDepth"], ["autoWah", null, "wahSensitivity"]]) {
  views[key] = ({ get }) => {
    const element = frame("is-lfo");
    element.append(svg("line", { class: "grid", x1: 0, x2: W, y1: H / 2, y2: H / 2 }));
    const curve = path(element, "fx-curve");
    return {
      element,
      update() {
        const hz = rate ? get(rate) : 3, amount = clamp(get(depth) / 100);
        const points = [];
        for (let x = 0; x <= W; x += 2) {
          // The auto-wah follows the voice: drawn as a syllable envelope instead of a sine.
          const v = rate ? Math.sin((x / W) * 2 * Math.PI * hz * 2) : Math.pow(Math.abs(Math.sin((x / W) * Math.PI * hz)), 3) * 2 - 1;
          points.push(`${x},${(H / 2 - v * amount * (H / 2 - 6)).toFixed(1)}`);
        }
        curve.setAttribute("d", `M${points.join(" L")}`);
      },
    };
  };
}

function dynamics({ get, set, range, threshold, ratio, above }) {
  const FLOOR = -60;
  const element = frame("is-dynamics");
  const x = (db) => ((db - FLOOR) / -FLOOR) * W, y = (db) => H - ((db - FLOOR) / -FLOOR) * H;
  for (const db of [-48, -36, -24, -12]) element.append(svg("line", { class: "grid", x1: x(db), x2: x(db), y1: 0, y2: H }));
  element.append(svg("line", { class: "grid dashed", x1: 0, y1: H, x2: W, y2: 0 }));
  const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
  const r = () => range(threshold) ?? { min: -60, max: 0 };
  const point = handle(element, (px, py, first) => set(threshold, Math.round(clamp(FLOOR + (px / W) * -FLOOR, r().min, r().max) * 10) / 10, first),
                       "Threshold: drag sideways");

  return {
    element,
    update() {
      const t = get(threshold), k = Math.max(1, ratio());
      const out = (db) => (above ? (db > t ? t + (db - t) / k : db) : (db < t ? t + (db - t) * k : db));
      const points = [];
      for (let db = FLOOR; db <= 0; db += 1) points.push(`${x(db).toFixed(1)},${clamp(y(out(db)), 0, H + 40).toFixed(1)}`);
      const d = `M${points.join(" L")}`;
      curve.setAttribute("d", d);
      fill.setAttribute("d", `${d} L${W},${H} L0,${H} Z`);
      point.move(x(t), y(t));
    },
  };
}

export function createFxView(moduleKey, access) {
  const factory = views[moduleKey];
  if (!factory) return null;
  const view = factory(access);

  const wrapper = document.createElement("div");
  wrapper.className = "fx-view";
  wrapper.append(view.element);
  for (const { x, y, text } of view.element.labels) {
    const span = Object.assign(document.createElement("span"), { className: "fx-label", textContent: text });
    span.style.left = `${(x / W) * 100}%`;
    span.style.top = `${(y / H) * 100}%`;
    wrapper.append(span);
  }

  view.update();
  return { element: wrapper, update: view.update };
}
