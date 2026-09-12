# RemoteMouseFix

A portable Windows diagnostic tool for broken mouse and camera behaviour in 3D games
driven over remote desktop software. The first target case is **TeamViewer + World of
Warcraft 3.3.5a**.

**This is Phase 1: it observes and records. It does not correct anything.**

## The problem being investigated

Over TeamViewer, ordinary UI clicks and keyboard input in WoW work correctly. A single
click into the 3D view can already make the camera jump hard, and dragging to turn the
camera afterwards produces further large, unwanted jumps.

Working theory: a conflict between the absolute pointer position delivered by the remote
client and WoW's own relative mouse handling, including its cursor capture and recenter
logic.

That is a theory, not a diagnosis. Phase 1 produces the evidence to confirm or kill it,
which is why this build deliberately contains no correction logic: a tool that rewrote
input could not be trusted as a witness.

## What it records

A single `WH_MOUSE_LL` low-level mouse hook, active only while the configured target
process owns the foreground window:

- timestamp (millisecond wall clock plus a high-resolution offset)
- event type: moves, left/right/middle/X button down and up, wheel and horizontal wheel
- absolute cursor position in physical screen pixels
- delta to the previous position, and the time gap in milliseconds
- position in the target window's client area, and the offset from the client centre
- `dwExtraInfo`
- `LLMHF_INJECTED` and `LLMHF_LOWER_IL_INJECTED`
- the foreground window and its process

Plus, on a separate 10 ms sampler, the state that produces no hook events at all and
that a cursor recenter would show up in: cursor visibility, `ClipCursor` confinement,
client geometry, DPI and foreground transitions.

See [docs/diagnostics.md](docs/diagnostics.md) for the exact log format and a procedure
for reading a click into the 3D view.

## Constraints this build honours

- C++17, Win32 API, CMake, Windows 10/11, x64
- no DLL injection, no driver, no kernel component
- no admin rights, no installer, no registry writes
- no network access of any kind
- no change to the game or its files
- no `SendInput`, `mouse_event`, `SetCursorPos` or `ClipCursor`: input is never written
- no keyboard hook, so keystrokes and text content cannot be recorded
- no filtering or correction; every event is passed on untouched
- effective only while it runs, and no residue once it exits

## Requirements

- Windows 10 or 11, x64
- The tool and the game should run at the same integrity level. If the log shows
  `<access-denied>` in the process column, either run the game un-elevated or run the
  tool elevated so the two match.

The EXE is statically linked and depends only on OS libraries, so it needs no runtime
redistributable.

## Build

### MSVC (preferred)

Needs Visual Studio 2019 or newer with the "Desktop development with C++" workload, or
the standalone Build Tools, plus CMake 3.20+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Result: `build\bin\Release\RemoteMouseFix.exe`, with `config.json` copied next to it.

Visual Studio can also open the folder directly: it reads `CMakeLists.txt` and offers an
`x64-Release` configuration.

### Cross-compile from WSL or Linux (MinGW-w64)

Produces a genuine native Windows x64 PE executable, not a Linux binary. Useful when no
MSVC installation is available.

```bash
sudo apt install g++-mingw-w64-x86-64 cmake
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake
cmake --build build-mingw -j
```

Result: `build-mingw/bin/RemoteMouseFix.exe`.

If the toolchain is not installed system-wide, point the toolchain file at it:

```bash
cmake -S . -B build-mingw \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake \
  -DRMF_MINGW_PREFIX=$HOME/.local/opt/mingw-w64/usr
```

`CMakeLists.txt` refuses to configure for a non-Windows target, so a Linux compiler
cannot be used by accident.

## Configuration

`config.json` is read from the EXE's directory, or from a path given as the first
argument. Keys beginning with `_` are comments.

| Key | Default | Meaning |
| --- | --- | --- |
| `target_process` | `Wow.exe` | executable to observe, matched case-insensitively on the file name |
| `diagnostic_mode` | `true` | must stay `true`; this build has no correction path and refuses to start otherwise |
| `log_mouse_moves` | `true` | `false` records buttons, wheel and state only |
| `log_directory` | `logs` | relative paths resolve next to the EXE |
| `log_file_prefix` | `remotemousefix` | log file name prefix |
| `max_log_bytes` | `8388608` | rotate past this size |
| `max_log_files` | `5` | keep at most this many files |
| `flush_on_button` | `true` | flush after every button event |
| `jump_threshold_px` | `40` | tag a move as `JUMP` at or above this delta |
| `recenter_tolerance_px` | `3` | an injected move this close to the client centre is tagged `RECENTER` |
| `state_poll_interval_ms` | `10` | cursor visibility and `ClipCursor` sampling period |
| `log_state_changes` | `true` | write state transition lines |
| `console_status_interval_ms` | `2000` | live console counters; `0` disables |
| `heartbeat_interval_ms` | `10000` | periodic liveness line in the log; `0` disables |

## Usage

```
RemoteMouseFix.exe [path\to\config.json] [--seconds N]
```

`--seconds N` stops automatically after N seconds, which is the easiest way to take a
bounded capture. `--help` prints the usage.

Hotkeys while running:

| Hotkey | Effect |
| --- | --- |
| `Ctrl+Alt+M` | write a `## MARKER` line into the log |
| `Ctrl+Alt+P` | pause / resume capture |
| `Ctrl+Alt+Q` | quit cleanly (`Ctrl+C` and closing the window also work) |

These use `RegisterHotKey`, not a keyboard hook, so the tool can only ever observe those
three combinations.

## Test procedure: TeamViewer + WoW 3.3.5a

Do this from the remote side, over TeamViewer, exactly as when the problem occurs.

1. Copy `RemoteMouseFix.exe` and `config.json` into the same folder on the **game**
   machine. Confirm `target_process` matches the real executable name (`Wow.exe`).
2. Start WoW and log in to a character standing still in a quiet spot.
3. Start `RemoteMouseFix.exe`. The console shows `idle` until WoW is in front.
4. Click into the WoW window. The console status must switch to `ACTIVE`.
5. Run this sequence, slowly and deliberately, leaving about two seconds between steps:
   - press `Ctrl+Alt+M`, then click once on a **UI element** (an action bar button).
     This is the known-good case and gives the baseline.
   - press `Ctrl+Alt+M`, then click once into the **3D view** and do not move the mouse.
     This is the reported failure.
   - press `Ctrl+Alt+M`, then hold the **right** mouse button and turn the camera slowly
     left and right for a few seconds.
   - press `Ctrl+Alt+M`, then hold the **left** mouse button and drag slowly.
6. Press `Ctrl+Alt+Q`. The footer must report `queue drops 0`.
7. For comparison, if it is possible at all, repeat steps 4 to 6 **locally at the machine**
   with a physical mouse. A local log turns every finding into a difference rather than
   an absolute, which is far more conclusive.

Hand back the file from `logs\` (both files if a local comparison run was taken).

## Layout

```
RemoteMouseFix/
  CMakeLists.txt
  README.md
  LICENSE
  cmake/
    Version.h.in                     generated version header template
    toolchain-mingw-w64-x64.cmake    cross-compile toolchain
  config/
    config.json
  docs/
    diagnostics.md                   log format and reading procedure
  include/rmf/
    Config.h  EventQueue.h  EventRecord.h  Logger.h
    MouseHook.h  ProcessInfo.h  StateSampler.h  WinCompat.h
  src/
    main.cpp         startup, message loop, hotkeys, writer thread
    Config.cpp       dependency-free flat JSON reader
    EventRecord.cpp  log line formatting
    Logger.cpp       size-rotating text log
    MouseHook.cpp    the WH_MOUSE_LL hook
    ProcessInfo.cpp  PID to process name, with PID-reuse detection
    StateSampler.cpp cursor visibility, ClipCursor, geometry polling
    WinCompat.cpp    DPI awareness and version-dependent APIs
```

## Design notes

**The hook callback does almost nothing.** A `WH_MOUSE_LL` callback that blocks is
silently removed by Windows after `LowLevelHooksTimeout`. The callback therefore only
captures cheap data into a lock-free single-producer ring buffer. A writer thread does
the process lookups, the delta arithmetic and all file I/O, so disk access can never
delay an input event.

**Scoping is done by the state sampler, not by the hook.** The sampler publishes the
target PID in a single atomic, and only while the target owns the foreground window. The
hook's first action is to read that atomic, so input outside the target window is
discarded before it is ever copied anywhere.

**DPI awareness is a correctness requirement, not a nicety.** The hook reports physical
pixels. Without per-monitor awareness the window rectangles would be scaled differently
and the `cli=` and `ctr=` columns would be quietly wrong by the display's scale factor.

## Phase 2

Out of scope here, and it should not start before a log exists. The order matters: decide
what the data shows, then decide what to do about it.

## License

MIT, see [LICENSE](LICENSE).
