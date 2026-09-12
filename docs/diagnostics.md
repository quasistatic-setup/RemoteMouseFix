# Log format and how to read it

RemoteMouseFix Phase 1 writes one plain-text, UTF-8, CRLF log per session under
`logs/` next to the EXE. Files are named `remotemousefix-YYYYMMDD-HHMMSS.log`,
rotate at `max_log_bytes` and are pruned to `max_log_files`.

Every timestamp in the file is **local time**, the same zone as the Windows clock.

There are four kinds of line:

| Prefix | Meaning |
| --- | --- |
| `#` | session header, footer, rotation marker |
| `##` | state transition, heartbeat, operator marker |
| (timestamp) | one low-level mouse event |
| everything else | nothing; the format has no other line kinds |

## Session header

```
# RemoteMouseFix 0.1.0 - Phase 1 diagnostic log
# started        : 2026-09-12 18:52:54
# os             : Windows 10.0 build 26200
# dpi awareness  : per-monitor-v2
# elevated       : no
# config file    : C:\Users\you\RemoteMouseFix\config.json
# target process : Wow.exe
# log mouse moves: yes
# thresholds     : jump>=40px  recenter<=3px  state_poll=10ms
# heartbeat      : every 10000 ms (0 = off)
# rotation       : 8388608 bytes, keep 5 files
# virtual screen : 1920x1200 at (0,0), monitors=1
# MOUSECFG      : speed=10 threshold1=6 threshold2=10 acceleration=1
```

Why these fields are in every log:

- **dpi awareness** must read `per-monitor-v2` (or `-v1`). The hook reports *physical*
  screen pixels. A DPI-virtualised process would compare them against scaled window
  rectangles and every `cli=` / `ctr=` column would be silently wrong. On a 125% display
  the difference is 25%, which is more than enough to fake or hide a "jump".
- **elevated** matters because a tool at a lower integrity level than the game can be
  blocked from querying it. If the process column shows `<access-denied>`, run the game
  un-elevated or the tool elevated so both sides match.
- **MOUSECFG** records pointer speed and the "enhance pointer precision" acceleration
  curve, since both change how a delta becomes cursor motion.

## Event lines

```
18:52:59.428      5.201 #000000001 MOVE  scr=(+00388,+00451) d=(+0000,+0000) dt= 218ms cli=(+00340,+00411) ctr=(+00000,+00000) flg=I- xi=0x0000000000000000 RECENTER pid=21792 Wow.exe
```

| Column | Source | Meaning |
| --- | --- | --- |
| `18:52:59.428` | QPC offset applied to the session start | wall clock, millisecond resolution |
| `5.201` | `QueryPerformanceCounter` | seconds since session start; use this for deltas, it is the precise clock |
| `#000000001` | internal counter | sequence number. A gap means events were dropped, which the footer also reports |
| `MOVE` | hook message | `MOVE`, `LDOWN`, `LUP`, `RDOWN`, `RUP`, `MDOWN`, `MUP`, `XDOWN`, `XUP`, `WHEEL`, `HWHEL` |
| `scr=(x,y)` | `MSLLHOOKSTRUCT.pt` | absolute position in physical screen pixels |
| `d=(dx,dy)` | derived | delta to the previous logged event's position |
| `dt=` | `MSLLHOOKSTRUCT.time` | milliseconds since the previous event, from the message tick count. `-` on the first event |
| `cli=(x,y)` | derived | position inside the target window's client area |
| `ctr=(dx,dy)` | derived | offset from the client-area centre. `(+00000,+00000)` means exactly centred |
| `flg=` | `MSLLHOOKSTRUCT.flags` | `I` = `LLMHF_INJECTED`, `L` = `LLMHF_LOWER_IL_INJECTED`, `-` = not set. `--` is a physical event |
| `xi=` | `MSLLHOOKSTRUCT.dwExtraInfo` | the 64-bit tag the injector may set. Non-zero values identify the injecting software |
| `wheel=` | `mouseData` high word | wheel notches, only on `WHEEL` / `HWHEL` |
| `xbtn=` | `mouseData` high word | which extra button, only on `XDOWN` / `XUP` |
| `RECENTER` | heuristic | an **injected move** landing within `recenter_tolerance_px` of the client centre |
| `JUMP` | heuristic | `\|dx\|` or `\|dy\|` reached `jump_threshold_px` |
| `pid=` / name | resolved from the foreground window | always the target process; the tool logs nothing else |

`RECENTER` is only ever applied to movement. A click that happens to land on the centre
is not a recenter, and tagging it would bury the real signal.

Both tags are **heuristics, deliberately named as guesses**. They are grep handles, not
conclusions. `flg`, `xi`, `d`, `dt` and `ctr` are the raw evidence.

## State lines

```
## STATE fg=0x00000000000605a2 pid=21792 Wow.exe | client=(48,40)-(728,862) 680x822 | cursor_at=(388,451)
## STATE cursor=HIDDEN | cursor_at=(388,451)
## STATE clip=CONFINED (387,450)-(390,453) 3x3 | cursor_at=(388,451)
## STATE clip=RELEASED | cursor_at=(388,451)
## STATE focus-left-target -> pid=3424 explorer.exe
```

A separate thread samples this every `state_poll_interval_ms` (default 10 ms) and writes
a line only when something changed. **None of it produces hook events**, which is why it
is polled rather than captured:

- `cursor=HIDDEN` — the game hid the pointer. WoW does this while it holds the mouse for
  camera control, so it brackets the camera-drag phase.
- `clip=CONFINED` — `ClipCursor` is active. A small rectangle around the window centre is
  the direct fingerprint of a capture-and-recenter loop. This is the strongest single
  piece of evidence for or against the recenter theory.
- `client=` — the client rectangle in screen coordinates. The anchor for `cli=` and `ctr=`.
- `focus-left-target` — something else took the foreground. This is logged even though
  the other process is not the target, because "the remote client's own window stole
  focus mid-drag" is a prime suspect. Only the handle, PID and process name are recorded;
  no input from any other application ever reaches the file.

## Heartbeat, marker and capture lines

```
## HEARTBEAT target_foreground=no fg_pid=3424 events=5 injected=5 recenter=2 jump=2 dropped=0 cursor=shown clip=released
## MARKER #1 at cursor=(388,451) cursor_visible=yes clip=released
## CAPTURE PAUSED by operator
```

The heartbeat exists so that an empty event section is unambiguous. `target_foreground=no`
throughout a session means the target name never matched or the window never came to the
front, which is a very different problem from "no events occurred".

`## MARKER` is written by Ctrl+Alt+M. Use it immediately before the action you want to
study, so the interesting moment can be found without reading the whole file.

## Session footer

```
# events logged  : 43
# injected       : 43
# recenter tagged: 10
# jump tagged    : 15
# hook events seen: 43 (fast-path skipped 0, queue drops 0)
# log rotations  : 0
```

`hook events seen` counts every event the callback saw, including those outside the
target window. `fast-path skipped` is that difference. **`queue drops` must be 0**; a
non-zero value means the writer could not keep up and the trace has holes.

## Reading a click into the 3D view

The question Phase 1 exists to answer. Filter the log around your marker and check, in
order:

1. **Is the incoming stream injected?** Look at `flg` on the moves arriving from the
   remote session. `I` means the remote client synthesises input via `SendInput`, so the
   game receives absolute positions rather than raw device deltas. `--` means the events
   arrive as physical device input and the theory needs revisiting.
2. **Does `xi` carry a signature?** A constant non-zero `dwExtraInfo` identifies the
   injector and would let a later phase recognise remote events specifically.
3. **What happens on `LDOWN` into the viewport?** Read the `d=` column on the next few
   moves. A large `d` within a few milliseconds of the click is the jump, in numbers.
4. **Is there a recenter?** Look for `RECENTER` moves and for `clip=CONFINED` /
   `cursor=HIDDEN` state lines around the click. Their presence and their timing
   relative to the click tell you whether the game warps the pointer, and whether the
   remote client then fights that warp by re-asserting an absolute position.
5. **What is the timing?** Use the `5.201`-style seconds column and `dt=`. A recenter
   followed within a frame or two by an absolute position from the remote client is the
   collision the theory predicts.

## What this build does not do

By design, so that the log is trustworthy evidence:

- no DLL injection, no driver, no code in the game process
- no `SendInput`, `mouse_event`, `SetCursorPos` or `ClipCursor`: it never writes input,
  so it cannot be the cause of anything it records
- no filtering, correction or suppression; the hook always calls `CallNextHookEx`
- no keyboard hook, so keystrokes and text content cannot be recorded. The three hotkeys
  use `RegisterHotKey`, which can only ever observe those three combinations
- no network access
- no admin rights, no installer, no change to the game

The tool has no effect once it exits.
