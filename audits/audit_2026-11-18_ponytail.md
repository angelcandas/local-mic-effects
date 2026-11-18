# MicFX Ponytail Audit

**Date** — 2026-11-18

## What this repo does

MicFX processes microphone audio in real time, runs as an AU/VST3 plug-in, and includes a standalone recording and mixing app. This audit assumes one person working locally, with 10–30 tracks at 48 kHz and background jobs running during playback.

## Must fix

1. **Opening another project or quitting can discard unsaved work.** The DAW tracks unsaved edits, but Open, New, and Quit proceed without checking them. A clean close also removes the marker that enables crash recovery, so reopening can show only the last manual save. Use one Save, Discard, or Cancel prompt before leaving a dirty project. [DawBridge.cpp](src/DawBridge.cpp#L1806) · [StandaloneApp.cpp](src/StandaloneApp.cpp#L832) · [Project.cpp](src/daw/Project.cpp#L309)

2. **A crash during Save can remove the previous project file.** The save code writes a temporary XML file, then calls JUCE’s `moveFileTo`, which deletes the destination before renaming the replacement. A crash between those operations can leave no `project.xml` for MicFX to open. Write directly with JUCE’s `XmlElement::writeTo(File)`, which already stages and replaces the file. [Project.cpp](src/daw/Project.cpp#L214) · [juce_File.cpp](libs/JUCE/modules/juce_core/files/juce_File.cpp#L300)

3. **A failed Save As can break clips in the original session.** Save As rewrites live clip paths while copying audio. A forced copy failure returned an error while the original session’s clip pointed to a nonexistent file. Copy and save against a separate project snapshot, then switch the live session only after everything succeeds. [Project.cpp](src/daw/Project.cpp#L262)

4. **A newly opened project can inherit the previous project’s effects.** The audio engine reuses running effects when track IDs and insert layouts match. Opening a project with denoise off after one with denoise on retained the old 512-sample denoise delay. Clear project-scoped audio and effect reuse when replacing the session so playback starts from the settings on disk. [DawEngine.cpp](src/daw/DawEngine.cpp#L733)

5. **A background bounce can modify the wrong project.** A bounce renders a snapshot but applies its result to whichever project is open when it finishes. If the new project has the same track ID, MicFX can add the old audio and mute the new project’s track. Tie jobs that edit a session to the project that started them and discard their results after a switch. [DawBridge.cpp](src/DawBridge.cpp#L1692) · [DawBridge.cpp](src/DawBridge.cpp#L2553)

6. **A full-song export can repeat the active loop.** The export engine inherits playback looping from the project. A one-second loop played again during what should have been a silent export tail. Disable playback looping in the export snapshot and use the explicit start and end times for loop-only exports. [Exporter.cpp](src/daw/Exporter.cpp#L81) · [DawEngine.cpp](src/daw/DawEngine.cpp#L2806)

7. **Cancelling an overwrite deletes the previous export.** The exporter deletes the existing file before it starts writing. Cancelling a replacement export left the previous file gone. Render to a temporary file beside the destination and replace the old export only after the new one finishes. [Exporter.cpp](src/daw/Exporter.cpp#L11)

8. **A full disk can truncate a recording without a warning.** The recorder checks whether audio entered the queue, but the background writer throws away the result of writing it to disk. A failed write can leave a short take whose reported duration still includes the missing audio. Carry disk-write errors back to the recorder and report the duration actually saved. [Recorder.cpp](src/Recorder.cpp#L121) · [juce_AudioFormatWriter.cpp](libs/JUCE/modules/juce_audio_formats/format/juce_AudioFormatWriter.cpp#L273)

9. **Quick setlist changes can play two backing tracks.** Changing songs removes loaded tracks, but an older load can still finish and add its track. Selecting two songs back to back left both backing tracks in the player. Invalidate pending loads when the setlist replaces the session so only the selected song can start playing. [Setlist.cpp](src/Setlist.cpp#L181) · [Multitrack.cpp](src/Multitrack.cpp#L110)

10. **The AI-control key file can lose its owner-only permissions.** MicFX sets permissions on the discovery file, then JUCE replaces it with a temporary file created with ordinary read permissions. Where parent-directory permissions allow access, another local account can read the key that authorizes project edits. Create the replacement with owner-only permissions before writing the key, and use an operating-system random source to generate it. [McpServer.cpp](src/McpServer.cpp#L46) · [juce_File.cpp](libs/JUCE/modules/juce_core/files/juce_File.cpp#L798)

## Should fix

11. **Opening several projects can steadily consume RAM.** The DAW keeps decoded audio from old projects until the sample rate changes. At 48 kHz, 24 four-minute stereo tracks take about 2.2 GB before effects or waveform data. Release unused decoded buffers on project switches and reload them when needed. [AudioPool.cpp](src/daw/AudioPool.cpp#L384) · [Multitrack.cpp](src/Multitrack.cpp#L66)

12. **Exporting stems repeatedly decodes the whole session.** Each stem creates a fresh engine, loads every track’s audio, and runs the full graph with other tracks muted. A 24-track export can decode the same sources 24 times, or 48 with normalization. Share decoded audio across passes first and keep the effect state fresh for each render. [Exporter.cpp](src/daw/Exporter.cpp#L175) · [DawEngine.cpp](src/daw/DawEngine.cpp#L153)

13. **Two test groups write into the user’s real Music folder.** Warp and drum slicing use the app’s default recordings path even though the tests already have temporary fixtures. Six assertions failed on those writes and passed with an isolated home directory. Give both tests a temporary project so they behave the same in CI and on a developer’s machine. [DspTests.cpp](tests/DspTests.cpp#L9812) · [DspTests.cpp](tests/DspTests.cpp#L16583)

14. **The documented minimum CMake cannot read the presets.** The README says 3.22, but the preset file uses schema 6, which requires 3.25. A user can meet the documented requirement and fail on the first build command. Set the preset schema to 3, which supports the fields MicFX uses. [CMakePresets.json](CMakePresets.json#L2) · [README.md](README.md#L462)

## Nice to have

15. **Open Recent duplicates the regular Open workflow.** Both paths stop playback, load the project, and refresh the same state. Route both through the same function so the unsaved-changes fix covers both entry points. [DawBridge.cpp](src/DawBridge.cpp#L6976)

## Verdict

Fix project saving and switching first, then protect exports and recordings. Those failures can lose work during ordinary use.

**Lean** — About 15 fewer lines by using JUCE’s XML replacement and sharing the project-opening workflow. No dependency changes.

**Verification** — The debug build, UI syntax and embedding checks, and five targeted reproductions completed. The isolated headless suite reported 2,112 passing checks and one failure at the MCP listener. A separate check confirmed the sandbox denies binding to loopback.

**Not checked** — The audit sampled WebView, DSP, and instrument code outside the main flows. It did not cover GUI or physical audio testing, real model inference, non-macOS builds, large-session benchmarks, or vendored libraries beyond the relevant JUCE calls.
