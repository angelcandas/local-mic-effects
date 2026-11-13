// The Practice view (Live mode): sing along to any song and see how close you get.
//
// The engine is src/Practice.* (the song's melody from its vocal stem, scoring) with the
// processor's PitchTap (your pitch before the effects). The page talks to it through the
// "practice" native function and hears back on three events:
//   "practice"      a song is being prepared, is ready, or failed
//   "practicePitch" { time, playing, midi, frames: [t, midi, ...] } about 30 times a second
//   "practiceScore" { reset, from, notes: [...], phrases: [...], pass } as notes and phrases end
//   "practiceReport" the pass report, when the song stops after some singing
// The song plays on the Live tracks, so the transport at the bottom drives it.
import * as Juce from "./juce/index.js";
import { clamp, formatDuration, html } from "./widgets.js";

const practice = Juce.getNativeFunction("practice");
const transport = Juce.getNativeFunction("transport");

const NOTE_NAMES = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"];
const noteName = (midi) => `${NOTE_NAMES[((Math.round(midi) % 12) + 12) % 12]}${Math.floor(Math.round(midi) / 12) - 1}`;

const IN_TUNE = 25;   // cents: green
const CLOSE = 60;     // cents: amber
const SECONDS_SHOWN = 9;
const PLAYHEAD = 0.28; // where "now" sits across the lane
const TRAIL_SECONDS = 12;

const PHASES = {
  reading: "Reading the song…",
  downloading: "Downloading the separation model (once, about 84 MB)…",
  loading: "Loading the separation model…",
  splitting: "Taking the voice out of the song…",
  listening: "Listening to the melody…",
};

// Score colours, shared with the CSS.
export const scoreClass = (score) => (score >= 80 ? "is-good" : score >= 55 ? "is-close" : "is-off");

// `sung` moved by whole octaves to the nearest one to `target`.
export const foldToOctave = (sung, target) => (sung > 0 && target > 0 ? sung - 12 * Math.round((sung - target) / 12) : sung);

export class PracticeView {
  constructor({ section, toggle, workspace }) {
    this.section = section;
    this.toggle = toggle;
    this.workspace = workspace;
    this.song = null;          // { name, notes: [{ start, end, pitch, phrase }], phrases: [{ start, end }], lowest, highest }
    this.noteScores = new Map();
    this.phraseScores = new Map();
    this.trail = [];           // [{ time, midi }]
    this.clock = { time: 0, playing: false, at: performance.now() };
    this.midiNow = 0;
    this.pass = 0;
    this.open = false;

    this.build();

    toggle.addEventListener("click", () => this.show(!this.open));
    const backend = window.__JUCE__.backend;
    backend.addEventListener("practice", (event) => this.onPractice(event));
    backend.addEventListener("practicePitch", (event) => this.onPitch(event));
    backend.addEventListener("practiceScore", (event) => this.onScore(event));
    backend.addEventListener("practiceReport", (report) => this.showReport(report));

    practice("state").then((state) => this.applyState(state)).catch(() => {});
    Juce.getNativeFunction("uiPref")("practiceShown").then((shown) => { if (shown === "1") this.show(true, false); }).catch(() => {});

    new ResizeObserver(() => this.draw()).observe(this.stage);
    const frame = () => {
      if (this.open && !document.hidden) this.draw();
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  //==============================================================================
  build() {
    const s = this.section;
    s.replaceChildren();

    // Top bar: the song, the guide level and the pass score.
    const bar = html("div", "practice-bar");
    const title = html("div", "practice-title");
    this.songLabel = html("strong", "practice-song", "No song yet");
    this.songInfo = html("small", "practice-info", "Open a song and sing its melody");
    title.append(this.songLabel, this.songInfo);

    this.openButton = Object.assign(html("button", "tool is-accent practice-open", "Open song…"),
                                    { type: "button", title: "Choose a song: its voice becomes the melody to sing and the guide, the rest the backing" });
    this.openButton.addEventListener("click", () => practice("open", { split: true }));
    this.voiceButton = Object.assign(html("button", "text-button practice-voice", "Voice only…"),
                                     { type: "button", title: "Choose a recording of a voice on its own (an a cappella or guide vocal): no splitting" });
    this.voiceButton.addEventListener("click", () => practice("open", { split: false }));

    const guide = html("label", "practice-guide");
    guide.title = "How loud the original singer plays under you (all the way down mutes it)";
    this.guideSlider = Object.assign(document.createElement("input"), { type: "range", min: 0, max: 150, step: 1, value: 35 });
    this.guideValue = html("span", "practice-guide-value", "35%");
    this.guideSlider.addEventListener("input", () => {
      const level = Number(this.guideSlider.value) / 100;
      this.guideValue.textContent = level <= 0 ? "Off" : `${Math.round(level * 100)}%`;
      practice("guide", { level });
    });
    guide.append(html("span", "", "Guide voice"), this.guideSlider, this.guideValue);

    const restart = Object.assign(html("button", "tool practice-restart", "From the top"), { type: "button", title: "Back to the start and play" });
    restart.addEventListener("click", async () => {
      await transport("stop");
      await practice("reset");
      transport("play");
    });

    this.reportButton = Object.assign(html("button", "tool practice-report-button", "Report"),
                                      { type: "button", title: "How this pass went, and the phrases worth looping" });
    this.reportButton.addEventListener("click", () => practice("report").then((report) => this.showReport(report, true)));

    this.passLabel = html("div", "practice-pass");
    this.passLabel.append(html("span", "practice-pass-value", "–"), html("small", "", "this pass"));

    bar.append(title, this.openButton, this.voiceButton, guide, restart, this.reportButton, this.passLabel);

    // Looping one phrase: what's looping, how slow, which key, and the way back.
    this.loopSpeed = 1;
    this.loopKey = 0;
    this.weak = [];
    this.loopBar = html("div", "practice-loop");
    this.loopBar.hidden = true;
    this.loopLabel = html("span", "practice-loop-label");
    const speedLabel = html("label", "", "Speed ");
    this.speedSelect = select([[1, "100%"], [0.9, "90%"], [0.75, "75%"], [0.6, "60%"], [0.5, "50%"]], 1);
    speedLabel.append(this.speedSelect);
    const keyLabel = html("label", "", "Key ");
    this.keySelect = select([-5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5].map((k) => [k, k === 0 ? "Original" : `${k > 0 ? "+" : "−"}${Math.abs(k)} st`]), 0);
    keyLabel.append(this.keySelect);
    const relooped = () => {
      this.loopSpeed = Number(this.speedSelect.value);
      this.loopKey = Number(this.keySelect.value);
      if (this.song?.loop) this.loopPhrase(this.song.loop.phrase);
    };
    this.speedSelect.addEventListener("change", relooped);
    this.keySelect.addEventListener("change", relooped);
    this.nextWeak = Object.assign(html("button", "tool", "Next weak phrase"), { type: "button" });
    this.nextWeak.addEventListener("click", () => {
      const at = this.weak.indexOf(this.song?.loop?.phrase);
      this.loopPhrase(this.weak[(at + 1) % this.weak.length]);
    });
    const back = Object.assign(html("button", "tool is-accent", "Back to the song"), { type: "button" });
    back.addEventListener("click", () => practice("unloop"));
    this.loopBar.append(this.loopLabel, speedLabel, keyLabel, this.nextWeak, back);

    // Preparing a song.
    this.progress = html("div", "practice-progress");
    this.progress.hidden = true;
    this.progressText = html("span", "practice-progress-text");
    this.progressBar = html("div", "practice-progress-bar");
    this.progressBar.append(html("div"));
    const cancel = Object.assign(html("button", "text-button", "Cancel"), { type: "button" });
    cancel.addEventListener("click", () => practice("cancel"));
    this.progress.append(this.progressText, this.progressBar, cancel);

    // The lane: target notes scrolling past, your pitch on top.
    this.stage = html("div", "practice-stage");
    this.canvas = document.createElement("canvas");
    this.tuner = html("div", "practice-tuner");
    this.empty = html("div", "practice-empty");
    this.empty.append(html("p", "big", "Practise any song"),
                      html("p", "", "Open a song: MicFX takes the voice out, keeps it as a quiet guide and turns its melody into notes to sing. Sing in any octave; each note turns green when you're within a quarter of a semitone."));
    const emptyOpen = Object.assign(html("button", "tool is-accent", "Open song…"), { type: "button" });
    emptyOpen.addEventListener("click", () => practice("open", { split: true }));
    this.empty.append(emptyOpen);
    this.stage.append(this.canvas, this.tuner, this.empty);
    this.stage.addEventListener("pointerdown", (event) => this.onStageClick(event));

    // The phrases, scored as you go; click one to go back to it.
    this.phraseStrip = html("div", "practice-phrases");

    this.message = html("div", "practice-message");
    // The pass report, over the lane.
    this.report = html("div", "practice-report");
    this.report.hidden = true;
    this.stage.append(this.report);
    this.report.addEventListener("pointerdown", (event) => event.stopPropagation());

    s.append(bar, this.loopBar, this.progress, this.stage, this.phraseStrip, this.message);
  }

  show(on, remember = true) {
    this.open = on;
    this.section.hidden = !on;
    this.workspace.hidden = on;
    this.toggle.setAttribute("aria-pressed", String(on));
    document.body.classList.toggle("is-practice", on);
    practice("view", { open: on }).then((state) => this.applyState(state)).catch(() => {});
    if (remember) Juce.getNativeFunction("uiPref")("practiceShown", on ? "1" : "0");
    if (on) this.draw();
  }

  //==============================================================================
  applyState(state) {
    if (!state) return;
    if (typeof state.guideLevel === "number") {
      this.guideSlider.value = String(Math.round(state.guideLevel * 100));
      this.guideValue.textContent = state.guideLevel <= 0 ? "Off" : `${Math.round(state.guideLevel * 100)}%`;
    }
    this.setSong(state.song);
    if (state.job) this.showProgress(state.job);
    if (state.scores) this.onScore(state.scores);
  }

  setSong(song) {
    if (!song || !song.notes) {
      this.song = null;
    } else {
      const notes = [];
      for (let i = 0; i + 3 < song.notes.length; i += 4)
        notes.push({ start: song.notes[i], end: song.notes[i + 1], pitch: song.notes[i + 2], phrase: song.notes[i + 3] });
      const phrases = [];
      for (let i = 0; i + 1 < song.phrases.length; i += 2) phrases.push({ start: song.phrases[i], end: song.phrases[i + 1] });
      this.song = { ...song, notes, phrases };
    }
    this.noteScores.clear();
    this.phraseScores.clear();
    this.trail = [];
    this.pass = 0;

    this.songLabel.textContent = this.song ? this.song.name || "Song" : "No song yet";
    if (!this.song?.loop) this.songInfo.textContent = this.song
      ? `${this.song.notes.length} notes in ${this.song.phrases.length} phrases · ${noteName(this.song.lowest)} to ${noteName(this.song.highest)}`
        + (this.song.hasBacking ? "" : " · voice only")
      : "Open a song and sing its melody";
    this.empty.hidden = Boolean(this.song);
    const loop = this.song?.loop;
    this.loopBar.hidden = !loop;
    this.section.classList.toggle("is-looping", Boolean(loop));
    if (loop) {
      const parts = [`Looping phrase ${loop.phrase + 1}`];
      if (loop.speed < 0.999) parts.push(`${Math.round(loop.speed * 100)}% speed`);
      if (loop.semitones) parts.push(`${loop.semitones > 0 ? "+" : "−"}${Math.abs(loop.semitones)} st`);
      this.loopLabel.textContent = parts.join(" · ");
      this.loopSpeed = loop.speed;
      this.loopKey = loop.semitones;
      this.speedSelect.value = String([1, 0.9, 0.75, 0.6, 0.5].reduce((a, b) => (Math.abs(b - loop.speed) < Math.abs(a - loop.speed) ? b : a)));
      this.keySelect.value = String(loop.semitones);
      this.nextWeak.hidden = this.weak.length < 2;
      this.report.hidden = true;
    }
    this.buildPhraseStrip();
    this.updatePass();
    this.draw();
  }

  showProgress(job) {
    this.progress.hidden = !job;
    if (!job) return;
    this.progressText.textContent = `${job.name ? `${job.name}: ` : ""}${PHASES[job.phase] ?? "Preparing…"}`;
    this.progressBar.firstChild.style.width = `${Math.round(clamp(job.progress) * 100)}%`;
  }

  onPractice(event) {
    if (event.state === "preparing") {
      this.showProgress(event.job);
      this.setMessage("");
      return;
    }
    if (event.state === "rendering") {
      this.setMessage("Preparing the loop…");
      return;
    }
    this.showProgress(null);
    if (event.state === "loop") {
      this.setSong(event.song);
      this.setMessage("Sing the phrase each time it comes round. Its score is the last time round.");
    } else if (event.state === "ready") {
      this.setSong(event.song);
      this.setMessage(event.quiet ? "" : event.fromCache ? "Ready (prepared before). Press play and sing."
                                                         : `Ready. Press play and sing.${event.note ? ` ${event.note}` : ""}`);
    } else if (event.state === "error") {
      this.setMessage(event.error, true);
    } else if (event.state === "cancelled") {
      this.setMessage("Cancelled.");
    }
  }

  setMessage(text, isError = false) {
    this.message.textContent = text ?? "";
    this.message.classList.toggle("is-error", isError);
  }

  onPitch(event) {
    this.clock = { time: event.time, playing: event.playing, at: performance.now() };
    this.midiNow = event.midi;
    const frames = event.frames ?? [];
    // Going back (a seek, "From the top"): what was sung after that point is forgotten.
    if (frames.length && this.trail.length && frames[0] < this.trail[this.trail.length - 1].time - 0.2)
      this.trail = this.trail.filter((f) => f.time < frames[0]);
    for (let i = 0; i + 1 < frames.length; i += 2) this.trail.push({ time: frames[i], midi: frames[i + 1] });
    const oldest = event.time - TRAIL_SECONDS;
    if (this.trail.length && (this.trail[0].time < oldest || this.trail[this.trail.length - 1].time > event.time + 1)) {
      this.trail = this.trail.filter((f) => f.time >= oldest && f.time <= event.time + 1);
    }
  }

  onScore(event) {
    if (!event) return;
    if (event.reset) {
      const from = event.from ?? 0;
      for (const [i] of this.noteScores) if (!this.song || this.song.notes[i]?.end > from) this.noteScores.delete(i);
      for (const [i] of this.phraseScores) if (!this.song || this.song.phrases[i]?.end > from) this.phraseScores.delete(i);
    }
    for (const note of event.notes ?? []) this.noteScores.set(note.i, note);
    for (const phrase of event.phrases ?? []) this.phraseScores.set(phrase.i, phrase);
    this.pass = event.pass ?? 0;
    this.updatePass();
    this.updatePhraseStrip();
  }

  updatePass() {
    const value = this.passLabel.querySelector(".practice-pass-value");
    const any = this.phraseScores.size > 0;
    value.textContent = any ? String(Math.round(this.pass)) : "–";
    this.passLabel.className = `practice-pass ${any ? scoreClass(this.pass) : ""}`;
  }

  buildPhraseStrip() {
    this.phraseStrip.replaceChildren();
    this.phraseChips = [];
    if (!this.song) return;
    this.song.phrases.forEach((phrase, i) => {
      const chip = Object.assign(html("button", "practice-phrase"), { type: "button" });
      chip.style.flexGrow = String(Math.max(0.6, phrase.end - phrase.start));
      chip.title = `Phrase ${i + 1} · ${formatDuration(phrase.start)}. Click to sing it again.`;
      chip.append(html("span", "practice-phrase-number", String(i + 1)), html("span", "practice-phrase-score"));
      chip.addEventListener("click", () => this.goTo(Math.max(0, phrase.start - 1.5)));
      this.phraseStrip.append(chip);
      this.phraseChips.push(chip);
    });
    this.updatePhraseStrip();
  }

  updatePhraseStrip() {
    (this.phraseChips ?? []).forEach((chip, i) => {
      const score = this.phraseScores.get(i);
      chip.className = `practice-phrase ${score?.scored ? scoreClass(score.score) : ""}`;
      chip.lastChild.textContent = score?.scored ? String(Math.round(score.score)) : "";
      if (score?.scored) {
        const lean = Math.abs(score.lean) >= 15 ? ` · ${score.lean > 0 ? "sharp" : "flat"} ${Math.round(Math.abs(score.lean))}¢` : "";
        const timing = Math.abs(score.timing) >= 0.08 ? ` · ${score.timing > 0 ? "late" : "early"} ${Math.round(Math.abs(score.timing) * 1000)} ms` : "";
        chip.title = `Phrase ${i + 1}: ${Math.round(score.score)} · ${score.notesInTune}/${score.notesSung} notes in tune${lean}${timing}. Click to sing it again.`;
      }
    });
  }

  loopPhrase(phrase) {
    if (phrase == null || phrase < 0) return;
    practice("loop", { phrase, speed: this.loopSpeed, semitones: this.loopKey });
  }

  // The pass report: score, its parts, the lean, a heat map over the song and the weak phrases.
  showReport(report, asked = false) {
    if (!report || !this.song) return;
    const r = this.report;
    r.replaceChildren();
    this.weak = report.looping ? this.weak : report.weak ?? [];

    const close = Object.assign(html("button", "mini practice-report-close", "×"), { type: "button", title: "Close" });
    close.addEventListener("click", () => { r.hidden = true; });
    const head = html("div", "practice-report-head");
    head.append(html("strong", "", report.looping ? "This time round" : "This pass"), close);
    r.append(head);

    if (!report.scored) {
      r.append(html("p", "practice-report-note", "Nothing scored yet: play the song and sing along."));
      r.hidden = !asked;
      return;
    }

    const big = html("div", `practice-report-score ${scoreClass(report.score)}`, String(Math.round(report.score)));
    const covered = html("small", "", report.looping ? "" : `${report.scored} of ${report.total} phrases`);
    const scoreRow = html("div", "practice-report-row");
    scoreRow.append(big, covered);
    r.append(scoreRow);

    for (const [label, value, hint] of [["Pitch", report.pitch, "how close to the notes"],
                                        ["Timing", report.timing, "coming in with the music"],
                                        ["Steadiness", report.steady, "holding notes without drifting"]]) {
      const row = html("div", "practice-meter");
      row.title = hint;
      const track = html("div", "practice-meter-track");
      const fill = html("div", `practice-meter-fill ${scoreClass(value * 100)}`);
      fill.style.width = `${Math.round(clamp(value) * 100)}%`;
      track.append(fill);
      row.append(html("span", "", label), track, html("span", "practice-meter-value", String(Math.round(value * 100))));
      r.append(row);
    }

    const tips = [];
    if (Math.abs(report.lean) >= 12) tips.push(`You leaned ${Math.round(Math.abs(report.lean))} cents ${report.lean > 0 ? "sharp" : "flat"} on average.`);
    else tips.push("No lean: you sat in the middle of the notes.");
    if (Math.abs(report.timingSeconds) >= 0.06) tips.push(`You came in ${Math.round(Math.abs(report.timingSeconds) * 1000)} ms ${report.timingSeconds > 0 ? "late" : "early"} on average.`);
    r.append(html("p", "practice-report-note", tips.join(" ")));

    // Heat map: every phrase where it is in the song, coloured by its score.
    if (!report.looping) {
      const heat = html("div", "practice-heat");
      const length = Math.max(1, ...this.song.phrases.map((p) => p.end));
      const byIndex = new Map((report.phrases ?? []).map((p) => [p.i, p]));
      this.song.phrases.forEach((phrase, i) => {
        const score = byIndex.get(i);
        const cell = Object.assign(html("button", `practice-heat-cell ${score ? scoreClass(score.score) : ""}`), { type: "button" });
        cell.style.left = `${(phrase.start / length) * 100}%`;
        cell.style.width = `${Math.max(0.6, ((phrase.end - phrase.start) / length) * 100)}%`;
        cell.title = `Phrase ${i + 1}${score ? `: ${Math.round(score.score)}` : ": not sung"}`;
        cell.addEventListener("click", () => this.goTo(Math.max(0, phrase.start - 1.5)));
        heat.append(cell);
      });
      r.append(heat);
    }

    if (!report.looping && this.weak.length) {
      r.append(html("div", "practice-report-sub", "Worth another go"));
      for (const i of this.weak) {
        const p = byPhrase(report, i);
        const row = html("div", "practice-weak");
        const what = [];
        if (p && Math.abs(p.lean) >= 15) what.push(`${p.lean > 0 ? "sharp" : "flat"} ${Math.round(Math.abs(p.lean))}¢`);
        if (p && Math.abs(p.timing) >= 0.08) what.push(`${p.timing > 0 ? "late" : "early"} ${Math.round(Math.abs(p.timing) * 1000)} ms`);
        if (p && p.coverage < 0.6) what.push("parts not sung");
        const loopButton = Object.assign(html("button", "tool is-accent", "Repeat phrase"), { type: "button" });
        loopButton.addEventListener("click", () => this.loopPhrase(i));
        row.append(html("span", `practice-weak-score ${scoreClass(p?.score ?? 0)}`, String(Math.round(p?.score ?? 0))),
                   html("span", "practice-weak-name", `Phrase ${i + 1}${what.length ? ` · ${what.join(", ")}` : ""}`), loopButton);
        r.append(row);
      }
      const options = html("div", "practice-report-options");
      const speed = html("label", "", "Repeat at ");
      const speedCopy = select([[1, "100%"], [0.9, "90%"], [0.75, "75%"], [0.6, "60%"], [0.5, "50%"]], this.loopSpeed);
      speedCopy.addEventListener("change", () => { this.loopSpeed = Number(speedCopy.value); this.speedSelect.value = speedCopy.value; });
      speed.append(speedCopy);
      const key = html("label", "", "in ");
      const keyCopy = select([-5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5].map((k) => [k, k === 0 ? "the original key" : `${k > 0 ? "+" : "−"}${Math.abs(k)} st`]), this.loopKey);
      keyCopy.addEventListener("change", () => { this.loopKey = Number(keyCopy.value); this.keySelect.value = keyCopy.value; });
      key.append(keyCopy);
      options.append(speed, key);
      r.append(options);
    } else if (!report.looping) {
      r.append(html("p", "practice-report-note", "Every phrase scored 75 or more. Try it in another key, or record a take."));
    }

    r.hidden = false;
  }

  goTo(seconds) {
    transport("seek", seconds);
    if (!this.clock.playing) transport("play");
  }

  onStageClick(event) {
    if (!this.song) return;
    const box = this.stage.getBoundingClientRect();
    const t = this.now() + ((event.clientX - box.left) / box.width - PLAYHEAD) * SECONDS_SHOWN;
    const phrase = this.song.phrases.findIndex((p) => t >= p.start - 0.3 && t <= p.end + 0.3);
    if (phrase >= 0) this.goTo(Math.max(0, this.song.phrases[phrase].start - 1.5));
  }

  // The song time now, run on between the engine's 30 Hz updates so the lane scrolls smoothly.
  now() {
    const { time, playing, at } = this.clock;
    return playing ? time + Math.min(0.1, (performance.now() - at) / 1000) : time;
  }

  // The target sung at `time` (or the nearest one within half a second), for folding the trail.
  targetAt(time) {
    if (!this.song) return 0;
    let best = 0, distance = 0.5;
    for (const note of this.song.notes) {
      if (note.start - 0.5 > time) break;
      const d = time < note.start ? note.start - time : time > note.end ? time - note.end : 0;
      if (d <= distance) { best = note.pitch; distance = d; if (d === 0) break; }
    }
    return best;
  }

  //==============================================================================
  draw() {
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.stage.clientWidth * ratio));
    const height = Math.max(1, Math.round(this.stage.clientHeight * ratio));
    if (this.canvas.width !== width) this.canvas.width = width;
    if (this.canvas.height !== height) this.canvas.height = height;
    const g = this.canvas.getContext("2d");
    g.clearRect(0, 0, width, height);
    if (!this.song) { this.tuner.hidden = true; return; }

    const css = getComputedStyle(this.section);
    const color = (name, fallback) => css.getPropertyValue(name).trim() || fallback;
    const good = color("--good", "#3fd9c6"), close = color("--close", "#ffc94d"), off = color("--off", "#ff7a90");
    const target = color("--target", "#8c7bff"), line = color("--line", "#242a37"), faint = color("--faint", "#566077");

    // Pitch range: the song's notes with room around them.
    const middle = (this.song.lowest + this.song.highest) / 2;
    const span = Math.max(14, this.song.highest - this.song.lowest + 6);
    const top = middle + span / 2;
    const rowHeight = height / span;
    const y = (midi) => (top - midi) * rowHeight;
    const now = this.now();
    const pxPerSecond = width / SECONDS_SHOWN;
    const x = (t) => (PLAYHEAD + (t - now) / SECONDS_SHOWN) * width;
    const from = now - PLAYHEAD * SECONDS_SHOWN, to = from + SECONDS_SHOWN;

    // Semitone rows, C rows stronger, with their names.
    g.font = `${10 * ratio}px ui-monospace, Menlo, monospace`;
    g.textBaseline = "middle";
    for (let m = Math.ceil(top - span); m <= Math.floor(top); ++m) {
      const isC = ((m % 12) + 12) % 12 === 0;
      const black = [1, 3, 6, 8, 10].includes(((m % 12) + 12) % 12);
      if (black) {
        g.fillStyle = "rgba(255,255,255,0.025)";
        g.fillRect(0, y(m) - rowHeight / 2, width, rowHeight);
      }
      if (isC) {
        g.fillStyle = line;
        g.fillRect(0, y(m) + rowHeight / 2, width, ratio);
        g.fillStyle = faint;
        g.fillText(noteName(m), 6 * ratio, y(m));
      }
    }

    // Phrase spans behind the notes.
    g.fillStyle = "rgba(140,123,255,0.05)";
    for (const p of this.song.phrases) if (p.end >= from && p.start <= to) g.fillRect(x(p.start), 0, (p.end - p.start) * pxPerSecond, height);

    // Target notes: coloured by their score once sung.
    const barHeight = Math.max(4 * ratio, rowHeight * 0.72);
    this.song.notes.forEach((note, i) => {
      if (note.end < from || note.start > to) return;
      const score = this.noteScores.get(i);
      const current = now >= note.start && now < note.end;
      g.globalAlpha = score ? 0.9 : current ? 0.95 : 0.55;
      g.fillStyle = score ? (score.sung ? (score.score >= 80 ? good : score.score >= 55 ? close : off) : faint) : target;
      const left = x(note.start), w = Math.max(2 * ratio, (note.end - note.start) * pxPerSecond - ratio);
      roundRect(g, left, y(note.pitch) - barHeight / 2, w, barHeight, Math.min(barHeight / 2, 5 * ratio));
      if (current) {
        g.globalAlpha = 1;
        g.strokeStyle = "rgba(255,255,255,0.85)";
        g.lineWidth = 1.5 * ratio;
        g.stroke();
      }
    });
    g.globalAlpha = 1;

    // Your pitch, folded to the octave of the note it was aiming for.
    g.lineWidth = 3 * ratio;
    g.lineCap = "round";
    let previous = null;
    for (const f of this.trail) {
      if (f.time < from || f.time > now + 0.05 || f.midi <= 0) { previous = null; continue; }
      const aim = this.targetAt(f.time);
      const midi = aim ? foldToOctave(f.midi, aim) : foldToOctave(f.midi, middle);
      const cents = aim ? Math.abs(midi - aim) * 100 : 999;
      const point = { x: x(f.time), y: y(midi), color: cents <= IN_TUNE ? good : cents <= CLOSE ? close : "rgba(233,236,244,0.8)" };
      if (previous && point.x - previous.x < 6 * ratio && Math.abs(point.y - previous.y) < 3 * rowHeight) {
        g.strokeStyle = point.color;
        g.beginPath();
        g.moveTo(previous.x, previous.y);
        g.lineTo(point.x, point.y);
        g.stroke();
      }
      previous = point;
    }

    // The playhead.
    g.fillStyle = "rgba(255,255,255,0.35)";
    g.fillRect(Math.round(PLAYHEAD * width), 0, ratio, height);

    // The tuner by the playhead: your note now and how far off the target it is.
    const aim = this.targetAt(now);
    if (this.midiNow > 0) {
      const midi = aim ? foldToOctave(this.midiNow, aim) : this.midiNow;
      const cents = aim ? Math.round((midi - aim) * 100) : 0;
      this.tuner.hidden = false;
      this.tuner.style.top = `${clamp(y(aim ? midi : foldToOctave(midi, middle)) / ratio, 10, this.stage.clientHeight - 10)}px`;
      this.tuner.style.left = `${PLAYHEAD * this.stage.clientWidth}px`;
      this.tuner.textContent = aim ? `${noteName(this.midiNow)} ${cents > 0 ? "+" : ""}${cents}¢` : noteName(this.midiNow);
      this.tuner.className = `practice-tuner ${aim ? (Math.abs(cents) <= IN_TUNE ? "is-good" : Math.abs(cents) <= CLOSE ? "is-close" : "is-off") : ""}`;
    } else {
      this.tuner.hidden = true;
    }
  }
}

function select(options, value) {
  const element = Object.assign(document.createElement("select"), { className: "dropdown" });
  for (const [v, label] of options) element.append(Object.assign(document.createElement("option"), { value: String(v), textContent: label }));
  element.value = String(value);
  return element;
}

const byPhrase = (report, i) => (report.phrases ?? []).find((p) => p.i === i);

function roundRect(g, x, y, w, h, r) {
  g.beginPath();
  g.moveTo(x + r, y);
  g.arcTo(x + w, y, x + w, y + h, r);
  g.arcTo(x + w, y + h, x, y + h, r);
  g.arcTo(x, y + h, x, y, r);
  g.arcTo(x, y, x + w, y, r);
  g.closePath();
  g.fill();
}
