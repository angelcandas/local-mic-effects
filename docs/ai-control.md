# AI control

MicFX can be driven by an AI program such as Claude Desktop or Claude Code, through the Model Context Protocol (MCP).
You say "add reverb to track 2" or "transpose the instrumental tracks down 3" and the AI does it in MicFX, as ordinary
changes you can undo.

Everything stays on your computer. The server only listens on `127.0.0.1`, only answers programs that hold a key, and is
**off until you turn it on**.

## Set it up

1. In MicFX, open **Settings**, section **AI control**, and tick *Let AI programs on this computer control MicFX*.
2. Press **Copy setup**. It copies a snippet like this:

   ```json
   { "mcpServers": { "micfx": { "command": "/Applications/MicFX.app/Contents/MacOS/micfx-mcp" } } }
   ```

3. Paste it into your AI program's MCP servers (Claude Desktop: Settings, Developer, Edit Config; Claude Code:
   `claude mcp add micfx -- <the command>`), and restart that program.
4. Keep MicFX open. The AI program starts `micfx-mcp`, a small helper that passes its messages to the app.

The snippet holds no secret. The key lives in a file only you can read (`MicFX/mcp.json` in your application data folder)
and the helper reads it. **New key** makes a new one: restart the AI program afterwards.

If MicFX isn't open or AI control is off, the helper tells the AI so, and the AI tells you.

## What it can do

Every tool is one undo step with a readable name (Edit menu, or ask the AI for `history.undo`). A tool that fails leaves
nothing half done.

Which tracks: by number (`2`, the first is 1), by name (part of a name works), by role (`voice`, `instrumental`,
`guitar`, `piano`, `pad`, `synth`, `bass`, `drums`, `background`), `selected`, `all`, or a list. Tag a track's role with
`track.role` so "the guitars" or "the instrumental tracks" mean something. Time can be seconds, `bar 9`, a section name
(`chorus 2`), `loop`, `playhead`, `start`, `end`.

| Tool | What it does |
|---|---|
| `session.summary` | Tempo, key, sections, loop, every track (role, level, effects), returns, playhead. Start here. |
| `track.get` | One track in detail: every effect value, sends, clips. |
| `mix.plan`, `mix.report` | The last Auto Mix plan, the last Mix Check report. |
| `transport.play`, `transport.stop`, `transport.locate`, `loop.set` | Play, stop, move the playhead, set the loop. |
| `tempo.set`, `key.set` | Project tempo and key. |
| `track.add`, `track.rename`, `track.mute`, `track.solo`, `track.volume`, `track.pan`, `track.role` | Tracks. |
| `track.transpose` | Change the key of tracks by semitones (`-3`). |
| `effect.add`, `effect.remove`, `effect.set` | Effects on tracks (reverb, compressor, eq...). |
| `return.add`, `send.set` | Shared effect tracks and sends to them. |
| `mix.auto`, `mix.check`, `mix.fix` | Run Auto Mix (listen and apply), run Mix Check, apply one of its fixes. |
| `history.undo`, `history.redo` | Undo and redo. |

Not offered: deleting tracks or clearing the project. Ask for those in MicFX itself.

## Examples

- "Add reverb to track 2." → `effect.add { tracks: 2, type: "reverb" }`
- "Mute the voice and loop the second chorus." → `track.mute { tracks: "voice" }`, `loop.set { from: "chorus 2" }`
- "Make the guitars 3 dB quieter and pan them left." → `track.volume { tracks: "guitar", change: -3 }`, `track.pan`
- "Run Auto Mix in rock style but let me hear the plan first." → `mix.auto { style: "rock", apply: false }`

## Other programs

Any MCP client can use the HTTP door directly: `POST http://127.0.0.1:<port>/mcp` with the header
`Authorization: Bearer <key>` (port and key are in `mcp.json`). Requests from web pages are refused. Both the 2026-07-28
stateless protocol and the older `initialize` handshake are understood.
