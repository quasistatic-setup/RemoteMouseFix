# RemoteMouseFix

A portable Windows tool against broken mouse and camera behaviour in 3D games driven over
remote desktop software. The first target case is **TeamViewer + World of Warcraft
3.3.5a**.

It records the low-level mouse stream as evidence, and it can optionally correct the
specific conflict that the evidence showed. The correction is off unless explicitly
enabled.

## The problem

Over TeamViewer, ordinary UI clicks and keyboard input in WoW work. A single click into the
3D view already makes the camera jump hard, and turning the camera produces further large
jumps.

The measurement of 2026-09-12 ([docs/findings-2026-09-12.md](docs/findings-2026-09-12.md))
established the mechanism:

1. While the camera is controlled with the mouse, WoW hides the cursor and keeps warping it
   back to a fixed anchor point. It reads the distance from that anchor as camera movement.
2. TeamViewer delivers every pointer update as an injected **absolute** position. That
   overwrites the warp: the cursor sits 140 to 410 px away from the anchor instead of 1 to
   3 px, and WoW turns the camera by that whole distance.
3. The remote stream is also coarser, with pauses of about 500 ms followed by a large
   catch-up.

## How the correction works

Only while a correction mode is on, the target window is in front and its cursor is hidden:

1. Every injected mouse move from another program is **withheld** from the game, so the
   game's warp stays intact.
2. The tool computes how far the **remote pointer itself** moved since its previous update.
3. It hands exactly that movement to the game as a signed `SendInput` event.

A click without mouse movement therefore reaches the game as zero movement, and a drag
reaches it as the real deltas. UI clicks are untouched, because the cursor is not hidden
then. Buttons, wheel and physical input are never withheld.

### Modes

| Mode | Hotkey | Output | Trade-off |
| --- | --- | --- | --- |
| `off` | `Ctrl+Alt+C` | none | observe only |
| `absolute` | `Ctrl+Alt+1` | absolute `SendInput` to cursor + delta | exact pixels, no pointer acceleration. Suits games that read the cursor position, like WoW 3.3.5a |
| `relative` | `Ctrl+Alt+2` | relative `SendInput` | also suits games that read Raw Input, but pointer speed and acceleration distort it, most strongly on large catch-up deltas |

A third variant based on `SetCursorPos` was built and measured against the test probe and
dropped: it lost about 40% of the movement, because `SetCursorPos` races with the input
thread that is still finishing the withheld event. `SendInput` is queued behind it and
processed in order.

The tool cannot reconstruct movement that TeamViewer has not delivered during a transport
pause. In `absolute` mode it spreads a late catch-up delta across bounded steps, which
keeps the camera moving but can add a small amount of latency.

### Safety

- The shipped `config.json` has `diagnostic_mode = true`: input is never modified and no
  mode can be enabled.
- Correction needs `diagnostic_mode = false` (see `config-ab-test.json`) and still starts
  in `correction_mode`, which defaults to `off`.
- `Ctrl+Alt+C` switches the correction off instantly. If that hotkey cannot be registered,
  correction stays locked, because the operator may be steering the machine through the
  remote client and must never lose the pointer.
- Releasing the mouse button, or any focus change, ends the intervention at once.
- Watchdogs switch the correction off and log the reason when the cursor stays hidden for
  too long, when output calls fail repeatedly, or when the tool's own signed events stop
  reaching the hook (typically a game running elevated).

## Constraints

- C++17, Win32 API, CMake, Windows 10/11, x64
- no DLL injection, no driver, no kernel component, no code in the game process
- no admin rights, no installer, no registry writes, no network access
- no change to the game or its files, no `ClipCursor`
- no keyboard hook: hotkeys use `RegisterHotKey` and can only observe their own combinations
- effective only while it runs, no residue once it exits

## Requirements

- Windows 10 or 11, x64
- The tool and the game must run at the same integrity level. Windows silently discards
  synthetic input to a more privileged window. If the log shows `<access-denied>` in the
  process column, run both un-elevated.

Both EXEs are statically linked and depend only on OS libraries.

## Build

### MSVC (preferred)

Visual Studio 2019 or newer with "Desktop development with C++", or the Build Tools, plus
CMake 3.20+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Result in `build\bin\Release\`: `RemoteMouseFix.exe`, `MouseLookProbe.exe`, `config.json`
and `config-ab-test.json`.

### Cross-compile from WSL or Linux (MinGW-w64)

Produces genuine native Windows x64 PE executables.

```bash
sudo apt install g++-mingw-w64-x86-64 cmake
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake
cmake --build build-mingw -j
```

For a toolchain that is not installed system-wide add
`-DRMF_MINGW_PREFIX=$HOME/.local/opt/mingw-w64/usr`. `-DRMF_BUILD_PROBE=OFF` skips the test
probe. `CMakeLists.txt` refuses a non-Windows target, so a Linux compiler cannot be used by
accident.

## Configuration

`config.json` is read from the EXE's directory, or from a path given as first argument. Keys
beginning with `_` are comments.

| Key | Default | Meaning |
| --- | --- | --- |
| `target_process` | `Wow.exe` | executable to observe, matched case-insensitively |
| `diagnostic_mode` | `true` | `true`: never modify input. `false`: correction modes available |
| `correction_mode` | `off` | mode at startup: `off`, `absolute` or `relative` |
| `watchdog_max_hidden_ms` | `180000` | hidden cursor longer than this switches correction off; `0` disables this check |
| `watchdog_max_unechoed` | `50` | own events allowed outstanding before the echo watchdog trips |
| `watchdog_max_output_failures` | `5` | consecutive failed `SendInput` calls before the watchdog trips |
| `log_mouse_moves` | `true` | `false` records buttons, wheel and state only |
| `log_directory` | `logs` | relative paths resolve next to the EXE |
| `log_file_prefix` | `remotemousefix` | log file name prefix |
| `max_log_bytes` | `8388608` | rotate past this size |
| `max_log_files` | `5` | keep at most this many files |
| `flush_on_button` | `true` | flush after every button event |
| `jump_threshold_px` | `40` | tag a move as `JUMP` at or above this delta |
| `anchor_tolerance_px` | `3` | a real move this close to the learned anchor is tagged `ANCHOR` |
| `state_poll_interval_ms` | `10` | sampling period for cursor state and anchor learning |
| `log_state_changes` | `true` | write state transition lines |
| `console_status_interval_ms` | `2000` | live console counters; `0` disables |
| `heartbeat_interval_ms` | `10000` | periodic liveness line in the log; `0` disables |

## Usage

```
RemoteMouseFix.exe [path\to\config.json] [--seconds N]
```

| Hotkey | Effect |
| --- | --- |
| `Ctrl+Alt+M` | write a `## MARKER` line, including the current correction mode |
| `Ctrl+Alt+P` | pause / resume logging (the correction keeps running) |
| `Ctrl+Alt+Q` | quit cleanly (`Ctrl+C` and closing the window also work) |
| `Ctrl+Alt+C` | correction off |
| `Ctrl+Alt+1` | correction `absolute` |
| `Ctrl+Alt+2` | correction `relative` |

`absolute` smooths batched TeamViewer movement only while the game cursor is hidden. It
emits at most 20 pixels per axis every 16 ms. Pending movement is discarded on release,
focus loss or correction shutdown. When new remote movement reverses an axis, stale
movement still queued on that axis is discarded so the camera follows the new direction
without elastic pullback. `relative` remains unsmoothed for direct comparison.
Opposing deltas below 5 pixels are treated as remote-input noise rather than a direction
change. The warp anchor becomes available as soon as five stable samples establish it,
including during the first camera hold.

Log files rotate and are pruned by prefix, so copy a log you want to keep out of `logs\`
before taking further captures.

## Test procedure: correction A/B over TeamViewer

Do this from the remote side, over TeamViewer, exactly as when the problem occurs.

1. Put `RemoteMouseFix.exe`, `config-ab-test.json` and `Start-AB-Test.cmd` into one folder
   on the game machine.
2. Start WoW and park a character in a quiet spot.
3. Start `Start-AB-Test.cmd`. The console must show `run profile: A/B CORRECTION TEST`,
   the path to `config-ab-test.json`, and `correction: absolute`. Click into WoW; the
   status must switch to `ACTIVE` with `corr=absolute`.
4. Run the same three steps once per mode, in the order `absolute`, `off`, `relative`.
   Switch with the hotkey first (`Ctrl+Alt+C`, `Ctrl+Alt+1`, `Ctrl+Alt+2`); the console
   confirms each switch.
   - `Ctrl+Alt+M`, then one click into the **3D view** without moving the mouse.
   - `Ctrl+Alt+M`, then hold the **right** button and turn the camera slowly left and right.
   - `Ctrl+Alt+M`, then hold the **left** button and drag slowly.
   Note for each mode whether it feels smooth, jumps, or turns too fast or too slow.
5. `Ctrl+Alt+C`, then `Ctrl+Alt+Q`. The footer must report `queue drops 0` and
   `watchdog trips 0`.
6. Copy the log out of `logs\` and hand it back together with your notes per mode.

`Ctrl+Alt+C` works at any time. With absolute remote input a single drag can only turn as
far as the remote pointer can travel on the controlling screen; release and grab again to
continue turning.

## Test procedure: diagnostic capture only

With the shipped `config.json`, `RemoteMouseFix.exe` records without changing anything. Use
`Ctrl+Alt+M` before each action, then compare a remote run with a local run on the same
machine. [docs/diagnostics.md](docs/diagnostics.md) explains every column and how to read a
click into the 3D view.

## Testing without the game: MouseLookProbe

`MouseLookProbe.exe` is a small window that reproduces the measured mouse-look mechanism:
it hides the cursor on a button press, warps it to a fixed off-centre anchor, reads the
distance on a timer and warps back. It writes each phase's summed movement to
`mouselook-probe.log`, which makes a correction's effect measurable without WoW.

[tools/probe/emulate-remote.ps1](tools/probe/emulate-remote.ps1) automates this: it starts
both programs, drives the probe with absolute injected input shaped like the measured
TeamViewer stream, and runs a click without motion, a steady drag and a stalled drag. Its
header describes the setup. A click without movement must sum to zero in the probe log, and
a drag of known length must sum to that length.

The probe reads the cursor position, as WoW does. A game reading Raw Input behaves
differently, so a pass against the probe proves the correction logic, not compatibility
with any particular game.

## Layout

```
RemoteMouseFix/
  CMakeLists.txt  README.md  LICENSE  AGENTS.md
  cmake/
    Version.h.in                     generated version header template
    toolchain-mingw-w64-x64.cmake    cross-compile toolchain
  config/
    config.json                      observe only (diagnostic_mode = true)
    config-ab-test.json              absolute correction, full diagnostic logging
    Start-AB-Test.cmd                unambiguous launcher for the correction A/B test
    config-play.json                 absolute correction, compact gameplay logging
    Start-Playing.cmd                launcher for normal corrected gameplay
  docs/
    diagnostics.md                   log format and reading procedures
    findings-2026-09-12.md           dated measurement report
    doc-manifest.json
  include/rmf/                       headers
  src/
    main.cpp          startup, message loop, hotkeys, correction output, writer thread
    Correction.cpp    correction modes, SendInput output, anchor learning
    MouseHook.cpp     the WH_MOUSE_LL hook and the withhold decision
    Config.cpp        dependency-free flat JSON reader
    EventRecord.cpp   log line formatting
    Logger.cpp        size-rotating text log
    ProcessInfo.cpp   PID to process name, with PID-reuse detection
    StateSampler.cpp  cursor visibility, ClipCursor, geometry polling
    WinCompat.cpp     DPI awareness and version-dependent APIs
  tools/probe/
    MouseLookProbe.cpp               stand-in game for testing the correction
    emulate-remote.ps1               remote-client emulator and test scenarios
```

## Design notes

**The hook callback stays minimal.** A `WH_MOUSE_LL` callback that blocks is silently
removed by Windows. The callback captures cheap data into a lock-free single-producer ring,
decides whether to withhold, and posts the replacement movement to the message loop. File
I/O happens on a writer thread; `SendInput` happens after the callback has returned.

**The event ring lives on the heap.** It holds 16,384 events and is several megabytes large,
more than a default main-thread stack.

**Scoping is done by the state sampler.** It publishes the target PID in an atomic only while
the target owns the foreground, so input outside the target never enters the tool's memory.

**The warp anchor is learned, not assumed.** The client centre is not WoW's anchor. The
anchor is taken from cursor positions sampled while the cursor is hidden and stored as a
client offset.

**DPI awareness is a correctness requirement.** The hook reports physical pixels and absolute
targets are computed in the same space.

## License

MIT, see [LICENSE](LICENSE).
