# RemoteMouseFix v0.8.0 — release notes

This version packages the **WoW 3.3.5a / TeamViewer tested** correction with
a more consistent 16 ms smoothing timer and easier target selection.

## What's new

- **Separate executable targets for Playing and A/B testing.** Edit
  `RemoteMouseFix.ini` beside the EXE. `[Playing]` and `[AB-Test]` each accept
  a process filename such as `Wow.exe` or an exact, absolute Windows EXE path.
  The program monitors an already running target; it does not start the game.
- **High-resolution 16 ms smoothing** enabled in both launch profiles. The
  measured average timer interval improved from approximately 26.7 ms
  (`WM_TIMER`) to 16.2 ms in A/B tests. Windows falls back to `WM_TIMER`
  if the high-resolution waitable timer is unavailable.
- **More useful A/B diagnostics:** numbered `Ctrl+Alt+M` test markers,
  250 ms timing summaries and a separate `logs-ab/` directory.
- **MouseLookProbe modes A/B/C:** visible/no-warp, hidden/no-warp and
  hidden/recentering, to help distinguish different input behaviors.
- **Guarded in-hold anchor relearning** to handle changed game-window
  positions without replaying queued corrections against the previous anchor.

## Compatibility and important limitations

- **Validated scenario:** WoW 3.3.5a controlled through TeamViewer, using
  cursor recentering during mouse-look. The stable build was preferred in
  practical testing because it avoided camera jumps.
- **Not a generic fix for every game.** A game that hides the cursor without
  recentering (like MouseLookProbe mode B) can lose movement when correction
  is enabled. Do not use the Playing/A/B correction on an unverified target.
  The default `RemoteMouseFix.exe` configuration is diagnostic-only and
  does not change input.
- The previously tested experimental `WarpEvidence` gate was **not**
  included because it caused regressions in WoW (including camera jumps
  during short clicks).
- Delays or gaps in TeamViewer input can still occur. This tool cannot
  recover input that was not delivered.
- The EXEs are unsigned. Follow the README's download verification and
  Windows security guidance. `Ctrl+Alt+C` immediately disables correction,
  and `Ctrl+Alt+Q` quits the tool.

## Getting started

1. Download the `RemoteMouseFix-v0.8.0-win-x64.zip` attachment and verify
   its SHA-256 checksum against the accompanying `.sha256` file.
2. Extract the ZIP. If necessary, edit `RemoteMouseFix.ini`.
3. Start WoW normally, then run `Start-Playing.cmd` (not as administrator).

For instructions and logs, see the [README](https://github.com/quasistatic-setup/RemoteMouseFix#readme).

**Full changes:** https://github.com/quasistatic-setup/RemoteMouseFix/compare/v0.7.0...v0.8.0

> Maintainer note: publish these notes only together with the two named
> release assets (`.zip` and `.zip.sha256`), after verifying the ZIP's
> actual SHA-256 checksum. Do not invent a checksum.
