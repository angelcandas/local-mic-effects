// Audio setup in our own UI (standalone app): the status strip at the foot of both modes,
// the first-run card, and the Settings window. The audio device itself still lives in
// JUCE's audio settings dialog, which the strip and the card open.
//
//   Status strip: "Scarlett 2i2 · 48 kHz · 128 · 5.4 ms round trip · CPU 12 %". Click opens the
//   audio settings. Amber after a dropout (for 10 s), red when no device is open.
//   First-run card: 1. microphone, output and headphones, 2. Mic check, 3. Sound check.
//   Settings window: Audio, Recording, Look (UI size) and Help.

import * as Juce from "./juce/index.js";
import { html, keyText } from "./widgets.js";

const audioSetup = Juce.getNativeFunction("audioSetup");
const uiPref = Juce.getNativeFunction("uiPref");
const POLL_MS = 1000;
const DROPOUT_SHOW_MS = 10000;
const UI_SCALES = [80, 90, 100, 110, 125, 150];

const rateText = (rate) => `${Number.isInteger(rate / 1000) ? rate / 1000 : (rate / 1000).toFixed(1)} kHz`;

export function describeAudio(state) {
  if (!state?.available) return "";
  if (!state.device) return "No audio device · click to pick one";
  return [state.device, rateText(state.rate), String(state.buffer), `${Number(state.latencyMs).toFixed(1)} ms round trip`,
          `CPU ${Math.round(state.cpu)} %`].join(" · ");
}

export function initSetup() {
  let state = null, lastXruns = null, dropoutAt = -Infinity;
  const strips = [];

  // One strip in each hint line (DAW and Live); both show the same.
  for (const line of document.querySelectorAll(".hint-line")) {
    const strip = html("button", "audio-status");
    strip.type = "button";
    strip.hidden = true;
    strip.title = "Audio: device, sample rate, buffer, round-trip latency and CPU. Click: audio settings";
    strip.addEventListener("click", () => audioSetup("settings").then(show));
    line.append(strip);
    strips.push(strip);
  }

  const show = (next) => {
    if (!next || typeof next !== "object") return;
    state = next;
    if (lastXruns !== null && next.xruns > lastXruns) dropoutAt = performance.now();
    lastXruns = next.xruns ?? lastXruns;
    const dropout = performance.now() - dropoutAt < DROPOUT_SHOW_MS;
    const text = describeAudio(next) + (dropout ? " · dropout" : "") + (next.inputMuted ? " · input muted" : "");
    for (const strip of strips) {
      strip.hidden = !next.available;
      strip.textContent = text;
      strip.classList.toggle("is-warn", dropout || Boolean(next.inputMuted));
      strip.classList.toggle("is-error", Boolean(next.available && !next.device));
    }
    firstRun.update();
  };
  const firstRun = createFirstRun(() => state, show);
  const settings = createSettings(() => state, show, firstRun);

  const poll = () => { if (!document.hidden) audioSetup("status").then(show).catch(() => {}); };
  setInterval(poll, POLL_MS);
  poll();

  document.addEventListener("micfx:settings", () => settings.toggle(true));
  document.addEventListener("keydown", (event) => {
    if ((event.ctrlKey || event.metaKey) && event.key === ",") { event.preventDefault(); settings.toggle(); }
  });

  // UI size, saved in the app settings.
  uiPref("uiScale").then((value) => applyScale(Number(value) || 100)).catch(() => {});
}

function applyScale(percent) {
  document.documentElement.style.zoom = percent === 100 ? "" : String(percent / 100);
}

// A modal panel with a head and a close button. Esc or a click outside closes it.
function modal(className, title, onClose) {
  const backdrop = html("div", `setup-backdrop ${className}`);
  backdrop.hidden = true;
  const panel = html("div", "setup-panel");
  panel.setAttribute("role", "dialog");
  panel.setAttribute("aria-label", title);
  const head = html("div", "setup-head");
  const close = html("button", "setup-close", "×");
  close.type = "button";
  close.setAttribute("aria-label", "Close");
  close.addEventListener("click", onClose);
  head.append(html("h2", "", title), close);
  const body = html("div", "setup-body");
  panel.append(head, body);
  backdrop.append(panel);
  backdrop.addEventListener("pointerdown", (event) => { if (event.target === backdrop) onClose(); });
  document.addEventListener("keydown", (event) => {
    if (!backdrop.hidden && event.key === "Escape") { event.preventDefault(); event.stopPropagation(); onClose(); }
  }, true);
  document.body.append(backdrop);
  return { backdrop, body };
}

//==============================================================================
// The first-run card: three steps, each a button that does it. Shown once, until "Done";
// Settings → Help shows it again.
function createFirstRun(getState, show) {
  const { backdrop, body } = modal("first-run", "Welcome to MicFX", () => finish());
  const intro = html("p", "setup-lead", "Three steps and you're singing through your effects.");
  const step = (number, title, text, action) => {
    const row = html("div", "setup-step");
    const label = html("div", "setup-step-text");
    label.append(html("strong", "", `${number}. ${title}`), html("span", "", text));
    row.append(html("span", "setup-step-check"), label, action);
    body.append(row);
    return row;
  };
  const deviceLine = html("span", "setup-device");
  const pick = html("button", "tool", "Audio settings…");
  pick.type = "button";
  pick.addEventListener("click", () => audioSetup("settings").then(show));
  const headphones = html("button", "tool is-accent", "I'm wearing headphones");
  headphones.type = "button";
  headphones.title = "Unmutes the microphone (it stays muted until then, so the speakers can't feed back)";
  headphones.addEventListener("click", () => audioSetup("headphones").then(show));
  const audioActions = html("div", "setup-actions");
  audioActions.append(pick, headphones);
  body.append(intro);
  const one = step(1, "Microphone, output and headphones", "Pick your interface, then put headphones on: without them the speakers feed back into the mic.", audioActions);
  one.querySelector(".setup-step-text").append(deviceLine);
  const micCheck = html("button", "tool", "Mic check");
  micCheck.type = "button";
  micCheck.addEventListener("click", () => { document.querySelector(".mic-check")?.click(); two.classList.add("is-done"); });
  const two = step(2, "Mic check", "Sing for five seconds: the input gain is set so your loudest notes have room.", micCheck);
  const soundCheck = html("button", "tool", "Sound check");
  soundCheck.type = "button";
  soundCheck.addEventListener("click", () => { finish(); document.querySelector(".sound-check")?.click(); });
  step(3, "Sound check", "Sing for 10 to 20 seconds: the chain is set for your voice, mic and room. Keep it or go back.", soundCheck);
  const foot = html("div", "setup-foot");
  const done = html("button", "tool", "Done");
  done.type = "button";
  done.addEventListener("click", () => finish());
  foot.append(html("span", "setup-hint", `Help → User manual (${keyText("F1")}) explains everything else.`), done);
  body.append(foot);

  let shown = false;
  function finish() {
    backdrop.hidden = true;
    uiPref("firstRunDone", "1").catch(() => {});
  }
  const api = {
    open() { backdrop.hidden = false; api.update(); },
    update() {
      const state = getState();
      if (!state) return;
      one.classList.toggle("is-done", Boolean(state.device) && !state.inputMuted);
      headphones.hidden = !state.inputMuted;
      deviceLine.textContent = state.device ? `Now: ${describeAudio(state)}` : "No audio device is open yet.";
      // Only in the app, only once, and only in Live mode (where the steps are).
      if (!shown && state.available) {
        shown = true;
        uiPref("firstRunDone").then((value) => {
          if (!value && !document.body.classList.contains("is-daw")) api.open();
        }).catch(() => {});
      }
    },
  };
  return api;
}

//==============================================================================
// Settings: the choices that are spread over the app, in one window. The controls where
// they're used stay; these are shortcuts to the same settings.
function createSettings(getState, show, firstRun) {
  const { backdrop, body } = modal("settings-window", "Settings", () => api.toggle(false));
  const section = (title) => {
    const element = html("section", "setup-section");
    element.append(html("h3", "", title));
    body.append(element);
    return element;
  };
  const row = (parent, label, ...controls) => {
    const element = html("div", "setup-row");
    element.append(html("span", "setup-row-label", label), ...controls);
    parent.append(element);
    return element;
  };
  const action = (text, title, onClick) => {
    const b = html("button", "tool", text);
    b.type = "button";
    b.title = title;
    b.addEventListener("click", onClick);
    return b;
  };

  const audio = section("Audio");
  const deviceText = html("span", "setup-value");
  row(audio, "Device", deviceText, action("Audio settings…", "Pick the interface, inputs, outputs, sample rate and buffer size", () => audioSetup("settings").then(show)));
  const hint = html("p", "setup-note", "Smaller buffers are quicker but need more CPU. 128 samples at 48 kHz is a good start; raise it if you hear clicks.");
  audio.append(hint);

  const recording = section("Recording");
  const folderText = html("span", "setup-value");
  row(recording, "Recordings folder", folderText,
      action("Change…", "Where Live mode's recordings and saved loops go", () => document.querySelector(".record-change")?.click()));
  const dryBox = Object.assign(html("input"), { type: "checkbox" });
  dryBox.addEventListener("change", () => {
    const dry = document.querySelector(".record-dry");
    if (dry && (dry.getAttribute("aria-pressed") === "true") !== dryBox.checked) dry.click();
  });
  const dryLabel = html("label", "setup-check");
  dryLabel.append(dryBox, html("span", "", "Also save the unprocessed mic (Live mode)"));
  row(recording, "Dry copy", dryLabel);

  const look = section("Look");
  const scale = html("select", "dropdown");
  for (const percent of UI_SCALES) scale.append(new Option(`${percent} %`, String(percent)));
  scale.addEventListener("change", () => {
    applyScale(Number(scale.value));
    uiPref("uiScale", scale.value).catch(() => {});
  });
  row(look, "UI size", scale);

  const help = section("Help");
  row(help, "First steps", action("Show the welcome card", "Audio, Mic check and Sound check, step by step", () => { api.toggle(false); firstRun.open(); }));
  row(help, "Manual", action("User manual", `Opens the manual (${keyText("F1")})`, () => Juce.getNativeFunction("help")("manual")));

  const api = {
    toggle(on = backdrop.hidden) {
      backdrop.hidden = !on;
      if (!on) return;
      const state = getState();
      deviceText.textContent = state?.device ? describeAudio(state) : "No audio device";
      folderText.textContent = document.querySelector(".record-path")?.textContent || "—";
      dryBox.checked = document.querySelector(".record-dry")?.getAttribute("aria-pressed") === "true";
      uiPref("uiScale").then((value) => { scale.value = String(Number(value) || 100); }).catch(() => {});
    },
  };
  return api;
}
