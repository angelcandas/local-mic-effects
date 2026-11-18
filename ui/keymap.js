// Custom key commands for the DAW: one table of named actions with their default keys,
// the user's changes (saved between launches) and the "Key commands" dialog.
//
// The DAW's own key handler (DawScreen.onKey in daw.js) still knows every default. This
// file sits in front of it: a key the user bound runs its action; a default key the user
// moved away is ignored; every other key goes on to the old handler unchanged.
//
// A key is written as text, "Ctrl+Shift+T": Ctrl stands for Ctrl or Cmd, then Alt (Option),
// then Shift, then the key. Letters and digits come from the physical key, so Option on a
// Mac and Shift+1 give the same text on every layout.

const STORAGE_KEY = "micfx.daw.keymap";

// [id, group, label, default keys]. The id is a name in DawScreen.commands.
const T = "Transport", K = "Tracks", C = "Clips", F = "File", V = "View";
export const KEY_COMMANDS = [
  ["play", T, "Play / pause", ["Space"]],
  ["stop", T, "Stop and go back to the start", ["Num 0"]],
  ["record", T, "Record on the armed tracks", ["R", "Num *"]],
  ["punch", T, "Punch on / off", ["Shift+R"]],
  ["stepBack", T, "Playhead back a beat", ["←"]],
  ["stepForward", T, "Playhead forward a beat", ["→"]],
  ["start", T, "Go to the start of the song", ["Home"]],
  ["end", T, "Go to the end of the song", ["End"]],
  ["loop", T, "Loop range on / off", ["L", "Num /"]],
  ["metronome", T, "Click (metronome) on / off", ["C"]],
  ["follow", T, "Follow the playhead on / off", ["F"]],
  ["addMarker", T, "Add a marker at the playhead", ["K", "Insert"]],
  ["previousMarker", T, "Previous marker", ["["]],
  ["nextMarker", T, "Next marker", ["]"]],
  ["tapTempo", T, "Tap tempo", ["T"]],

  ["previousTrack", K, "Select the track above", ["↑"]],
  ["nextTrack", K, "Select the track below", ["↓"]],
  ["addTrack", K, "New audio track", ["Ctrl+T"]],
  ["addMidiTrack", K, "New MIDI track", ["Ctrl+Shift+T"]],
  ["duplicateTrack", K, "Duplicate the selected track", ["Ctrl+Shift+D"]],
  ["groupTracks", K, "Group the selected tracks", ["Ctrl+G"]],
  ["ungroup", K, "Ungroup", ["Ctrl+Shift+G"]],
  ["bounce", K, "Bounce the selected track to a new audio track", ["Ctrl+B"]],
  ["renameTrack", K, "Rename the selected track", ["Ctrl+R"]],
  ["deleteTrack", K, "Delete the selected track", ["Ctrl+Delete", "Ctrl+Backspace"]],
  ["muteTrack", K, "Mute the selected track", ["M", "0"]],
  ["soloTrack", K, "Solo the selected track", ["S"]],
  ["armTrack", K, "Arm the selected track", ["A"]],
  ["monitorTrack", K, "Monitor the mic on the selected track", ["I"]],
  ["quantize", K, "Quantize the selected clip or track", ["Q", "Ctrl+U"]],
  ["togglePiano", K, "Computer keyboard piano on / off", ["Ctrl+K"]],

  ["selectAll", C, "Select every clip", ["Ctrl+A"]],
  ["previousClip", C, "Select the previous clip on the track", [","]],
  ["nextClip", C, "Select the next clip on the track", ["."]],
  ["split", C, "Split the selected clip at the playhead", ["Ctrl+E"]],
  ["duplicate", C, "Duplicate", ["Ctrl+D"]],
  ["cut", C, "Cut", ["Ctrl+X"]],
  ["copy", C, "Copy", ["Ctrl+C"]],
  ["paste", C, "Paste", ["Ctrl+V"]],
  ["delete", C, "Delete the clip", ["Delete", "Backspace"]],
  ["nudgeBack", C, "Nudge the selected clip back a beat", ["Alt+←"]],
  ["nudgeForward", C, "Nudge the selected clip forward a beat", ["Alt+→"]],
  ["muteClip", C, "Mute / unmute the selected clip", ["Shift+M"]],
  ["loopClip", C, "Loop this clip", ["Ctrl+L"]],
  ["editWarp", C, "Edit the selected clip (pitch, timing, warp)", ["W"]],
  ["toolSelect", C, "Select tool", ["V", "1"]],
  ["toolRange", C, "Range tool", ["2"]],
  ["toolCut", C, "Cut tool", ["B"]],
  ["toolGain", C, "Gain tool", ["G"]],
  ["toolEnvelope", C, "Envelope tool", ["H"]],
  ["toolDraw", C, "Draw tool", ["P"]],
  ["toolWarp", C, "Warp tool", ["J"]],
  ["gridNarrow", C, "Narrower grid", ["Ctrl+1"]],
  ["gridWiden", C, "Wider grid", ["Ctrl+2"]],
  ["gridTriplet", C, "Triplet grid on / off", ["Ctrl+3"]],
  ["gridSnap", C, "Snap on / off", ["Ctrl+4"]],
  ["nextHitpoint", C, "Playhead to the next hitpoint", ["Alt+N"]],
  ["previousHitpoint", C, "Playhead to the previous hitpoint", ["Alt+B"]],

  ["new", F, "New project", ["Ctrl+N"]],
  ["open", F, "Open a project", ["Ctrl+O"]],
  ["save", F, "Save", ["Ctrl+S"]],
  ["saveAs", F, "Save as", ["Ctrl+Shift+S"]],
  ["import", F, "Import audio", ["Ctrl+I", "Ctrl+Shift+I"]],
  ["export", F, "Export (bounce)", ["Ctrl+Shift+R"]],
  ["undo", F, "Undo", ["Ctrl+Z"]],
  ["redo", F, "Redo", ["Ctrl+Shift+Z", "Ctrl+Y"]],

  ["toggleView", V, "Tracks / mixer", ["Tab", "X"]],
  ["toggleDock", V, "Show / hide the effects panel", ["E", "Shift+Tab"]],
  ["toggleLibrary", V, "Show / hide the library", ["Shift+B", "Ctrl+Alt+B"]],
  ["searchLibrary", V, "Search the library", ["Ctrl+F"]],
  ["zoomIn", V, "Zoom in", ["+", "="]],
  ["zoomOut", V, "Zoom out", ["-", "_"]],
  ["zoomSelection", V, "Zoom to the selected clip", ["Z"]],
  ["zoomFit", V, "Zoom to the whole song", ["Shift+Z"]],
  ["taller", V, "Taller tracks", ["Shift+↑"]],
  ["shorter", V, "Shorter tracks", ["Shift+↓"]],
  ["shortcuts", V, "Keyboard shortcuts list", ["?", "/"]],
  ["keyCommands", V, "Key commands (this window)", []],
].map(([id, group, label, keys]) => ({ id, group, label, keys }));

const BY_ID = new Map(KEY_COMMANDS.map((command) => [command.id, command]));

const NAMED = { " ": "Space", ArrowLeft: "←", ArrowRight: "→", ArrowUp: "↑", ArrowDown: "↓", Esc: "Escape" };
const MODIFIER_KEYS = new Set(["Control", "Shift", "Alt", "Meta", "AltGraph", "CapsLock", "OS"]);

// The text of a key press, or "" for a modifier on its own.
export function eventCombo(event) {
  if (MODIFIER_KEYS.has(event.key)) return "";
  const code = event.code || "";
  let key;
  if (/^Key[A-Z]$/.test(code)) key = code.slice(3);
  else if (/^Digit[0-9]$/.test(code)) key = code.slice(5);
  else if (/^Numpad[0-9]$/.test(code)) key = `Num ${code.slice(6)}`;
  else if (code === "NumpadMultiply") key = "Num *";
  else if (code === "NumpadDivide") key = "Num /";
  else if (code === "NumpadAdd") key = "Num +";
  else if (code === "NumpadSubtract") key = "Num -";
  else key = NAMED[event.key] ?? (event.key.length === 1 ? event.key.toUpperCase() : event.key);
  const shiftMatters = !(event.key.length === 1 && !/[a-z0-9]/i.test(event.key)); // "?" is already shifted
  return [(event.ctrlKey || event.metaKey) && "Ctrl", event.altKey && "Alt", event.shiftKey && shiftMatters && "Shift", key]
    .filter(Boolean).join("+");
}

// Keys the user may not take: Esc cancels and closes things everywhere.
const RESERVED = new Set(["Escape"]);

//==============================================================================
// The bindings: defaults plus the user's changes. changes[id] is the whole list of keys
// for that action (an empty list: none).
export function createKeymap({ load, save } = {}) {
  let changes = {};
  try { const stored = load?.(); if (stored && typeof stored === "object") changes = stored; } catch { /* no storage */ }
  const persist = () => { try { save?.(changes); } catch { /* no storage */ } };
  const listeners = new Set();
  const changed = () => { index = null; persist(); listeners.forEach((fn) => fn()); };

  const keysOf = (id) => {
    const own = changes[id];
    return Array.isArray(own) ? own.filter((k) => typeof k === "string") : (BY_ID.get(id)?.keys ?? []);
  };
  const isChanged = (id) => Array.isArray(changes[id]);

  // key text -> action id, and the default keys that moved away.
  let index = null;
  const build = () => {
    const map = new Map(), moved = new Set();
    for (const { id } of KEY_COMMANDS) for (const key of keysOf(id)) if (!map.has(key)) map.set(key, id);
    for (const { id, keys } of KEY_COMMANDS) if (isChanged(id)) for (const key of keys) if (!map.has(key)) moved.add(key);
    index = { map, moved };
    return index;
  };
  const table = () => index ?? build();

  return {
    keysOf, isChanged,
    // The action for a key press: { id } to run, { ignore: true } for a default key that
    // was moved away, or null to leave the key to the old handler.
    resolve(event) {
      if (!Object.keys(changes).length) return null;
      const combo = eventCombo(event);
      if (!combo) return null;
      const { map, moved } = table();
      if (map.has(combo)) {
        const id = map.get(combo);
        // An unchanged action's default key is the old handler's job.
        return isChanged(id) ? { id } : null;
      }
      return moved.has(combo) ? { ignore: true } : null;
    },
    // Who has this key now, if anyone.
    owner(combo) { return table().map.get(combo) ?? null; },
    reserved: (combo) => RESERVED.has(combo),
    // Give an action one key (taking it from whoever had it).
    assign(id, combo) {
      const from = table().map.get(combo);
      if (from && from !== id) changes[from] = keysOf(from).filter((k) => k !== combo);
      changes[id] = [combo];
      changed();
    },
    clear(id) { changes[id] = []; changed(); },
    reset(id) { delete changes[id]; changed(); },
    resetAll() { changes = {}; changed(); },
    anyChanged: () => Object.keys(changes).length > 0,
    onChange(fn) { listeners.add(fn); return () => listeners.delete(fn); },
  };

}

//==============================================================================
// The dialog. `h` is html() from widgets.js, `keyText` shows Cmd/Option on a Mac.
export function createKeyDialog({ keymap, h, keyText, toast }) {
  const backdrop = h("div", "shortcuts keycmds");
  backdrop.hidden = true;
  const panel = h("div", "shortcuts-panel");
  const head = h("div", "export-head");
  const close = h("button", "mini", "×");
  close.type = "button";
  close.title = "Close";
  head.append(h("strong", "", "Key commands"), close);
  const tools = h("div", "keycmds-tools");
  const search = h("input", "keycmds-search");
  search.type = "search";
  search.placeholder = "Search commands or keys";
  const resetAll = h("button", "mini keycmds-reset-all", "Reset all to defaults");
  resetAll.type = "button";
  tools.append(search, resetAll);
  const note = h("div", "keycmds-note");
  const list = h("div", "keycmds-list");
  panel.append(head, tools, note, list);
  backdrop.append(panel);

  let capturing = null; // { id, row, pending? }

  const stopCapture = () => {
    capturing = null;
    note.textContent = "";
    note.className = "keycmds-note";
    render();
  };

  const button = (text, title, onClick, className = "mini") => {
    const b = h("button", className, text);
    b.type = "button";
    b.title = title;
    b.addEventListener("click", (event) => { event.stopPropagation(); onClick(); });
    return b;
  };

  const render = () => {
    const query = search.value.trim().toLowerCase();
    const rows = [];
    let group = "";
    for (const command of KEY_COMMANDS) {
      const keys = keymap.keysOf(command.id);
      const haystack = `${command.label} ${command.group} ${keys.map(keyText).join(" ")}`.toLowerCase();
      if (query && !haystack.includes(query)) continue;
      if (command.group !== group) { group = command.group; rows.push(h("h4", "", group)); }
      const row = h("div", "keycmd" + (keymap.isChanged(command.id) ? " is-changed" : "") + (capturing?.id === command.id ? " is-capturing" : ""));
      const keysCell = h("span", "keycmd-keys");
      if (capturing?.id === command.id) keysCell.append(h("em", "", capturing.pending ? "Waiting for your answer…" : "Press the new key (Esc cancels)"));
      else if (keys.length) for (const key of keys) keysCell.append(h("kbd", "", keyText(key)));
      else keysCell.append(h("em", "", "no key"));
      const actions = h("span", "keycmd-actions");
      actions.append(
        button("Change", "Press a new key for this command", () => startCapture(command.id)),
        ...(keys.length ? [button("Clear", "Leave this command without a key", () => { keymap.clear(command.id); render(); })] : []),
        ...(keymap.isChanged(command.id) ? [button("Reset", "Back to the default key", () => { keymap.reset(command.id); render(); })] : []));
      row.append(h("span", "keycmd-label", command.label), keysCell, actions);
      rows.push(row);
    }
    if (!rows.length) rows.push(h("p", "keycmds-empty", "No command matches."));
    list.replaceChildren(...rows);
    resetAll.disabled = !keymap.anyChanged();
  };

  const startCapture = (id) => {
    capturing = { id };
    note.className = "keycmds-note";
    note.textContent = `Press the new key for “${KEY_COMMANDS.find((c) => c.id === id).label}”. Esc cancels.`;
    render();
  };

  const finish = (id, combo) => {
    keymap.assign(id, combo);
    toast?.(`${KEY_COMMANDS.find((c) => c.id === id).label}: ${keyText(combo)}`);
    stopCapture();
  };

  // While the dialog is open it sees every key first.
  const onKey = (event) => {
    if (backdrop.hidden) return;
    if (capturing && !capturing.pending) {
      const combo = eventCombo(event);
      if (!combo) return; // a modifier on its own: wait for the key
      event.preventDefault(); event.stopPropagation();
      if (combo === "Escape") return stopCapture();
      if (keymap.reserved(combo)) { note.className = "keycmds-note is-warn"; note.textContent = `${keyText(combo)} can't be used.`; return; }
      const owner = keymap.owner(combo);
      if (!owner || owner === capturing.id) return finish(capturing.id, combo);
      // Taken: ask before moving it.
      const id = capturing.id;
      capturing.pending = true;
      const label = KEY_COMMANDS.find((c) => c.id === owner).label;
      note.className = "keycmds-note is-warn";
      note.replaceChildren(h("span", "", `${keyText(combo)} is already “${label}”. `),
        button("Reassign it here", "Take the key from that command", () => finish(id, combo), "mini primary"),
        button("Keep it there", "Cancel", stopCapture));
      render();
      return;
    }
    if (event.key === "Escape") { event.preventDefault(); event.stopPropagation(); hide(); }
    else if (event.target !== search && !event.ctrlKey && !event.metaKey) event.stopPropagation(); // no shortcut fires behind the dialog
    else if (event.target === search) event.stopPropagation();
  };
  const hide = () => { stopCapture(); backdrop.hidden = true; };
  const show = () => { backdrop.hidden = false; search.value = ""; capturing = null; note.textContent = ""; render(); search.focus(); };

  document.addEventListener("keydown", onKey, true);
  close.addEventListener("click", hide);
  backdrop.addEventListener("pointerdown", (event) => { if (event.target === backdrop) hide(); });
  search.addEventListener("input", render);
  resetAll.addEventListener("click", () => { keymap.resetAll(); toast?.("Key commands are back to the defaults."); stopCapture(); });

  return { element: backdrop, show, hide, toggle: () => (backdrop.hidden ? show() : hide()) };
}
