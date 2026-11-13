// Mix Check (DAW): a report on the mix in plain words, with a Fix button for what Auto Mix's
// moves can cure. The bridge command "mixCheck" measures the tracks and the mix offline and
// answers with "dawMixCheck" events (see handleMixCheck in src/DawBridge.cpp,
// src/analysis/MixCheck.h). This view owns the panel; the DAW screen gives it `host`:
//   daw(command, args)   the bridge call
//   select(trackId)      select a track (0: the master)
//   seek(seconds)
//   setJob(id, job) / endJob(id)   the activity list's running jobs
import { html } from "./widgets.js";

const SEVERITY = ["Note", "Worth fixing", "Problem"];

// Which stat each finding colours.
const STAT_KEYS = { lufs: ["loudness"], truePeak: ["truePeak"], range: ["overCompressed"], plr: ["overCompressed"] };

export class MixCheckView {
  constructor(panel, host) {
    this.panel = panel;
    this.host = host;
    this.report = null;
    this.cards = new Map(); // finding index -> { card, actions, fix }
    const $ = (selector) => panel.querySelector(selector);
    this.body = $(".mixcheck-findings");
    this.score = $(".mixcheck-score");

    $(".mixcheck-close").addEventListener("click", () => this.toggle(false));
    $(".mixcheck-cancel").addEventListener("click", () => host.daw("mixCheck", { action: "cancel" }));
    $(".mixcheck-run").addEventListener("click", () => this.run());
    $(".mixcheck-copy").addEventListener("click", async () => {
      const result = await host.daw("mixCheck", { action: "copy" });
      if (result?.ok) this.setResult("Report copied.");
    });
    $(".mixcheck-save").addEventListener("click", async () => {
      const result = await host.daw("mixCheck", { action: "save" });
      if (result?.ok) this.setResult("Report saved.");
    });
    window.__JUCE__.backend.addEventListener("dawMixCheck", (event) => this.onEvent(event));
  }

  toggle(show = this.panel.hidden) {
    this.panel.hidden = !show;
    if (show) this.host.closeOthers?.();
  }

  setResult(text, isError = false) {
    this.panel.querySelector(".mixcheck-result").replaceChildren(text ? html("span", isError ? "is-error" : "", text) : "");
  }

  async run() {
    const style = this.panel.querySelector("[name=style]").value;
    const target = Number(this.panel.querySelector("[name=target]").value);
    this.setResult("Starting…");
    const result = await this.host.daw("mixCheck", { action: "run", style, target });
    if (result?.ok === false) this.setResult(result.error, true);
  }

  onEvent(event) {
    const panel = this.panel;
    const running = event.state === "analysing";
    panel.classList.toggle("is-running", running);
    if (event.progress != null) panel.querySelector(".export-bar").style.setProperty("--progress", event.progress);
    if (running) this.host.setJob("mixcheck", { label: "Mix Check", progress: event.progress ?? null, cancel: () => this.host.daw("mixCheck", { action: "cancel" }), open: () => this.toggle(true) });
    else this.host.endJob("mixcheck");

    if (event.state === "analysing") return this.setResult("Listening to every track and the mix…");
    if (event.state === "cancelled") return this.setResult("Cancelled.");
    if (event.state === "error") return this.setResult(event.error, true);
    if (event.state === "fixed") return; // the card shows it from the fix call's answer
    if (event.state !== "ready") return;

    panel.hidden = false;
    this.report = event;
    this.cards.clear();
    this.setResult("");
    if (event.style) panel.querySelector("[name=style]").value = event.style;
    if (event.target != null) panel.querySelector("[name=target]").value = String(event.target);
    this.render();
  }

  statClass(statKey) {
    const keys = STAT_KEYS[statKey] ?? [];
    const worst = Math.max(-1, ...this.report.findings.filter((f) => keys.includes(f.key)).map((f) => f.severity));
    return worst < 0 ? "is-ok" : worst === 2 ? "is-bad" : "is-warn";
  }

  render() {
    const report = this.report;
    const panel = this.panel;
    const hasReport = Boolean(report);
    panel.querySelector(".mixcheck-run").textContent = hasReport ? "Check again" : "Check the mix";
    panel.querySelector(".mixcheck-copy").hidden = !hasReport;
    panel.querySelector(".mixcheck-save").hidden = !hasReport;
    this.score.hidden = !hasReport;
    this.body.replaceChildren();
    if (!hasReport) return;

    const score = report.score;
    this.score.style.setProperty("--score", score);
    this.score.dataset.level = score >= 85 ? "good" : score >= 60 ? "fair" : "poor";
    this.score.querySelector(".mc-score-value").textContent = String(score);
    this.score.querySelector(".mc-score-label").textContent = report.empty ? "Nothing to check"
      : score >= 85 ? "Ready to share" : score >= 60 ? "Almost there" : "Needs work";
    const stats = [
      ["lufs", "Loudness", report.empty ? "–" : `${report.lufs.toFixed(1)} LUFS`],
      ["truePeak", "True peak", report.empty ? "–" : `${report.truePeak.toFixed(1)} dBTP`],
      ["range", "Range", report.empty ? "–" : `${report.range.toFixed(1)} LU`],
      ["plr", "Punch", report.empty ? "–" : `${report.plr.toFixed(1)} dB`],
    ];
    const grid = this.score.querySelector(".mc-stats");
    grid.replaceChildren();
    for (const [key, label, value] of stats) {
      const cell = html("div", `mc-stat ${report.empty ? "" : this.statClass(key)}`);
      cell.append(html("small", "", label), html("strong", "", value));
      grid.append(cell);
    }

    if (!report.findings.length) {
      this.body.append(html("div", "mc-clear", "Nothing to fix: the mix passes every check."));
      return;
    }

    for (const finding of report.findings) this.body.append(this.card(finding));
    const counts = [2, 1, 0].map((s) => report.findings.filter((f) => f.severity === s).length);
    this.body.prepend(html("div", "mc-summary",
      [counts[0] && `${counts[0]} problem${counts[0] === 1 ? "" : "s"}`, counts[1] && `${counts[1]} to fix`, counts[2] && `${counts[2]} note${counts[2] === 1 ? "" : "s"}`]
        .filter(Boolean).join(" · ")));
  }

  card(finding) {
    const card = html("section", `mc-finding is-sev${finding.severity}`);
    card.dataset.key = finding.key;
    const head = html("header");
    const dot = html("span", "mc-dot");
    dot.title = SEVERITY[finding.severity];
    head.append(dot, html("strong", "", finding.title));
    if (finding.to > finding.from) head.append(html("small", "mc-when", `${formatTime(finding.from)}–${formatTime(finding.to)}`));
    else if (finding.from > 0) head.append(html("small", "mc-when", formatTime(finding.from)));
    card.append(head, html("p", "mc-detail", finding.detail), html("p", "mc-why", finding.why));

    const actions = html("div", "mc-actions");
    const show = html("button", "tool mc-show", finding.track ? "Show" : "Show master");
    show.type = "button";
    show.title = finding.to > finding.from ? "Select the track and loop this stretch" : "Select the track";
    show.addEventListener("click", () => this.show(finding));
    actions.append(show);

    if (finding.fix !== "none") {
      const fix = html("button", "tool is-accent mc-fix", finding.fixLabel || "Fix");
      fix.type = "button";
      fix.title = "Apply the fix: one undo step";
      fix.addEventListener("click", async () => {
        fix.disabled = true;
        const result = await this.host.daw("mixCheck", { action: "fix", index: finding.index });
        if (!result?.ok) { fix.disabled = false; return; }
        // Fixes that share this plan (loudness and true peak) are done too; one Undo takes them all back.
        const group = (result.fixed ?? []).filter((index) => index !== finding.index && this.cards.has(index));
        this.showFixed(card, actions, fix, result.reasons ?? [], "✓ Fixed", group);
        for (const index of group) {
          const other = this.cards.get(index);
          if (!other.card.classList.contains("is-fixed")) this.showFixed(other.card, other.actions, other.fix, [], "✓ Done with the fix above", null);
        }
      });
      actions.append(fix);
    }
    card.append(actions);
    this.cards.set(finding.index, { card, actions, fix: actions.querySelector(".mc-fix") });
    if (finding.fixed) this.showFixed(card, actions, actions.querySelector(".mc-fix"), []);
    return card;
  }

  // `group`: the other findings the same fix settled (null: this one is one of those, no Undo of its own).
  showFixed(card, actions, fix, reasons, label = "✓ Fixed", group = []) {
    card.classList.add("is-fixed");
    if (fix) fix.hidden = true;
    const done = html("div", "mc-done");
    done.append(html("span", "mc-tick", label));
    const list = html("ul");
    for (const reason of reasons) list.append(html("li", "", reason));
    if (group) {
      const undo = html("button", "text-button", "Undo");
      undo.type = "button";
      undo.addEventListener("click", async () => {
        await this.host.daw("undo");
        for (const entry of [{ card, fix, done }, ...group.map((index) => this.cards.get(index))]) {
          entry.card.classList.remove("is-fixed");
          (entry.done ?? entry.card.querySelector(".mc-done"))?.remove();
          if (entry.fix) { entry.fix.hidden = false; entry.fix.disabled = false; }
        }
      });
      done.append(undo);
    }
    if (reasons.length) done.append(list);
    actions.after(done);
    this.setResult("Fixed. Check again to hear the difference in the numbers.");
  }

  show(finding) {
    this.host.select(finding.track || 0);
    if (finding.to > finding.from) {
      this.host.daw("loop", { on: true, start: finding.from, end: finding.to });
      this.host.seek(finding.from);
    } else if (finding.from > 0) this.host.seek(Math.max(0, finding.from - 1));
  }
}

function formatTime(seconds) {
  const total = Math.floor(seconds);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}
