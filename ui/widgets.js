// Small helpers shared by the Live screen (app.js) and the DAW screen (daw.js).

export const ARC_START = -135;
export const ARC_END = 135;
const SVG_NS = "http://www.w3.org/2000/svg";

export const clamp = (value, min = 0, max = 1) => Math.min(max, Math.max(min, value));

export function formatValue(value, label) {
  const signed = (text) => (value > 0.05 ? `+${text}` : text);

  switch (label) {
    case "Hz": return value >= 1000 ? `${(value / 1000).toFixed(value >= 10000 ? 1 : 2)} kHz`
                                    : `${value < 10 ? value.toFixed(2) : Math.round(value)} Hz`;
    case "dB": return `${signed(value.toFixed(1))} dB`;
    case "st": return `${signed(value.toFixed(Number.isInteger(value) ? 0 : 1))} st`;
    case "%": return `${Math.round(value)}%`;
    case "ms": return `${Math.round(value)} ms`;
    case "bit": return `${value.toFixed(1)} bit`;
    case ":1": return `${value.toFixed(1)}:1`;
    case "LUFS": return `${value.toFixed(1)} LUFS`;
    case "LU": return `${signed(value.toFixed(1))} LU`;
    default: return value.toFixed(2);
  }
}

export function formatDuration(seconds) {
  const tenths = Math.floor(Math.max(0, seconds) * 10);
  const minutes = Math.floor(tenths / 600);
  return `${minutes}:${((tenths % 600) / 10).toFixed(1).padStart(4, "0")}`;
}

export function svg(tag, attributes = {}) {
  const element = document.createElementNS(SVG_NS, tag);
  for (const [name, value] of Object.entries(attributes)) element.setAttribute(name, value);
  return element;
}

export function html(tag, className = "", text = "") {
  return Object.assign(document.createElement(tag), { className, textContent: text });
}

// Point on a circle in the knob's 100x100 viewBox; 0 degrees is straight up.
export function polar(angleDeg, radius) {
  const angle = (angleDeg * Math.PI) / 180;
  return [50 + radius * Math.sin(angle), 50 - radius * Math.cos(angle)];
}

export function arcPath(fromDeg, toDeg, radius) {
  const [a, b] = fromDeg <= toDeg ? [fromDeg, toDeg] : [toDeg, fromDeg];
  const [x1, y1] = polar(a, radius);
  const [x2, y2] = polar(b, radius);
  return `M ${x1} ${y1} A ${radius} ${radius} 0 ${b - a > 180 ? 1 : 0} 1 ${x2} ${y2}`;
}

// Shortcut text as the user's keyboard shows it: Ctrl and Alt are Cmd and Option on a Mac
// (the screens' shortcuts accept either Ctrl or Cmd). "Ctrl+Cmd+F" stays as it is.
export const IS_MAC = /Mac|iPhone|iPad/.test(globalThis.navigator?.platform || globalThis.navigator?.userAgent || "");
export const keyText = (text) => (IS_MAC && typeof text === "string"
  ? text.replace(/\bCtrl\+(?!Cmd)/g, "Cmd+").replace(/\bAlt\+/g, "Option+") : text);

// The file an effect plays through (an impulse response): its name, Load... and a clear
// button. load() and clear() resolve to { file, error?, cancelled? }; update(path) shows a
// file the effect has (an empty path: none).
export function createFilePicker({ label, load, clear }) {
  const element = html("div", "file-picker");
  const name = html("span", "file-name");
  const loadButton = html("button", "file-load", "Load…");
  const clearButton = html("button", "file-clear", "×");
  const message = html("span", "file-error");
  clearButton.title = "Stop using this file";
  element.append(html("span", "name", label), name, loadButton, clearButton, message);

  const update = (path) => {
    const file = typeof path === "string" ? path : "";
    const base = file.split(/[\\/]/).pop();
    name.textContent = base || "No file";
    name.title = file;
    element.classList.toggle("has-file", Boolean(base));
    clearButton.hidden = !base;
  };

  const run = async (action) => {
    message.textContent = "";
    const result = await action();
    if (result?.error) message.textContent = result.error;
    else if (result && !result.cancelled && "file" in result) update(result.file);
  };

  loadButton.addEventListener("click", () => run(load));
  clearButton.addEventListener("click", () => run(clear));
  update("");
  return { element, update };
}
