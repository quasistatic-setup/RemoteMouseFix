# Contributing

Thanks for looking into RemoteMouseFix. The most valuable contribution right now is a
measurement from a setup other than TeamViewer with WoW 3.3.5a.

## Report a setup instead of guessing

Run the A/B profile as described in
[Report a problem or test another setup](../README.md#report-a-problem-or-test-another-setup)
and open an issue with the resulting log. The correction was derived from measurements,
and changes to it are accepted on the same basis. Review a log before attaching it; it
contains window geometry, process names and identifiers.

## Safety rules that apply to every change

[AGENTS.md](../AGENTS.md) lists the project's boundaries in full. The short version, none of
which is negotiable:

- No DLL injection, no driver, no code in the game process, no `ClipCursor`.
- No keyboard hook, no network access, no installer, no registry writes, no admin rights.
- Input is only ever modified with `diagnostic_mode = false`, an active correction mode,
  the target window in front and its cursor hidden. Buttons, the wheel and physical input
  always pass through untouched.
- The emergency-off hotkey `Ctrl+Alt+C` must stay registered, otherwise correction stays
  locked. A user may be controlling the machine remotely and must never lose the pointer.
- The hook callback must not block. No file I/O, no locks, no output from it.
- No external dependencies. Every EXE stays statically linked.

## Build and test

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Cross-building from WSL works too, see the README. A change counts as tested when:

- it compiles without warnings, with MSVC `/W4` or GCC `-Wall -Wextra -Wpedantic`;
- a run produces a log whose final lines report `queue drops 0` and `watchdog trips 0`;
- a change to the correction was tested against `MouseLookProbe` with injected absolute
  input in every mode. A click without movement must sum to zero, a drag of known length
  must sum to that length.

Never run tests in a real user's log folder. Rotation deletes evidence there.

## Pull requests

- One logical change per commit, present-tense subject line. The history uses
  [Conventional Commits](https://www.conventionalcommits.org/) prefixes such as `feat:`,
  `fix:`, `docs:` and `chore:`.
- Documentation is English and describes the current state. Dated measurement reports are
  the exception and stay as they were written.
- Update [CHANGELOG.md](../CHANGELOG.md) under `Unreleased` for anything a user would notice.
- Never commit logs, build output or captured session data.
- Say in the pull request what you tested and on which Windows version.

By contributing you agree that your work is published under the MIT
[LICENSE](../LICENSE).
