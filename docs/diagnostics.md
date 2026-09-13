# Log format and how to read it

RemoteMouseFix writes one plain-text, UTF-8, CRLF log per session under `logs/` next to
the EXE. Files are named `remotemousefix-YYYYMMDD-HHMMSS.log`, rotate at `max_log_bytes`
and are pruned to `max_log_files`. Pruning matches the `remotemousefix-*.log` prefix, so a
renamed file that still starts with it is pruned too: copy evidence elsewhere.

Every timestamp in the file is **local time**, the same zone as the Windows clock.

| Prefix | Meaning |
| --- | --- |
| `#` | session header, footer, rotation marker |
| `##` | state transition, correction change, heartbeat, operator marker |
| (timestamp) | one low-level mouse event |

## Session header

```
# RemoteMouseFix 0.2.0 - diagnostic log
# started        : 2026-09-12 20:50:01
# os             : Windows 10.0 build 26200
# dpi awareness  : per-monitor-v2
# elevated       : no
# config file    : C:\WOW\RemoteMouseFix\config-ab-test.json
# target process : Wow.exe
# log mouse moves: yes
# diagnostic mode: no
# correction     : permitted, start=absolute
# watchdog       : max_hidden=180000ms max_unechoed=50 max_output_failures=5
# thresholds     : jump>=40px  anchor<=3px  state_poll=10ms
# smoothing      : absolute only, interval=16ms max_step=20px
# heartbeat      : every 10000 ms (0 = off)
# rotation       : 8388608 bytes, keep 5 files
# virtual screen : 1920x1200 at (0,0), monitors=1
# MOUSECFG      : speed=10 threshold1=6 threshold2=10 acceleration=1
```

- **dpi awareness** must read `per-monitor-v2` (or `-v1`). The hook reports *physical*
  pixels and the `absolute` correction computes its targets in the same space. On a 125% display a
  DPI-virtualised process would be off by 25%.
- **elevated** matters twice: a lower-integrity tool cannot query the game, and Windows
  silently discards its synthetic input to a higher-integrity game (UIPI). Run the game
  and the tool at the same level.
- **correction** reads `permitted` or `LOCKED OFF (reason)`. It is locked while
  `diagnostic_mode = true`, and also when the emergency-off hotkey could not be registered.
- **MOUSECFG** records pointer speed and acceleration. The `relative` correction mode is
  subject to both; `absolute` is not.
- `# HOTKEY FAILED` and `# CONFIG WARNING` lines appear when applicable. A failed
  `Ctrl+Alt+C` is the reason for a locked correction.

## Event lines

```
20:50:04.118      3.117 #000000042 MOVE  scr=(+00948,+00672) d=(+0005,+0000) dt=  16ms cli=(+00700,+00500) ctr=(+00200,+00150) anc=(+00320,+00170) flg=I- cur=H xi=0x0000000000000000 DROP rd=(+5,+0) via=absolute pid=4242   Wow.exe
20:50:04.118      3.117 #--------- APPLY via=absolute rd=(+5,+0) before=(628,482) target=(633,482) after=(628,482) ok qpc_ms=81234567
20:50:04.119      3.118 #000000043 MOVE  scr=(+00633,+00482) d=(-0315,-0190) dt=   0ms cli=(+00385,+00330) ctr=(-00115,-00020) anc=(+00005,+00000) flg=I- cur=H xi=0x00000000524d4658 OWN pid=4242   Wow.exe
```

| Column | Source | Meaning |
| --- | --- | --- |
| `20:50:04.118` | QPC offset applied to the session start | wall clock, millisecond resolution |
| `3.117` | `QueryPerformanceCounter` | seconds since session start; the precise clock |
| `#000000042` | internal counter | sequence number. A gap means dropped events |
| `MOVE` | hook message | `MOVE`, `LDOWN`, `LUP`, `RDOWN`, `RUP`, `MDOWN`, `MUP`, `XDOWN`, `XUP`, `WHEEL`, `HWHEL` |
| `scr=(x,y)` | `MSLLHOOKSTRUCT.pt` | absolute position in physical screen pixels. For a `DROP` line this is where the remote client wanted the pointer; it never got there |
| `d=(dx,dy)` | derived | delta to the previous logged event's position |
| `dt=` | `MSLLHOOKSTRUCT.time` | milliseconds since the previous event. `-` on the first event |
| `cli=(x,y)` | derived | position inside the target client area |
| `ctr=(dx,dy)` | derived | offset from the client-area centre |
| `anc=(dx,dy)` | derived | offset from the **learned warp anchor**, only while the cursor is hidden and an anchor is known. This is the quantity the game reads as camera movement |
| `flg=` | `MSLLHOOKSTRUCT.flags` | `I` = `LLMHF_INJECTED`, `L` = `LLMHF_LOWER_IL_INJECTED`. `--` is physical input |
| `cur=` | `GetCursorInfo` in the hook | `H` = cursor hidden, the game holds the mouse. `-` = shown |
| `xi=` | `dwExtraInfo` | the injector's tag. `0x00000000524d4658` is this tool's own signature |
| `wheel=` / `xbtn=` | `mouseData` | wheel notches, or which extra button |
| `DROP rd=(dx,dy) via=mode` | correction | the event was **withheld** from the game; the remote pointer moved by `rd` and that delta was handed on via `mode`. A trailing `reset` means no reference position existed yet, so nothing was moved |
| `OWN` | `xi` signature | synthetic input produced by this tool. In `absolute` mode its `scr=` must equal the preceding `APPLY target=`; its `anc=` is what the game will read |
| `ANCHOR` | heuristic | a real, not withheld, move within `anchor_tolerance_px` of the anchor while hidden: the game's warp held |
| `JUMP` | heuristic | `\|dx\|` or `\|dy\|` reached `jump_threshold_px` |
| `pid=` / name | foreground window | always the target process |

`ANCHOR` and `JUMP` are grep handles, not conclusions. Versions up to 0.1.0 had a
`RECENTER` tag that assumed the client centre as warp target; the measurement of
2026-09-12 showed that assumption to be wrong, so it was replaced by the learned anchor.

## State lines

```
## STATE fg=0x00000000000605a2 pid=6180 Wow.exe | client=(320,75)-(1600,1099) 1280x1024 | dpi=120 | cursor_at=(968,58)
## STATE cursor=HIDDEN | cursor_at=(803,554)
## STATE cursor=SHOWN | cursor_at=(627,813)
## STATE clip=CONFINED (387,450)-(390,453) 3x3 | cursor_at=(388,451)
## STATE anchor-learned=(803,554) client_offset=(483,479) hits=37/80
## STATE focus-left-target -> pid=3424 explorer.exe
```

Sampled every `state_poll_interval_ms`; written only on change. None of it produces hook
events, which is why it is polled:

- `cursor=HIDDEN` / `SHOWN` bracket the phase in which the game holds the mouse. The
  game's cursor warp itself uses `SetCursorPos` and is **invisible** in the event stream;
  it shows up only as `cursor_at` here.
- `anchor-learned` is written when a hidden phase ends and one cursor position clearly
  dominated its samples (at least 5 hits and 20%). The anchor is stored as a client offset,
  so it follows the window when it moves.
- `clip=CONFINED` means `ClipCursor` is active. WoW 3.3.5a does not use it.
- `focus-left-target` names the new foreground process. No input from it is recorded.

## Apply lines

```
20:50:04.118      3.117 #--------- APPLY via=absolute rd=(+5,+0) before=(628,482) target=(633,482) after=(628,482) ok qpc_ms=81234567
```

One line per correction output, written by the message loop right after the `SendInput`
call. In `absolute` mode, `rd=` is one smoothed output step; the corresponding `DROP`
lines retain the original TeamViewer deltas. `before` and `after` are cursor positions read around the call; `after` usually still
equals `before`, because the input thread processes the event slightly later. `target` is
the intended landing position in `absolute` mode. `FAILED error=N` replaces `ok` when the
call failed. `qpc_ms` is the machine-wide QueryPerformanceCounter in milliseconds, so the
line can be aligned exactly with another process's trace, such as `MouseLookProbe`.

## Correction lines

```
## CORRECTION mode=absolute previous=off source=hotkey
## CORRECTION mode=off previous=absolute source=hotkey
## CORRECTION refused mode=relative: diagnostic_mode = true
## CORRECTION config correction_mode=relative ignored: diagnostic_mode = true
## CORRECTION DISABLED by watchdog: cursor hidden continuously for more than 180000 ms (was relative)
```

`source` is `config` (startup mode) or `hotkey`. Every watchdog trip names its reason:

`## SMOOTH RETARGET axis=x discarded=+120 incoming=-8` records that `absolute`
smoothing detected a direction reversal. The stale queued movement on that axis was
discarded so output could follow the new direction immediately. Opposing movement below
5 pixels is ignored for retargeting because the 0.5 trace showed one-pixel remote noise.

| Reason | Meaning |
| --- | --- |
| cursor hidden continuously | the hidden phase outlasted `watchdog_max_hidden_ms`; either a very long hold or a game state that hides the cursor for other reasons |
| consecutive `SendInput` failures | the output call itself failed `watchdog_max_output_failures` times in a row |
| own moves never reached the hook | more than `watchdog_max_unechoed` signed events are outstanding: synthetic input is discarded silently, typically because the game runs elevated |

## Heartbeat and marker lines

```
## HEARTBEAT target_foreground=yes fg_pid=6180 correction=absolute events=1452 injected=1430 suppressed=640 applied=598 own=598 anchor=0 jump=3 output_failures=0 watchdog=0 dropped=0 cursor=shown clip=released
## MARKER #1 at cursor=(388,451) cursor_visible=yes clip=released correction=absolute
## CAPTURE PAUSED by operator (correction unaffected)
```

The heartbeat makes an empty event section unambiguous: `target_foreground=no` throughout
means the target never matched. `correction=locked` means no mode could be enabled.

`## MARKER` is written by `Ctrl+Alt+M`; it records the correction mode, so a marker before
each test step documents which mode that step ran in. `Ctrl+Alt+P` pauses **logging only**.

## Session footer

```
# events logged   : 812
# injected        : 790
# at anchor       : 0
# jump tagged     : 3
# suppressed      : 640
# applied         : 598
# own seen        : 598
# output failures : 0
# watchdog trips  : 0
# hook events seen: 2004 (outside target 1192, queue drops 0)
# log rotations   : 0
```

**`queue drops` must be 0**; otherwise the trace has holes. `suppressed` greater than
`applied` is normal: a withheld event whose remote position did not change hands on no
delta. `own seen` should track `applied` closely in both correction modes.

## Reading a click into the 3D view

1. **Is the incoming stream injected?** `flg=I-` on the remote moves means absolute
   positioning via `SendInput`.
2. **Does `xi` carry a signature?** TeamViewer sets none (`0x0`).
3. **Is there a hidden phase?** Look for `cursor=HIDDEN` after the `LDOWN` and for an
   `anchor-learned` line when it ends.
4. **Does the warp hold?** With correction off over a remote client, `anc=` on the moves
   during the hidden phase is large and nothing is tagged `ANCHOR`: the remote position
   overwrites the warp. Locally, or with a working correction, the game-side offsets stay
   small.
5. **What is the timing?** Use the seconds column and `dt=`. Pauses of about 500 ms followed
   by a large `d=` are a property of the remote transport, not of the game.

## Reading a correction run

- During a hidden phase every remote move should be a `DROP` line. A remote move with
  `cur=H` that is **not** a `DROP` means the correction was off at that moment.
- `rd=` is what the game should receive per event. The sum of `rd` across a phase is the
  intended camera movement.
- A `DROP ... reset` at the start of a phase is expected once after a mode change or a
  focus change and moves nothing.
- Every applied delta produces an `APPLY` line followed by an `OWN` move. In `absolute`
  mode the `OWN` position must equal `target`. In `relative` mode the `OWN` move's offset
  from the anchor (`anc=`) shows what pointer speed and acceleration made of `rd`;
  comparing the two measures the acceleration distortion directly.
