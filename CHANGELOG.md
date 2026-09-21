# Changelog

All notable changes to RemoteMouseFix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html) with a leading `0.` while the
correction is still experimental.

## [Unreleased]

## [0.7.0] - 2026-09-21

### Added

- `InputEchoCheck.exe`, a companion that measures whether our own `SendInput` returns
  through our own hook and reports the foreground window's integrity level. It answers the
  watchdog reason "own moves never reached the hook", which `SendInput` itself cannot,
  because UIPI discards input without failing the call.

### Changed

- The console shows a colour-coded status panel instead of a single line of counters:
  one line for the game window, one for the fix, one for what it has done and one that
  names the next step. A correction that a watchdog switched off now stays visible in red
  until it is switched back on, and the hotkeys stay on screen.
- The released package keeps only the launchers, the two programs, README and LICENSE at
  the top level; the profiles move to `config\`, and `InputEchoCheck` writes its result
  into `logs\` instead of the program folder. A relative configuration name that is not
  found beside the EXE is now also looked up in `config\`, so an older folder and the
  previous command line keep working.

### Fixed

- `MouseLookProbe` opens its log through `_wfopen_s` under MSVC, so the build stays free of
  warnings with `/W4 /WX`.

## [0.6.0] - 2026-09-17

First public release.

### Added

- Gameplay profile `config-play.json` with `Start-Playing.cmd`, which enables the
  recommended `absolute` correction immediately and keeps logs compact.
- A/B profile `config-ab-test.json` with `Start-AB-Test.cmd` for comparing the modes.
- Hotkeys to switch modes while running: `Ctrl+Alt+1` absolute, `Ctrl+Alt+2` relative,
  `Ctrl+Alt+C` off, `Ctrl+Alt+Q` quit, `Ctrl+Alt+M` log marker.

### Changed

- `absolute` mode spreads batched remote movement across steps of at most 20 pixels per
  axis every 16 ms instead of applying it at once.
- Pending movement is discarded on button release, focus loss and correction shutdown.
  When new remote movement reverses an axis, stale queued movement on that axis is
  dropped, so the camera no longer pulls back elastically.
- Opposing deltas below 5 pixels are treated as remote-input noise.

## [0.2.0] - 2026-09-12

Guarded correction, not released as a binary.

### Added

- Correction modes `absolute` and `relative`, both gated behind `diagnostic_mode = false`
  and an active mode. The hook withholds injected `WM_MOUSEMOVE` from other programs while
  the target window is in front and its cursor is hidden, then passes the remote pointer's
  own delta to the game through signed `SendInput`.
- Watchdogs that disable the correction after a long hidden-cursor phase, repeated output
  failures, or own events that fail to echo back through the hook.
- Runtime learning of the warp anchor from hidden-cursor samples instead of assuming the
  client centre.

### Removed

- A `SetCursorPos` output path, dropped after measurement: it lost about 40 percent of a
  drag because it raced with the input thread finishing the withheld event.

## [0.1.0] - 2026-09-12

Phase 1, observation only.

### Added

- Low-level mouse hook, state sampler and log writer with rotation, limited to the
  configured target process.
- `MouseLookProbe` as a stand-in game and `emulate-remote.ps1` to inject a
  TeamViewer-shaped absolute input stream.
- Dated measurement report in [docs/findings-2026-09-12.md](docs/findings-2026-09-12.md).

[Unreleased]: https://github.com/quasistatic-setup/RemoteMouseFix/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/quasistatic-setup/RemoteMouseFix/releases/tag/v0.7.0
[0.6.0]: https://github.com/quasistatic-setup/RemoteMouseFix/releases/tag/v0.6.0
[0.2.0]: https://github.com/quasistatic-setup/RemoteMouseFix/commit/cafe36a
[0.1.0]: https://github.com/quasistatic-setup/RemoteMouseFix/commit/aa00292
