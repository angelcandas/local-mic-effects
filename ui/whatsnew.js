// What's new and the changelog (data: ui/changelog.js, generated from CHANGELOG.md).
//
//   After an update the first launch opens "What's new" with the entries you haven't seen yet
//   (a new install only records the version, the welcome card covers it). Help → What's new
//   shows the same, Help → Changelog the whole list. "Seen" is the highest PR number the
//   window was last closed on, kept in the app settings.

import * as Juce from "./juce/index.js";
import { html } from "./widgets.js";
import { modal } from "./setup.js";
import { CHANGELOG } from "./changelog.js";

const uiPref = Juce.getNativeFunction("uiPref");
const KEY = "changelogSeen";
const newest = (entry) => Math.max(0, ...entry.prs);

const dateText = (iso) => new Date(`${iso}T12:00:00`).toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" });

// seen = null shows everything; otherwise only the entries newer than that.
function render(body, seen) {
  body.replaceChildren();
  let shown = 0;
  for (const day of CHANGELOG.days) {
    const entries = seen === null ? day.entries : day.entries.filter((e) => newest(e) > seen);
    if (!entries.length) continue;
    const section = html("section", "setup-section whatsnew-day");
    section.append(html("h3", "", dateText(day.date)));
    const list = html("ul", "whatsnew-list");
    for (const entry of entries) {
      const item = html("li");
      if (entry.title) item.append(html("strong", "", entry.title + "."), " ");
      item.append(entry.text);
      list.append(item);
      shown += 1;
    }
    section.append(list);
    body.append(section);
  }
  return shown;
}

export function initWhatsNew() {
  let mode = "new";
  const { backdrop, body } = modal("whatsnew-window", "What's new", () => close());
  const title = backdrop.querySelector("h2");
  const foot = html("div", "setup-foot");
  const switcher = html("button", "tool");
  switcher.type = "button";
  const done = html("button", "tool", "Got it");
  done.type = "button";
  foot.append(switcher, done);
  backdrop.querySelector(".setup-panel").append(foot);

  const close = () => {
    backdrop.hidden = true;
    uiPref(KEY, String(CHANGELOG.latest)).catch(() => {});
  };
  done.addEventListener("click", close);
  switcher.addEventListener("click", () => open(mode === "new" ? "all" : "new"));

  // what = "new" (since you last looked), "all" (the whole changelog). since: what "new" is relative to.
  let since = 0;
  function open(what, from = since) {
    since = from;
    mode = what;
    const shown = render(body, what === "all" ? null : since);
    if (what === "new" && !shown) { mode = "all"; render(body, null); }
    title.textContent = mode === "all" ? "Changelog" : "What's new";
    switcher.textContent = mode === "all" ? "Show only what's new" : "Full changelog";
    switcher.hidden = mode === "all" && !shown && what === "new";
    backdrop.hidden = false;
    body.scrollTop = 0;
  }

  document.addEventListener("micfx:whatsNew", () => uiPref(KEY).then((v) => open("new", Number(v) || 0)).catch(() => open("new", 0)));
  document.addEventListener("micfx:changelog", () => open("all"));

  // First launch after an update.
  Promise.all([uiPref(KEY), uiPref("firstRunDone")]).then(([seen, firstRun]) => {
    const last = seen === "" || seen == null ? null : Number(seen);
    if (last !== null && last >= CHANGELOG.latest) return;
    if (last === null && !firstRun) { uiPref(KEY, String(CHANGELOG.latest)).catch(() => {}); return; } // brand new install
    // Never seen the changelog before: show the newest day; afterwards, everything since.
    setTimeout(() => open("new", last ?? Math.min(...CHANGELOG.days[0].entries.map(newest)) - 1), 900);
  }).catch(() => {});
}
