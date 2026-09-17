# RemoteMouseFix: project rules

This file describes what is specific to this repository. Any machine-wide or personal
agent rules apply in addition and take precedence on Git, secrets and documentation form.

## Project boundaries

A portable Windows tool against faulty mouse and camera jumps in 3D games used through
remote-control software. First case: TeamViewer with WoW 3.3.5a.

Technically fixed and not to be changed without an explicit request:
C++17, Win32 API, CMake, Windows 10/11, x64 for now.

The repository contains source code, configuration templates, the test target
`tools/probe` and documentation only. Recorded logs are session data and never belong in
Git.

## Overarching safety rules

In every mode, without exception:

- No DLL injection, no driver, no code inside the game process.
- No `ClipCursor`, no change to the game or its files.
- No keyboard hook. Hotkeys use `RegisterHotKey` and see exactly those combinations.
  Keystrokes and text content are technically not observable.
- No network access, no administrator rights, no installer, no registry writes.
- Effect only while running, nothing left behind after it exits.
- Logging is limited to the target process. The exception is focus changes with window
  handle, PID and process name, but without input from other applications.

With `diagnostic_mode = true` no input is modified and no correction mode can be enabled.
That is the default of the shipped `config.json`.

Interfering with input is permitted only under all of the following conditions. Any
extension beyond them needs an evaluated log that justifies it first:

- `diagnostic_mode = false` and a correction mode is active. The startup mode defaults to
  `off`.
- Only injected `WM_MOUSEMOVE` without our own signature in `dwExtraInfo` are withheld,
  and only while the target process is in the foreground and the cursor is hidden.
- Keys, the wheel and physical input are never withheld or modified.
- Our own synthetic input always carries the signature and is never processed again.
- The emergency-off hotkey `Ctrl+Alt+C` must be registered, otherwise correction stays
  locked. The user may be controlling the machine through remote software and must never
  lose control of the pointer.
- Watchdogs disable the correction on anomalies and write the reason to the log: a cursor
  hidden for too long, failed output, own input that does not come back.

## Non-negotiable implementation limits

- The hook callback must not do anything blocking. Windows silently removes a low-level
  hook after `LowLevelHooksTimeout`. File I/O, locks and the correction output do not
  belong in the callback; the output is posted to the message loop.
- The ring-buffer coupling is a single producer and a single consumer.
- DPI awareness is a correctness condition. The hook and `SetCursorPos` work in physical
  pixels.
- The game's warp anchor is learned from the data, never assumed to be the client centre.
- No external dependencies. Every EXE stays statically linked.

## Verification entry points

```bash
# Cross-build from WSL, produces RemoteMouseFix.exe and MouseLookProbe.exe
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake
cmake --build build-mingw -j
```

On Windows, MSVC is the preferred route, see [README.md](README.md).

A build counts as verified only once it compiles without warnings and a run produces a log
with `queue drops 0`. A change to the correction counts as verified only once it has been
tested against `MouseLookProbe` with injected absolute input in every mode. Never run
tests in a user's log folder: rotation deletes evidence there.

## Topic map

| Topic | Location |
| --- | --- |
| Log format, columns, how to read a click into the 3D view | [docs/diagnostics.md](docs/diagnostics.md) |
| Build with MSVC and cross-build, configuration keys, test procedures | [README.md](README.md) |
| Symptom, working hypothesis, correction modes | [README.md](README.md) |
| Measurement result of 2026-09-12, chain of causes and constraints for Phase 2 | [docs/findings-2026-09-12.md](docs/findings-2026-09-12.md) |
