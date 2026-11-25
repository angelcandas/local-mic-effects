// The clip launcher in the DAW screen: the Launch tab. A grid with a column per track and a row
// per scene; each cell can hold a clip (a copy of a song clip, or a new MIDI clip) that plays as
// a loop apart from the song. Clicking a clip launches it on the next quantize line, a scene's
// button launches its whole row, a track's square stops it, and "Back to song" hands every
// track back to its timeline. The model and the playback are in src/daw/Launcher.cpp and
// DawEngine; every gesture is one command, and the page redraws from the session that comes back.
import { html } from "./widgets.js";

const QUANTIZE = [["none", "None"], ["beat", "1 beat"], ["bar", "1 bar"], ["2bars", "2 bars"], ["4bars", "4 bars"]];
const NOT_QUEUED = -1, STOP = -2, SONG = -3;

// The loop lengths offered for a clip, in beats, from the time signature.
const lengthChoices = (beatsPerBar) => [
  [1, "1 beat"], [2, "2 beats"], [beatsPerBar, "1 bar"], [beatsPerBar * 2, "2 bars"],
  [beatsPerBar * 4, "4 bars"], [beatsPerBar * 8, "8 bars"], [beatsPerBar * 16, "16 bars"],
];

const lengthText = (beats, beatsPerBar) => {
  const bars = beats / beatsPerBar;
  if (Math.abs(bars - Math.round(bars)) < 1e-6 && bars >= 1) return `${Math.round(bars)} bar${Math.round(bars) === 1 ? "" : "s"}`;
  return `${+beats.toFixed(2)} beat${beats === 1 ? "" : "s"}`;
};

// screen: the DAW screen (session, status, beatsPerBar, setDockTab, setDock, select...).
// helpers: popMenu, toast, trackColour (track, index). Returns { element, update(session),
// setStatus(status), firstFreeRow(track) }.
export function createLauncherPanel(screen, daw, { popMenu, toast = () => {}, trackColour }) {
  const element = html("section", "launcher-panel");
  let signature = "";
  let cells = new Map();     // "track:row" -> { cell, slot, bar }
  let stops = new Map();     // track id -> its stop button
  let heads = new Map();     // track id -> its column head
  let sceneButtons = [];     // per row
  let dragging = null;

  //------------------------------------------------------------------ the bar
  const bar = html("div", "launch-bar");
  const quantize = html("select", "dropdown launch-quantize");
  quantize.title = "Launch quantize: launches, stops and Back to song wait for the next line of this length on the song's grid";
  for (const [value, label] of QUANTIZE) {
    const option = html("option", "", label);
    option.value = value;
    quantize.append(option);
  }
  quantize.addEventListener("change", () => daw("setLaunchQuantize", { value: quantize.value }));
  const quantizeLabel = html("label", "launch-label", "Quantize");
  quantizeLabel.append(quantize);

  const stopAll = html("button", "pat-tool launch-stop-all", "■ Stop all");
  stopAll.type = "button";
  stopAll.title = "Stop every track's clip (on the next quantize line). The tracks stay silent until Back to song";
  stopAll.addEventListener("click", () => daw("stopAllSlots"));

  const backToSong = html("button", "pat-tool launch-back", "Back to song");
  backToSong.type = "button";
  backToSong.title = "Every track plays its timeline again (on the next quantize line)";
  backToSong.addEventListener("click", () => daw("backToSong"));

  // Record to song: what the launcher plays from now on is written onto the tracks' timelines
  // (when it's switched off, or the transport stops).
  const record = html("button", "pat-tool launch-record", "● Record to song");
  record.type = "button";
  record.title = "Record a performance: launch clips and scenes while it plays, and they're written onto the song's tracks (a clip per loop pass) when you switch it off or stop";
  record.addEventListener("click", async () => {
    const on = !record.classList.contains("is-on");
    const result = await daw("launchRecord", { on });
    if (!result?.ok) return;
    if (on) {
      record.classList.add("is-on");
      toast(screen.status?.playing ? "Recording what the launcher plays into the song." : "Recording: press Play or launch a clip.");
    } else {
      record.classList.remove("is-on");
      toast(result.clips ? `Written into the song: ${result.clips} clip${result.clips === 1 ? "" : "s"}.` : "Nothing was played to write.", "", result.clips ? "undo" : "");
    }
  });

  const addScene = html("button", "pat-tool", "+ Scene");
  addScene.type = "button";
  addScene.title = "Add an empty scene (row) at the bottom";
  addScene.addEventListener("click", () => daw("insertScene", { row: screen.session?.sceneCount ?? 8 }));

  const hint = html("span", "launch-hint", "");
  bar.append(quantizeLabel, stopAll, backToSong, record, html("span", "pat-spacer"), hint, addScene);

  const grid = html("div", "launch-grid");
  grid.setAttribute("role", "grid");
  grid.setAttribute("aria-label", "Clip launcher");
  element.append(bar, grid);

  //------------------------------------------------------------------ helpers
  const tracks = () => screen.session?.tracks ?? [];
  const slotAt = (track, row) => track.slots?.find((s) => s.row === row) ?? null;
  const beatsPerBar = () => screen.session?.beatsPerBar ?? 4;

  // The first empty row on a track (a new row at the bottom when every row is taken).
  const firstFreeRow = (track) => {
    const count = screen.session?.sceneCount ?? 8;
    for (let row = 0; row < count; ++row) if (!slotAt(track, row)) return row;
    return count;
  };

  const rename = (target, current, commit) => {
    const input = html("input", "launch-rename");
    input.value = current;
    input.maxLength = 60;
    target.replaceChildren(input);
    input.focus();
    input.select();
    let done = false;
    const finish = (save) => {
      if (done) return;
      done = true;
      if (save && input.value.trim() !== current) commit(input.value.trim());
      else render(true);
    };
    input.addEventListener("keydown", (e) => {
      e.stopPropagation();
      if (e.key === "Enter") finish(true);
      if (e.key === "Escape") finish(false);
    });
    input.addEventListener("blur", () => finish(true));
    input.addEventListener("pointerdown", (e) => e.stopPropagation());
  };

  const editNotes = (track, slot) => {
    screen.selectedTrack = track.id;
    screen.selectedClip = slot.clip.id;
    screen.setDockTab("track");
    screen.setDock(true);
  };

  const newMidiClip = async (track, row) => {
    const id = await daw("addMidiSlot", { track: track.id, row });
    if (typeof id === "number") toast("A new 1 bar MIDI clip. Right-click it to edit its notes.");
  };

  const slotMenu = (track, slot, x, y) => {
    popMenu(x, y, ({ item, rule, heading }) => {
      item("Launch", () => daw("launchSlot", { track: track.id, slot: slot.id }));
      if (slot.clip.midi) item("Edit notes", () => editNotes(track, slot), { hint: "Open its notes in the piano roll (the Clip tab)" });
      item("Rename", () => {
        const name = cells.get(`${track.id}:${slot.row}`)?.cell.querySelector(".launch-name");
        if (name) rename(name, slot.clip.name, (value) => daw("setSlot", { slot: slot.id, name: value }));
      });
      item("Loop", () => daw("setSlot", { slot: slot.id, loop: !slot.loop }), { checked: slot.loop, hint: "Off: it plays once, then the track is silent" });
      heading("Length");
      for (const [beats, label] of lengthChoices(beatsPerBar()))
        item(label, () => daw("setSlot", { slot: slot.id, beats }), { checked: Math.abs(slot.beats - beats) < 1e-6, indent: true });
      rule();
      item("Duplicate", () => daw("moveSlot", { slot: slot.id, track: track.id, row: firstFreeRow(track), copy: true }),
           { hint: "A copy in the first empty cell of this track" });
      item("Write to the song at the playhead", () => writeToSong(track, slot),
           { hint: "Put one pass of it on the track's timeline where the playhead is" });
      item("Delete", () => daw("removeSlot", { slot: slot.id }), { danger: true });
    });
  };

  const writeToSong = async (track, slot) => {
    const result = await daw("writeSlot", { slot: slot.id, start: screen.status?.position ?? 0, passes: 1 });
    if (result?.ok) toast(`Written to ${track.name} at the playhead.`, "", "undo");
  };

  const emptyMenu = (track, row, x, y) => {
    popMenu(x, y, ({ item }) => {
      if (track.kind === "midi") item("New MIDI clip", () => newMidiClip(track, row), { hint: "An empty 1 bar loop to fill in the piano roll" });
      else item("Copy a clip here: right-click it in the song", () => {}, { disabled: true });
      item("Stop this track", () => daw("stopSlot", { track: track.id }));
    });
  };

  const sceneMenu = (row, x, y) => {
    const name = screen.session?.scenes?.[row] ?? "";
    popMenu(x, y, ({ item, rule }) => {
      item("Launch scene", () => daw("launchScene", { row }));
      item("Rename", () => {
        const label = sceneButtons[row]?.querySelector(".launch-scene-name");
        if (label) rename(label, name, (value) => daw("setSceneName", { row, name: value }));
      });
      rule();
      item("Insert scene above", () => daw("insertScene", { row }));
      item("Duplicate scene", () => daw("duplicateScene", { row }), { hint: "A copy of its clips in a new row below" });
      item("Delete scene", () => daw("removeScene", { row }), { danger: true, disabled: (screen.session?.sceneCount ?? 1) <= 1, hint: "With its clips" });
    });
  };

  // Drag a clip to another cell (Alt or Ctrl: a copy).
  const startDrag = (event, track, slot, cell) => {
    if (event.button !== 0) return;
    const x0 = event.clientX, y0 = event.clientY;
    let moved = false;
    const onMove = (e) => {
      if (!moved && Math.hypot(e.clientX - x0, e.clientY - y0) < 4) return;
      moved = true;
      dragging = { slot, from: track };
      cell.classList.add("is-dragged");
      grid.querySelectorAll(".is-drop").forEach((c) => c.classList.remove("is-drop"));
      document.elementFromPoint(e.clientX, e.clientY)?.closest(".launch-cell")?.classList.add("is-drop");
    };
    const onUp = (e) => {
      removeEventListener("pointermove", onMove);
      removeEventListener("pointerup", onUp);
      cell.classList.remove("is-dragged");
      grid.querySelectorAll(".is-drop").forEach((c) => c.classList.remove("is-drop"));
      dragging = null;
      if (!moved) return daw("launchSlot", { track: track.id, slot: slot.id });
      const target = document.elementFromPoint(e.clientX, e.clientY)?.closest(".launch-cell");
      if (!target) return;
      const toTrack = Number(target.dataset.track), toRow = Number(target.dataset.row);
      if (toTrack === track.id && toRow === slot.row) return;
      daw("moveSlot", { slot: slot.id, track: toTrack, row: toRow, copy: e.altKey || e.ctrlKey || e.metaKey });
    };
    addEventListener("pointermove", onMove);
    addEventListener("pointerup", onUp);
  };

  //------------------------------------------------------------------ drawing
  const render = (force = false) => {
    const session = screen.session;
    if (!session) return;
    const list = tracks();
    const count = session.sceneCount ?? 8;
    const key = JSON.stringify([count, session.scenes, beatsPerBar(), list.map((t, i) => [t.id, t.name, trackColour(t, i), t.kind,
      (t.slots ?? []).map((s) => [s.id, s.row, s.beats, s.loop, s.clip.name, s.clip.midi, s.clip.notes?.length ?? 0])])]);
    if (quantize.value !== session.launchQuantize) quantize.value = session.launchQuantize ?? "bar";
    if (!force && key === signature) return;
    signature = key;

    cells = new Map();
    stops = new Map();
    heads = new Map();
    sceneButtons = [];
    grid.replaceChildren();
    grid.style.setProperty("--launch-tracks", String(list.length));

    if (!list.length) {
      grid.append(html("div", "launch-empty", "Add a track to start: each track is a column of clips here."));
      return;
    }

    // Heads: the scenes' corner, then a column per track.
    const corner = html("div", "launch-corner", "Scenes");
    grid.append(corner);
    list.forEach((track, index) => {
      const head = html("div", "launch-head");
      head.style.setProperty("--track", trackColour(track, index));
      head.append(html("span", "launch-head-name", track.name));
      head.title = `${track.name}${track.kind === "midi" ? " (MIDI)" : ""}`;
      heads.set(track.id, head);
      grid.append(head);
    });

    for (let row = 0; row < count; ++row) {
      const scene = html("button", "launch-scene");
      scene.type = "button";
      scene.title = "Launch this scene: every track plays its clip in this row (an empty cell stops its track). Right-click for more";
      scene.append(html("span", "launch-play", "▶"), html("span", "launch-scene-name", session.scenes?.[row] || `Scene ${row + 1}`));
      scene.addEventListener("click", () => daw("launchScene", { row }));
      scene.addEventListener("dblclick", (e) => {
        e.preventDefault();
        rename(scene.querySelector(".launch-scene-name"), session.scenes?.[row] ?? "", (value) => daw("setSceneName", { row, name: value }));
      });
      scene.addEventListener("contextmenu", (e) => { e.preventDefault(); sceneMenu(row, e.clientX, e.clientY); });
      sceneButtons.push(scene);
      grid.append(scene);

      list.forEach((track, index) => {
        const slot = slotAt(track, row);
        const cell = html("div", `launch-cell${slot ? " has-clip" : ""}${track.kind === "midi" ? " is-midi" : ""}`);
        cell.dataset.track = String(track.id);
        cell.dataset.row = String(row);
        cell.style.setProperty("--track", trackColour(track, index));
        cell.setAttribute("role", "gridcell");
        let progress = null;
        if (slot) {
          const play = html("span", "launch-play", "▶");
          const name = html("span", "launch-name", slot.clip.name || track.name);
          const info = html("span", "launch-info", `${lengthText(slot.beats, beatsPerBar())}${slot.loop ? "" : " · once"}`);
          progress = html("span", "launch-progress");
          cell.append(play, name, info, progress);
          cell.tabIndex = 0;
          cell.title = `${slot.clip.name}: click to launch, drag to move (Alt: copy), right-click for more`;
          cell.addEventListener("pointerdown", (e) => startDrag(e, track, slot, cell));
          cell.addEventListener("keydown", (e) => {
            if (e.key === "Enter") { e.preventDefault(); daw("launchSlot", { track: track.id, slot: slot.id }); }
            if (e.key === "Delete" || e.key === "Backspace") { e.preventDefault(); e.stopPropagation(); daw("removeSlot", { slot: slot.id }); }
          });
          cell.addEventListener("dblclick", () => { if (slot.clip.midi) editNotes(track, slot); });
          cell.addEventListener("contextmenu", (e) => { e.preventDefault(); slotMenu(track, slot, e.clientX, e.clientY); });
        } else {
          if (track.kind === "midi") {
            const add = html("button", "launch-add", "+");
            add.type = "button";
            add.title = "New MIDI clip here";
            add.addEventListener("click", () => newMidiClip(track, row));
            cell.append(add);
          }
          cell.addEventListener("contextmenu", (e) => { e.preventDefault(); emptyMenu(track, row, e.clientX, e.clientY); });
        }
        cells.set(`${track.id}:${row}`, { cell, slot, progress });
        grid.append(cell);
      });
    }

    // The foot: stop buttons.
    grid.append(html("div", "launch-foot-corner"));
    list.forEach((track) => {
      const stop = html("button", "launch-stop", "■");
      stop.type = "button";
      stop.title = `Stop ${track.name} (on the next quantize line)`;
      stop.addEventListener("click", () => daw("stopSlot", { track: track.id }));
      stops.set(track.id, stop);
      grid.append(stop);
    });

    hint.textContent = list.some((t) => t.slots?.length) ? "" : "Right-click a clip in the song → Copy to the launcher, or + on a MIDI track";
    setStatus(screen.status ?? {});
  };

  //------------------------------------------------------------------ the playing lights
  const setStatus = (status) => {
    const launch = status?.launch ?? {};
    backToSong.classList.toggle("is-active", Boolean(status?.onLauncher));
    record.classList.toggle("is-on", Boolean(status?.launchRecording));
    backToSong.disabled = !status?.onLauncher;
    const playingRows = new Map(); // row -> [playing, filled]
    for (const [key, { cell, slot, progress }] of cells) {
      const trackId = Number(key.split(":")[0]);
      const state = launch[trackId] ?? {};
      const playing = slot != null && state.playing === slot.id;
      const queued = slot != null && state.queued === slot.id;
      cell.classList.toggle("is-playing", playing);
      cell.classList.toggle("is-queued", queued);
      if (progress) progress.style.width = playing ? `${Math.round((state.progress ?? 0) * 1000) / 10}%` : "0";
      if (slot) {
        const entry = playingRows.get(slot.row) ?? [0, 0];
        playingRows.set(slot.row, [entry[0] + (playing ? 1 : 0), entry[1] + 1]);
      }
    }
    sceneButtons.forEach((button, row) => {
      const [playing, filled] = playingRows.get(row) ?? [0, 0];
      button.classList.toggle("is-playing", filled > 0 && playing === filled);
    });
    for (const [trackId, stop] of stops) {
      const state = launch[trackId] ?? {};
      stop.classList.toggle("is-queued", state.queued === STOP);
      stop.classList.toggle("is-on", Boolean(state.on) && state.playing === -1);
      heads.get(trackId)?.classList.toggle("is-on-launcher", Boolean(state.on));
      heads.get(trackId)?.classList.toggle("is-going-back", state.queued === SONG);
    }
  };

  return { element, update: (session) => { if (session) render(); }, setStatus, firstFreeRow, NOT_QUEUED };
}
