// Pictures of what an effect does, drawn from its current values: EQ and filter
// response curves, compressor and gate transfer curves, echo taps, reverb decay,
// LFO shapes, the saturation and bitcrusher curves. Some have handles you can drag
// (EQ bands, high-pass, compressor and gate thresholds).
//
//   createFxView (moduleKey, { get (param), set (param, value, first), range (param) })
//     -> { element, update(), live (reading) } or null when the effect has no picture.
//
// live (reading) animates the picture from the signal, about 30 times a second:
// reading is { in, out } peak dB going into and out of the effect (plus, for the
// frequency pictures, spectrum { pre, post }: dB bands, log-spaced 20 Hz..20 kHz), or
// null while the effect is off or silent. The spectrum sits behind the EQ and filter
// curves, a dot rides the compressor and gate curves at the real level, echo taps
// replay your voice, the LFOs show where they are...
//
// moduleKey is the C++ fx::getModuleKey name ("eq", "compressor"...). The curves
// follow the DSP in src/dsp (same filter types, frequencies and Qs).
import { clamp, html, svg } from "./widgets.js";

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

//============================================================================== live helpers
const gainOf = (db) => Math.pow(10, (db ?? -100) / 20);
// 0 (silent, -60 dB) .. 1 (full scale).
const levelOf = (db) => clamp(((db ?? -100) + 60) / 60);

// A round dot that keeps its shape however the picture stretches.
function dot(element, className = "fx-dot") {
  const p = svg("path", { class: className });
  element.append(p);
  return {
    move(x, y, visible = true) {
      p.setAttribute("d", visible ? `M${x.toFixed(1)} ${y.toFixed(1)} l0.01 0` : "");
    },
  };
}

// The live spectrum behind a frequency picture: what comes out, filled, and what goes
// in, as a thin line. Bands fall back slowly so the picture doesn't flicker. Tilted
// +3 dB per octave around 1 kHz, as analysers do, so a voice reads level.
function spectrumLayer(element) {
  const pre = path(element, "fx-spectrum-in"), post = path(element, "fx-spectrum");
  let shownPre = null, shownPost = null;
  const yOf = (db, i, count) => {
    const f = 20 * Math.pow(1000, (i + 0.5) / count);
    return H - clamp((db + 3 * Math.log2(f / 1000) + 84) / 84) * H * 0.8; // the top fifth stays clear for the curve
  };
  const fall = (shown, bands) => bands.map((v, i) => Math.max(v, (shown?.[i] ?? -100) - 2.5));
  const line = (bands) => bands.map((db, i) => `${(((i + 0.5) / bands.length) * W).toFixed(1)},${yOf(db, i, bands.length).toFixed(1)}`);

  return (reading) => {
    const spectrum = reading?.spectrum;
    if (!spectrum?.post?.length) {
      shownPre = shownPost = null;
      pre.setAttribute("d", "");
      post.setAttribute("d", "");
      return;
    }
    shownPre = fall(shownPre, spectrum.pre);
    shownPost = fall(shownPost, spectrum.post);
    pre.setAttribute("d", `M${line(shownPre).join(" L")}`);
    post.setAttribute("d", `M0,${H} L${line(shownPost).join(" L")} L${W},${H} Z`);
  };
}

//============================================================================== views
const views = {
  eq({ get, set, range }) {
    const RANGE = 15;
    const element = frame("is-eq");
    frequencyGrid(element, RANGE);
    const live = spectrumLayer(element);
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
      live,
    };
  },

  // The parametric EQ (src/dsp/ParametricEq.h): 24 dB/oct cuts (two Butterworth
  // stages), 0.707-Q shelves and three bells. Drag a point for frequency and gain;
  // scroll on a bell to change its Q.
  paraEq({ get, set, range }) {
    const RANGE = 18;
    const element = frame("is-eq");
    frequencyGrid(element, RANGE);
    const live = spectrumLayer(element);
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
      live,
    };
  },

  highPass({ get, set, range }) {
    const RANGE = 24;
    const element = frame("is-filter");
    frequencyGrid(element);
    const live = spectrumLayer(element);
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
      live,
    };
  },

  radio({ get }) {
    const RANGE = 24;
    const element = frame("is-filter");
    frequencyGrid(element);
    const live = spectrumLayer(element);
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
      live,
    };
  },

  // A 24 dB/oct ladder, drawn as two biquads; resonance raises the peak.
  filter({ get }) {
    const RANGE = 24;
    const element = frame("is-filter");
    frequencyGrid(element);
    const live = spectrumLayer(element);
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
      live,
    };
  },

  deEsser({ get }) {
    const element = frame("is-filter");
    frequencyGrid(element);
    const spectrum = spectrumLayer(element);
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
      // The band lights up while it's taking the esses down.
      live(reading) {
        spectrum(reading);
        const reduction = reading ? Math.max(0, reading.in - reading.out) : 0;
        band.style.opacity = reading ? 0.45 + clamp(reduction / 6) * 0.55 : "";
        return reduction >= 0.5 ? `−${reduction.toFixed(1)} dB` : "";
      },
    };
  },

  // The noise profile it's working from (the shaded floor, per band, low to high), and
  // how far each band is turned down right now (the bars hanging from the top). While
  // Learn is on, the caption counts the seconds of room noise heard.
  denoise({ get }) {
    const BANDS = 32, LOW = 60, HIGH = 16000;       // SpectralDenoiser::numBands, bandLowHz, bandHighHz
    const FLOOR = -110, CEILING = -30, MAX_CUT = 40;
    const element = frame("is-denoise");
    frequencyGrid(element);
    const edges = Array.from({ length: BANDS + 1 }, (_, b) => xOfF(LOW * Math.pow(HIGH / LOW, b / BANDS)));
    const profile = path(element, "fx-fill"), outline = path(element, "fx-curve");
    const bars = edges.slice(0, BANDS).map((x, b) => {
      const bar = svg("rect", { class: "fx-reduction", x: x + 0.4, width: Math.max(0.6, edges[b + 1] - x - 0.8), y: 0, height: 0 });
      element.append(bar);
      return bar;
    });
    const floor = svg("line", { class: "fx-threshold", x1: 0, x2: W });
    element.append(floor);
    label(element, 3, 11, "Noise");
    let shown = new Array(BANDS).fill(0);

    return {
      element,
      // The dashed line: the most any band can be turned down (Reduction).
      update() {
        const y = clamp(get("denoiseReduction") / MAX_CUT) * H * 0.6;
        floor.setAttribute("y1", y);
        floor.setAttribute("y2", y);
      },
      live(reading) {
        const noise = reading?.noise ?? [], cut = reading?.reduction ?? [];
        if (noise.length === BANDS && noise.some((db) => db > -99)) {
          const points = noise.map((db, b) => {
            const x = (edges[b] + edges[b + 1]) / 2;
            return `${x.toFixed(1)},${(H - clamp((db - FLOOR) / (CEILING - FLOOR)) * H * 0.75).toFixed(1)}`;
          });
          outline.setAttribute("d", `M${points.join(" L")}`);
          profile.setAttribute("d", `M${edges[0].toFixed(1)},${H} L${points.join(" L")} L${edges[BANDS].toFixed(1)},${H} Z`);
        } else {
          outline.setAttribute("d", "");
          profile.setAttribute("d", "");
        }
        shown = shown.map((v, b) => Math.max(cut[b] ?? 0, v - 0.75));
        bars.forEach((bar, b) => bar.setAttribute("height", clamp(shown[b] / MAX_CUT) * H * 0.6));
        if (reading?.learning !== undefined) return `Learning ${reading.learning.toFixed(1)} s`;
        const average = shown.reduce((sum, v) => sum + v, 0) / BANDS;
        return reading && average >= 0.5 ? `−${average.toFixed(1)} dB` : "";
      },
    };
  },

  // Speaker cabinet: its frequency response (from C++: the built-in model with the mic and
  // cuts, or the loaded impulse response) over the live spectrum.
  cab() {
    const RANGE = 24, BANDS = 48, LOW = 40, HIGH = 16000; // modules::Cab::pictureBands, getPictureHz
    const element = frame("is-filter");
    frequencyGrid(element);
    const spectrum = spectrumLayer(element);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const xs = Array.from({ length: BANDS }, (_, i) => xOfF(LOW * Math.pow(HIGH / LOW, i / (BANDS - 1))));
    let drawn = "";
    return {
      element,
      update() {},
      live(reading) {
        spectrum(reading);
        const response = reading?.response;
        if (Array.isArray(response) && response.length === BANDS) {
          const d = `M${response.map((db, i) => `${xs[i].toFixed(1)},${yOfDb(clamp(db, -RANGE, RANGE), RANGE).toFixed(1)}`).join(" L")}`;
          if (d !== drawn) {
            drawn = d;
            curve.setAttribute("d", d);
            fill.setAttribute("d", `${d} L${xs[BANDS - 1].toFixed(1)},${H} L${xs[0].toFixed(1)},${H} Z`);
          }
        }
        return "";
      },
    };
  },

  // Amp: its tone stack's curve (bass / mid / treble), relative to the knobs at noon.
  amp(access) { return views.cab(access); },

  // IR reverb: the room's decay (built-in space or the loaded file's length), after the
  // pre-delay, with the early reflections as ticks. Mix sets how tall it stands.
  irReverb({ get }) {
    const SPACES = [[0.45, 18], [0.8, 28], [1.7, 0], [2.3, 60], [4.0, 90]]; // spaces::getSpaces(): decay s, early ms
    const SPAN = 4.5; // seconds across the picture
    const element = frame("is-decay");
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const ticks = path(element, "fx-curve");
    let fileSeconds = 0;
    const draw = () => {
      const space = Math.round(get("irvSpace"));
      const [decay, early] = SPACES[space] ?? [Math.max(0.2, fileSeconds * 0.6), 30];
      const mix = clamp(get("irvMix") / 100);
      const x0 = (get("irvPreDelay") / 1000 / SPAN) * W;
      const top = H - 6 - (H - 14) * Math.sin(mix * Math.PI / 2);
      const points = [];
      for (let i = 0; i <= 60; i++) {
        const t = (i / 60) * SPAN;
        const level = Math.exp(-6.9 * t / decay); // -60 dB at the decay time
        points.push(`${(x0 + (t / SPAN) * W).toFixed(1)},${(H - 6 - (H - 6 - top) * Math.max(0, 1 + Math.log10(Math.max(level, 1e-3)) / 3)).toFixed(1)}`);
      }
      const d = `M${x0.toFixed(1)},${H - 6} L${points.join(" L")}`;
      curve.setAttribute("d", d);
      fill.setAttribute("d", `${d} L${W},${H - 6} Z`);
      const tickD = [];
      for (let k = 1; early > 0 && k <= 6; k++) {
        const x = x0 + ((early * k / 6) / 1000 / SPAN) * W;
        tickD.push(`M${x.toFixed(1)},${H - 6} L${x.toFixed(1)},${(top + (H - 6 - top) * (k / 8)).toFixed(1)}`);
      }
      ticks.setAttribute("d", tickD.join(" "));
    };
    label(element, 3, 11, "Decay");
    return {
      element,
      update: draw,
      live(reading) {
        if (reading?.seconds && Math.abs(reading.seconds - fileSeconds) > 0.01) {
          fileSeconds = reading.seconds;
          draw();
        }
        return reading?.seconds ? `${reading.seconds.toFixed(1)} s` : "";
      },
    };
  },

  // Resonance suppressor: the live spectrum behind, the Focus range shaded, and the cut
  // right now hanging from the top (deepest bin per band, so a narrow cut shows at its
  // full depth). The dashed line is Depth, the most any frequency comes down.
  smooth({ get }) {
    const BANDS = 32, LOW = 40, HIGH = 18000;      // ResonanceSuppressor::numBands, bandLowHz, bandHighHz
    const MAX_CUT = 24;                            // ResonanceSuppressor::maxDepthDb
    const element = frame("is-filter");
    frequencyGrid(element);
    const spectrum = spectrumLayer(element);
    const range = svg("rect", { class: "fx-band", y: 0, height: H, opacity: 0.35 });
    element.insertBefore(range, element.firstChild);
    const fill = path(element, "fx-reduction"), curve = path(element, "fx-curve");
    const depth = svg("line", { class: "fx-threshold", x1: 0, x2: W });
    element.append(depth);
    label(element, 3, 11, "Cut");
    const centres = Array.from({ length: BANDS }, (_, b) => xOfF(LOW * Math.pow(HIGH / LOW, (b + 0.5) / BANDS)));
    const yOfCut = (db) => clamp(db / MAX_CUT) * H * 0.7;
    let shown = new Array(BANDS).fill(0);

    return {
      element,
      update() {
        const x0 = xOfF(get("smoothLowHz")), x1 = xOfF(get("smoothHighHz"));
        range.setAttribute("x", Math.min(x0, x1));
        range.setAttribute("width", Math.max(1, Math.abs(x1 - x0)));
        const y = yOfCut(get("smoothDepth"));
        depth.setAttribute("y1", y);
        depth.setAttribute("y2", y);
      },
      live(reading) {
        spectrum(reading);
        const cut = reading?.reduction ?? [];
        shown = shown.map((v, b) => Math.max(cut[b] ?? 0, v - 0.6)); // falls back gently
        if (!reading || shown.every((v) => v < 0.05)) {
          fill.setAttribute("d", "");
          curve.setAttribute("d", "");
        } else {
          const points = shown.map((db, b) => `${centres[b].toFixed(1)},${yOfCut(db).toFixed(1)}`);
          const d = `M${points.join(" L")}`;
          curve.setAttribute("d", d);
          fill.setAttribute("d", `M${centres[0].toFixed(1)},0 L${points.join(" L")} L${centres[BANDS - 1].toFixed(1)},0 Z`);
        }
        const deepest = Math.max(reading?.deepest ?? 0, ...cut);
        return reading && deepest >= 0.5 ? `−${deepest.toFixed(1)} dB` : "";
      },
    };
  },

  // Input level (x) against output level (y), -60..0 dB.
  // Mastering (src/dsp/Mastering.h).
  masterEq({ get, set, range }) {
    const RANGE = 12, AIR = 16000;
    const element = frame("is-eq");
    frequencyGrid(element, RANGE);
    const live = spectrumLayer(element);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const r = (p, fallback) => range(p) ?? fallback;
    const setFreq = (p, x, first) => set(p, Math.round(clamp(fOfX(x), r(p, { min: 20 }).min, r(p, { max: 20000 }).max)), first);
    const setGain = (p, y, first) => set(p, Math.round(clamp(dbOfY(y, RANGE), -12, 12) * 10) / 10, first);
    const band = (freq, gain, title) => handle(element, (x, y, first) => { setFreq(freq, x, first); setGain(gain, y, false); }, title);
    const lowCut = handle(element, (x, y, first) => setFreq("meqLowCut", x, first), "Low cut: drag sideways");
    const low = band("meqLowFreq", "meqLowGain", "Low shelf: drag to set frequency and gain");
    const mid = band("meqMidFreq", "meqMidGain", "Wide bell: drag to set frequency and gain");
    const high = band("meqHighFreq", "meqHighGain", "High shelf: drag to set frequency and gain");
    const air = handle(element, (x, y, first) => setGain("meqAirGain", y, first), "Air (16 kHz shelf): drag up or down");

    return {
      element,
      update() {
        const filters = [biquad("highPass", get("meqLowCut"), 0.707), biquad("lowShelf", get("meqLowFreq"), 0.707, get("meqLowGain")),
                         biquad("peak", get("meqMidFreq"), 0.7, get("meqMidGain")), biquad("highShelf", get("meqHighFreq"), 0.707, get("meqHighGain")),
                         biquad("highShelf", AIR, 0.707, get("meqAirGain"))];
        const d = responsePath(filters, RANGE);
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H / 2} L0,${H / 2} Z`);
        lowCut.move(xOfF(get("meqLowCut")), yOfDb(-3, RANGE));
        low.move(xOfF(get("meqLowFreq")), yOfDb(get("meqLowGain"), RANGE));
        mid.move(xOfF(get("meqMidFreq")), yOfDb(get("meqMidGain"), RANGE));
        high.move(xOfF(get("meqHighFreq")), yOfDb(get("meqHighGain"), RANGE));
        air.move(xOfF(AIR), yOfDb(get("meqAirGain"), RANGE));
      },
      live,
    };
  },

  // Three bands split at the crossovers (drag them), each with its gain reduction.
  multiband({ get, set, range }) {
    const element = frame("is-multiband");
    frequencyGrid(element);
    const bars = [0, 1, 2].map(() => {
      const bar = svg("rect", { class: "fx-reduction", y: 0, height: 0 });
      element.append(bar);
      return bar;
    });
    const splits = ["mbLowFreq", "mbHighFreq"].map((param) => {
      const line = svg("line", { class: "fx-curve", y1: 0, y2: H });
      element.append(line);
      return line;
    });
    const r = (p, fallback) => range(p) ?? fallback;
    const handles = ["mbLowFreq", "mbHighFreq"].map((param, i) => handle(element,
      (x, y, first) => set(param, Math.round(clamp(fOfX(x), r(param, { min: 40 }).min, r(param, { max: 12000 }).max)), first),
      i === 0 ? "Low / mid crossover: drag sideways" : "Mid / high crossover: drag sideways"));
    ["Low", "Mid", "High"].forEach((text, i) => label(element, [8, W * 0.42, W * 0.82][i], 11, text));
    let shown = [0, 0, 0];
    const edges = () => [0, xOfF(get("mbLowFreq")), xOfF(get("mbHighFreq")), W];

    return {
      element,
      update() {
        const x = edges();
        splits.forEach((line, i) => { line.setAttribute("x1", x[i + 1]); line.setAttribute("x2", x[i + 1]); });
        handles.forEach((h, i) => h.move(x[i + 1], H / 2));
        bars.forEach((bar, i) => { bar.setAttribute("x", x[i] + 2); bar.setAttribute("width", Math.max(2, x[i + 1] - x[i] - 4)); });
      },
      live(reading) {
        const values = reading?.bands ?? [0, 0, 0];
        shown = shown.map((v, i) => Math.max(values[i] ?? 0, v - 0.75));
        bars.forEach((bar, i) => bar.setAttribute("height", clamp(shown[i] / 12) * H));
        return reading && shown.some((v) => v >= 0.5) ? shown.map((v) => `−${v.toFixed(1)}`).join(" / ") + " dB" : "";
      },
    };
  },

  // Level in and out on a -24..0 dBFS scale, the ceiling line, and the gain reduction.
  limiter({ get }) {
    const FLOOR = -24;
    const element = frame("is-limiter");
    const y = (db) => clamp(db / FLOOR) * H;
    for (const db of [-6, -12, -18]) element.append(svg("line", { class: "grid", x1: 0, x2: W, y1: y(db), y2: y(db) }));
    const ceiling = svg("line", { class: "fx-curve", x1: 0, x2: W });
    const input = svg("rect", { class: "fx-fill", x: W * 0.15, width: W * 0.25 });
    const output = svg("rect", { class: "fx-spectrum", x: W * 0.5, width: W * 0.25 });
    const reductionBar = svg("rect", { class: "fx-reduction", x: W - 8, width: 8, y: 0, height: 0 });
    element.append(input, output, ceiling, reductionBar);
    label(element, W * 0.15 + 2, H - 3, "In");
    label(element, W * 0.5 + 2, H - 3, "Out");
    let shownIn = -100, shownOut = -100, shownReduction = 0;
    const bar = (rect, db) => { const top = y(clamp(db, FLOOR, 6)); rect.setAttribute("y", top); rect.setAttribute("height", Math.max(0, H - top)); };

    return {
      element,
      update() {
        const c = y(get("limCeiling"));
        ceiling.setAttribute("y1", c);
        ceiling.setAttribute("y2", c);
      },
      live(reading) {
        shownIn = Math.max(reading?.in ?? -100, shownIn - 1.5);
        shownOut = Math.max(reading?.out ?? -100, shownOut - 1.5);
        shownReduction = Math.max(reading?.reduction ?? 0, shownReduction - 0.75);
        bar(input, shownIn);
        bar(output, shownOut);
        reductionBar.setAttribute("height", clamp(shownReduction / 12) * H);
        return reading && shownReduction >= 0.1 ? `−${shownReduction.toFixed(1)} dB` : "";
      },
    };
  },

  // The stereo spread as a fan (narrow to wide), and a correlation bar along the bottom.
  imager({ get }) {
    const element = frame("is-imager");
    const fan = path(element, "fx-fill"), edge = path(element, "fx-curve");
    element.append(svg("line", { class: "grid", x1: W / 2, x2: W / 2, y1: 0, y2: H }));
    const correlation = svg("rect", { class: "fx-spectrum", y: H - 6, height: 6 });
    element.append(correlation);
    label(element, 3, H - 9, "−1");
    label(element, W - 14, H - 9, "+1");
    let shown = 1;

    return {
      element,
      update() {
        const spread = clamp(get("imgWidth") / 200) * (W / 2 - 8);
        const tip = `M${W / 2},${H - 10} L${W / 2 - spread},8 L${W / 2 + spread},8 Z`;
        fan.setAttribute("d", tip);
        edge.setAttribute("d", `M${W / 2 - spread},8 L${W / 2},${H - 10} L${W / 2 + spread},8`);
      },
      live(reading) {
        if (reading?.correlation !== undefined) shown += (reading.correlation - shown) * 0.3;
        const x = ((shown + 1) / 2) * W;
        correlation.setAttribute("x", Math.min(W / 2, x));
        correlation.setAttribute("width", Math.abs(x - W / 2));
        return reading ? `corr ${shown.toFixed(2)}` : "";
      },
    };
  },

  // Momentary and short-term loudness as bars on a -36..0 LUFS scale with the target
  // line; integrated loudness and the peak in the corner.
  loudness({ get }) {
    const FLOOR = -36;
    const element = frame("is-loudness");
    const x = (lufs) => clamp((lufs - FLOOR) / -FLOOR) * W;
    for (const lufs of [-30, -24, -18, -12, -6]) element.append(svg("line", { class: "grid", x1: x(lufs), x2: x(lufs), y1: 0, y2: H }));
    for (const lufs of [-24, -12]) label(element, x(lufs) + 2, H - 3, String(lufs));
    const momentary = svg("rect", { class: "fx-fill", x: 0, y: 14, height: 26, width: 0 });
    const shortTerm = svg("rect", { class: "fx-spectrum", x: 0, y: 50, height: 26, width: 0 });
    const target = svg("line", { class: "fx-curve", y1: 0, y2: H });
    element.append(momentary, shortTerm, target);
    label(element, 3, 12, "Momentary");
    label(element, 3, 48, "Short-term");
    let shownM = -100;

    return {
      element,
      update() {
        target.setAttribute("x1", x(get("loudTarget")));
        target.setAttribute("x2", x(get("loudTarget")));
      },
      live(reading) {
        if (!reading) { momentary.setAttribute("width", 0); shortTerm.setAttribute("width", 0); return ""; }
        shownM = Math.max(reading.momentary ?? -100, shownM - 0.6);
        momentary.setAttribute("width", x(shownM));
        shortTerm.setAttribute("width", x(reading.shortTerm ?? -100));
        const integrated = reading.integrated > -99 ? `${reading.integrated.toFixed(1)} LUFS` : "– LUFS";
        const off = reading.integrated > -99 ? ` (${reading.integrated - reading.target >= 0 ? "+" : "−"}${Math.abs(reading.integrated - reading.target).toFixed(1)})` : "";
        return `I ${integrated}${off}`;
      },
    };
  },

  // Auto level: the ride (the gain it applies, dB) over the last few seconds, between
  // +/- range; the corner says how loud the voice is against what it aims at.
  autoLevel({ get }) {
    const element = frame("is-ride");
    element.append(svg("line", { class: "grid zero", x1: 0, x2: W, y1: H / 2, y2: H / 2 }));
    const top = svg("line", { class: "grid dashed", x1: 0, x2: W }), bottom = svg("line", { class: "grid dashed", x1: 0, x2: W });
    element.append(top, bottom);
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const marker = dot(element);
    label(element, 3, 11, "Up");
    label(element, 3, H - 3, "Down");
    const history = [];
    const HISTORY = 120; // about 4 s of readings
    const scale = () => Math.max(3, get("alRange") + 1);
    const yOf = (db) => yOfDb(db, scale());

    const draw = () => {
      const points = history.map((db, i) => `${((i / (HISTORY - 1)) * W).toFixed(1)},${yOf(db).toFixed(1)}`);
      if (!points.length) { curve.setAttribute("d", ""); fill.setAttribute("d", ""); marker.move(0, 0, false); return; }
      curve.setAttribute("d", `M${points.join(" L")}`);
      fill.setAttribute("d", `M0,${H / 2} L${points.join(" L")} L${(((history.length - 1) / (HISTORY - 1)) * W).toFixed(1)},${H / 2} Z`);
      marker.move(((history.length - 1) / (HISTORY - 1)) * W, yOf(history[history.length - 1]));
    };

    return {
      element,
      update() {
        for (const [line, db] of [[top, get("alRange")], [bottom, -get("alRange")]]) {
          line.setAttribute("y1", yOf(db));
          line.setAttribute("y2", yOf(db));
        }
        draw();
      },
      live(reading) {
        if (!reading) { history.length = 0; draw(); return ""; }
        history.push(reading.ride ?? 0);
        if (history.length > HISTORY) history.shift();
        draw();
        const ride = reading.ride ?? 0;
        const sign = ride >= 0.05 ? "+" : ride <= -0.05 ? "−" : "";
        return reading.frozen ? `holding ${sign}${Math.abs(ride).toFixed(1)} dB`
                              : `${sign}${Math.abs(ride).toFixed(1)} dB → ${Math.round(reading.target)} LUFS`;
      },
    };
  },

  compressor({ get, set, range }) {
    // Glue always has at least a 10 dB knee (src/dsp/VocalDynamics.h).
    const knee = () => (Math.round(get("compStyle") ?? 0) === 2 ? Math.max(10, get("compKnee") ?? 0) : get("compKnee") ?? 0);
    return dynamics({ get, set, range, threshold: "compThreshold", ratio: () => get("compRatio"), above: true, knee });
  },

  gate({ get, set, range }) {
    return dynamics({ get, set, range, threshold: "gateThreshold", ratio: () => 10, above: false, floor: () => get("gateRange") ?? 80 });
  },

  // The opto's curve: the threshold falls as Amount rises, and the ratio grows with
  // level over a 12 dB knee (as vocal::OptoCompressor), plus its gain reduction.
  opto({ get }) {
    const FLOOR = -60;
    const element = frame("is-dynamics");
    const x = (db) => ((db - FLOOR) / -FLOOR) * W, y = (db) => H - ((db - FLOOR) / -FLOOR) * H;
    for (const db of [-48, -36, -24, -12]) element.append(svg("line", { class: "grid", x1: x(db), x2: x(db), y1: 0, y2: H }));
    element.append(svg("line", { class: "grid dashed", x1: 0, y1: H, x2: W, y2: 0 }));
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    const reductionBar = svg("rect", { class: "fx-reduction", x: W - 5, width: 5, y: 0, height: 0 });
    element.append(reductionBar);
    let shown = 0;
    return {
      element,
      update() {
        const t = -4 - 34 * clamp((get("optoAmount") ?? 30) / 100), limit = Math.round(get("optoMode") ?? 0) === 1;
        const points = [];
        for (let db = FLOOR; db <= 0; db += 1) {
          const over = clamp((db - t) / 20), ratio = limit ? 8 + 8 * over : 2.5 + 2 * over;
          const out = db - kneeReduction(db, t, 1 - 1 / ratio, 12) + (get("optoOutput") ?? 0);
          points.push(`${x(db).toFixed(1)},${clamp(y(out), 0, H + 40).toFixed(1)}`);
        }
        const d = `M${points.join(" L")}`;
        curve.setAttribute("d", d);
        fill.setAttribute("d", `${d} L${W},${H} L0,${H} Z`);
      },
      live(reading) {
        shown = Math.max(reading?.reduction ?? 0, shown - 0.5);
        reductionBar.setAttribute("height", clamp(shown / 12) * H);
        return reading && shown >= 0.1 ? `−${shown.toFixed(1)} dB` : "";
      },
    };
  },

  saturation({ get }) {
    const element = frame("is-curve");
    element.append(svg("line", { class: "grid", x1: 0, x2: W, y1: H / 2, y2: H / 2 }),
                   svg("line", { class: "grid", x1: W / 2, x2: W / 2, y1: 0, y2: H }),
                   svg("line", { class: "grid dashed", x1: W / 2 - H / 2, y1: H, x2: W / 2 + H / 2, y2: 0 }));
    const curve = path(element, "fx-curve");
    const marker = dot(element);
    const shape = (x) => {
      const drive = Math.pow(10, get("satDrive") / 20), makeup = 1 / Math.pow(drive, 0.7), mix = get("satMix") / 100;
      return x + mix * (Math.tanh(drive * x) * makeup - x);
    };
    const at = (x) => [W / 2 + x * (H / 2 - 4), H / 2 - clamp(shape(x), -1.1, 1.1) * (H / 2 - 4)];
    let peak = 0;
    return {
      element,
      update() {
        const points = [];
        for (let i = 0; i <= 60; ++i) points.push(at(-1 + (2 * i) / 60).map((v) => v.toFixed(1)).join(","));
        curve.setAttribute("d", `M${points.join(" L")}`);
      },
      // Your loudest peaks ride up the curve, into the bend when you push it.
      live(reading) {
        peak = reading ? Math.max(clamp(gainOf(reading.in)), peak * 0.8) : 0;
        marker.move(...at(peak), peak > 0.002);
        element.style.setProperty("--drive", reading ? clamp(peak * 1.5) : 0);
      },
    };
  },

  bitcrusher({ get }) {
    const element = frame("is-curve");
    element.append(svg("line", { class: "grid", x1: 0, x2: W, y1: H / 2, y2: H / 2 }));
    const ghost = path(element, "fx-ghost"), curve = path(element, "fx-curve");
    let amplitude = 0.9, phase = 0; // live: the wave scrolls and follows your level
    return {
      element,
      live(reading) {
        const target = reading ? 0.08 + clamp(gainOf(reading.in) * 2) * 0.82 : 0.9;
        amplitude = Math.max(target, amplitude * 0.85);
        phase = reading ? (phase + 0.35) % (Math.PI * 2) : 0;
        this.update();
      },
      update() {
        const levels = Math.pow(2, Math.max(1, get("crushBits")) - 1);
        const hold = Math.max(1, Math.round(48000 / Math.max(100, get("crushRate")) / 6)); // drawn 6x slower so steps show
        const mix = get("crushMix") / 100;
        const clean = [], crushed = [];
        let held = 0;
        for (let i = 0; i <= W; ++i) {
          const x = Math.sin((i / W) * Math.PI * 4 - phase) * amplitude;
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
    // Live: the input level over the last two seconds, so each tap replays what you
    // sang one echo time (two, three...) ago.
    let history = null;
    const levelAgo = (seconds) => {
      if (!history) return 1;
      const t = performance.now() / 1000 - seconds;
      for (let i = history.length - 1; i >= 0; --i) if (history[i][0] <= t) return history[i][1];
      return 0;
    };
    return {
      element,
      live(reading) {
        if (!reading) { if (history) { history = null; this.update(); } return; }
        const now = performance.now() / 1000;
        history = (history ?? []).filter(([t]) => t > now - 2.2);
        history.push([now, levelOf(reading.in)]);
        this.update();
      },
      update() {
        const seconds = 2, time = Math.max(0.01, get("delayTime") / 1000), feedback = get("delayFeedback") / 100;
        const mix = get("delayMix") / 100, pingPong = get("delayPingPong") >= 0.5;
        const dry = history ? Math.max(0.04, levelAgo(0)) : 1;
        taps.replaceChildren(svg("rect", { class: "fx-tap is-dry", x: 2, width: 4, y: H / 2 - dry * (H / 2 - 6), height: Math.max(1, dry * (H / 2 - 6)) }));
        let level = mix;
        for (let n = 1, t = time; t < seconds && level > 0.01; ++n, t += time, level *= feedback) {
          const x = 2 + (t / seconds) * (W - 8), h = level * (history ? Math.max(0.04, levelAgo(t)) : 1) * (H / 2 - 6);
          const up = !pingPong || n % 2 === 1;
          taps.append(svg("rect", { class: "fx-tap", x, width: 4, y: up ? H / 2 - h : H / 2, height: Math.max(1, h) }));
        }
      },
    };
  },

  reverb({ get }) {
    const element = frame("is-decay");
    const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
    let energy = 1; // live: the tail swells with what comes out and dies away with it
    return {
      element,
      live(reading) {
        energy = reading ? Math.max(0.05 + levelOf(reading.out) * 0.95, energy * 0.93) : 1;
        this.update();
      },
      update() {
        const size = get("reverbSize") / 100, damping = get("reverbDamping") / 100, mix = get("reverbMix") / 100 * energy;
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
    const marker = dot(element);
    const valueAt = (x) => {
      const hz = rate ? get(rate) : 3;
      // The auto-wah follows the voice: drawn as a syllable envelope instead of a sine.
      return rate ? Math.sin((x / W) * 2 * Math.PI * hz * 2) : Math.pow(Math.abs(Math.sin((x / W) * Math.PI * hz)), 3) * 2 - 1;
    };
    let envelope = 0;
    return {
      element,
      update() {
        const amount = clamp(get(depth) / 100);
        const points = [];
        for (let x = 0; x <= W; x += 2) points.push(`${x},${(H / 2 - valueAt(x) * amount * (H / 2 - 6)).toFixed(1)}`);
        curve.setAttribute("d", `M${points.join(" L")}`);
      },
      // Live: a dot sweeps the LFO in real time (the picture is half a second wide), or,
      // for the auto-wah, rides your voice's level; it fades when you stop.
      live(reading) {
        envelope = reading ? Math.max(levelOf(reading.in), envelope * 0.8) : 0;
        const amount = clamp(get(depth) / 100);
        if (rate) {
          const x = ((performance.now() / 500) % 1) * W;
          marker.move(x, H / 2 - valueAt(x) * amount * (H / 2 - 6), envelope > 0.05);
        } else {
          marker.move(W / 2, H / 2 - (envelope * 2 - 1) * amount * (H / 2 - 6), envelope > 0.05);
        }
        element.style.setProperty("--live", envelope);
      },
    };
  };
}

// Gain reduction (dB) of a soft-knee curve, as reductionFor in src/dsp/VocalDynamics.h.
function kneeReduction(db, threshold, slope, knee) {
  const over = db - threshold;
  if (knee > 0 && 2 * Math.abs(over) < knee) return (slope * (over + knee / 2) ** 2) / (2 * knee);
  return over > 0 ? slope * over : 0;
}

function dynamics({ get, set, range, threshold, ratio, above, knee = () => 0, floor = () => 120 }) {
  const FLOOR = -60;
  const element = frame("is-dynamics");
  const x = (db) => ((db - FLOOR) / -FLOOR) * W, y = (db) => H - ((db - FLOOR) / -FLOOR) * H;
  for (const db of [-48, -36, -24, -12]) element.append(svg("line", { class: "grid", x1: x(db), x2: x(db), y1: 0, y2: H }));
  element.append(svg("line", { class: "grid dashed", x1: 0, y1: H, x2: W, y2: 0 }));
  const fill = path(element, "fx-fill"), curve = path(element, "fx-curve");
  const r = () => range(threshold) ?? { min: -60, max: 0 };
  const point = handle(element, (px, py, first) => set(threshold, Math.round(clamp(FLOOR + (px / W) * -FLOOR, r().min, r().max) * 10) / 10, first),
                       "Threshold: drag sideways");
  // Live: a bar for the gain reduction down the right edge, and a dot at the real level
  // in (across) and out (up), so you see it ride the curve.
  const reductionBar = svg("rect", { class: "fx-reduction", x: W - 5, width: 5, y: 0, height: 0 });
  element.append(reductionBar);
  const marker = dot(element);
  let shownReduction = 0, shownIn = FLOOR, shownOut = FLOOR;

  return {
    element,
    live(reading) {
      if (!reading) {
        shownReduction = 0;
        reductionBar.setAttribute("height", 0);
        marker.move(0, 0, false);
        element.classList.remove("is-closed");
        return "";
      }
      // Fast attack, slower release, like the meters.
      shownIn = Math.max(reading.in, shownIn - 2);
      shownOut = Math.max(reading.out, shownOut - 2);
      // The effect's own reading when it has one (the new engines), else in minus out.
      const reduction = reading.reduction ?? Math.max(0, shownIn - shownOut);
      shownReduction = Math.max(reduction, shownReduction - 1.5);
      reductionBar.setAttribute("height", clamp(shownReduction / 24) * H);
      const audible = shownIn > FLOOR + 1;
      marker.move(x(clamp(shownIn, FLOOR, 0)), y(clamp(shownOut, FLOOR, 0)), audible);
      if (!above) {
        const closed = audible ? shownReduction > 6 : true;
        element.classList.toggle("is-closed", closed);
        return closed ? "Closed" : "Open";
      }
      return shownReduction >= 0.5 ? `−${shownReduction.toFixed(1)} dB` : "";
    },
    update() {
      const t = get(threshold), k = Math.max(1, ratio()), w = knee(), lowest = floor();
      const out = (db) => (above ? db - kneeReduction(db, t, 1 - 1 / k, w)
                                 : (db < t ? Math.max(db - lowest, t + (db - t) * k) : db));
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

  // A corner readout for what the live picture measures (gain reduction, gate open...).
  const badge = html("span", "fx-badge");
  wrapper.append(badge);
  let lastBadge = "";

  view.update();
  return {
    element: wrapper,
    update: () => view.update(),
    live(reading) {
      const text = view.live?.(reading) ?? "";
      wrapper.classList.toggle("is-live", Boolean(reading));
      if (text !== lastBadge) badge.textContent = lastBadge = text;
    },
  };
}

// Two small level bars, in and out, for every effect card (also the ones without a
// picture). push (reading) as for live(); null shows nothing.
export function createActivity() {
  const element = html("div", "fx-activity");
  element.title = "Level going into and coming out of this effect";
  const bars = ["in", "out"].map((name) => {
    const bar = html("span", `fx-activity-bar is-${name}`);
    element.append(bar);
    return { bar, shown: 0 };
  });
  return {
    element,
    push(reading) {
      const values = reading ? [levelOf(reading.in), levelOf(reading.out)] : [0, 0];
      bars.forEach((entry, i) => {
        entry.shown = Math.max(values[i], entry.shown - 0.06);
        entry.bar.style.setProperty("--level", entry.shown.toFixed(3));
      });
      return Math.max(bars[0].shown, bars[1].shown);
    },
  };
}
