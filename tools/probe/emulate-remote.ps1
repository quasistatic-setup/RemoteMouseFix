# Emulates a remote desktop client against MouseLookProbe: absolute SendInput moves with
# dwExtraInfo 0, exactly the input pattern measured from TeamViewer on 2026-09-12.
#
# Runs three scenarios and leaves two logs behind: logs-<Name>\ from RemoteMouseFix and
# probe-<Name>.log from the probe. Compare each probe PHASE sum with the expected movement:
#   click without motion (0,0) | steady right drag (+300,+0) | stall drag (+250,+0)
#
# Setup: put RemoteMouseFix.exe, MouseLookProbe.exe and this script into one folder and create
# cfg-<Name>.json there, a copy of config-ab-test.json with
#   "target_process": "MouseLookProbe.exe", "log_directory": "logs-<Name>",
# and the correction_mode / diagnostic_mode under test. Never use the folder that holds real
# captures: log rotation would delete them.
#
# It moves the real pointer and injects clicks, so run it on an otherwise idle desktop. It
# aborts before any button press if the probe is not the foreground window.
#
#   powershell -ExecutionPolicy Bypass -File emulate-remote.ps1 -Name absolute -HotkeysBefore 1 -OffThenRepeatClick
param(
  [Parameter(Mandatory)] [string] $Name,
  [string]   $Dir = $PSScriptRoot,
  [string[]] $HotkeysBefore = @(),   # e.g. '1' -> Ctrl+Alt+1 before the scenarios
  [switch]   $OffThenRepeatClick     # Ctrl+Alt+C after the scenarios, then click again
)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Emu {
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION u; }
  [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

  public static int SizeOfInput { get { return Marshal.SizeOf(typeof(INPUT)); } }
  static void Send(INPUT i) {
    if (SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT))) != 1)
      throw new Exception("SendInput failed, GLE=" + Marshal.GetLastWin32Error());
  }
  static void Mouse(uint flags, int dx, int dy) {
    INPUT i = new INPUT(); i.type = 0; i.u.mi.dx = dx; i.u.mi.dy = dy; i.u.mi.dwFlags = flags; Send(i);
  }
  public static void MoveAbs(int x, int y) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    Mouse(0x0001 | 0x8000, (int)(((long)x * 65535) / (sw - 1)), (int)(((long)y * 65535) / (sh - 1)));
  }
  public static void Nudge() { Mouse(0x0001, 0, 0); }
  public static void LDown() { Mouse(0x0002, 0, 0); }
  public static void LUp()   { Mouse(0x0004, 0, 0); }
  public static void RDown() { Mouse(0x0008, 0, 0); }
  public static void RUp()   { Mouse(0x0010, 0, 0); }
  static void Key(ushort vk, bool up) {
    INPUT i = new INPUT(); i.type = 1; i.u.ki.wVk = vk; i.u.ki.dwFlags = up ? 2u : 0u; Send(i);
  }
  public static void CtrlAlt(char c) {
    Key(0x11, false); Key(0x12, false); Key((ushort)c, false);
    Key((ushort)c, true); Key(0x12, true); Key(0x11, true);
  }
}
'@
[Emu]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
if ([Emu]::SizeOfInput -ne 40) { throw "sizeof(INPUT) = $([Emu]::SizeOfInput), expected 40" }

$dir = $Dir
$orig = New-Object Emu+POINT; [Emu]::GetCursorPos([ref]$orig) | Out-Null

# Tool first, probe last: the most recently launched window is the one Windows lets take
# the foreground.
$tool  = Start-Process -FilePath "$dir\RemoteMouseFix.exe" -ArgumentList "cfg-$Name.json",'--seconds','40' -WorkingDirectory $dir -WindowStyle Minimized -PassThru
Start-Sleep -Milliseconds 1500
$probe = Start-Process -FilePath "$dir\MouseLookProbe.exe" -ArgumentList '--log',"$dir\probe-$Name.log",'--seconds','45' -WorkingDirectory $dir -PassThru
$h = [IntPtr]::Zero
try {
for ($attempt = 0; $attempt -lt 20; $attempt++) {
  Start-Sleep -Milliseconds 250
  $probe.Refresh()
  $h = $probe.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { continue }
  # Being the source of the last input event lifts the foreground lock for this process.
  [Emu]::Nudge()
  [Emu]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 150
  if ([Emu]::GetForegroundWindow() -eq $h) { break }
}
if ($h -eq [IntPtr]::Zero -or [Emu]::GetForegroundWindow() -ne $h) { throw "probe did not come to the foreground" }
Start-Sleep -Milliseconds 700

function Assert-ToolAlive {
  # A Ctrl+Alt hotkey only stays inside the tool while the tool has it registered. If the tool
  # is gone the key combination reaches the focused application (Ctrl+Alt+Q is AltGr+Q = '@'
  # on a German layout), so never send one to a dead tool.
  if ($tool.HasExited) { throw "RemoteMouseFix exited early with code $($tool.ExitCode)" }
}
function Assert-Foreground {
  # The operator may be using this desktop. Never inject a button press unless the probe is
  # still the foreground window, or the click would land in someone else's application.
  if ([Emu]::GetForegroundWindow() -ne $h) { throw "focus left the probe (operator input?), aborting before injecting" }
}
function At([int]$cx, [int]$cy) { $p = New-Object Emu+POINT; $p.X = $cx; $p.Y = $cy; [Emu]::ClientToScreen($h, [ref]$p) | Out-Null; return $p }
function Hover([int]$cx, [int]$cy) {
  Assert-Foreground
  $p = At $cx $cy
  for ($i = 0; $i -lt 6; $i++) { [Emu]::MoveAbs($p.X, $p.Y); Start-Sleep -Milliseconds 16 }
  Start-Sleep -Milliseconds 300
}
function ClickNoMotion {
  # The reported symptom: one click into the 3D view without moving the mouse. The remote
  # client keeps re-sending the unchanged absolute position while the button is held.
  Hover 700 500
  $p = At 700 500
  Assert-Foreground; [Emu]::LDown(); Start-Sleep -Milliseconds 50
  [Emu]::MoveAbs($p.X, $p.Y); Start-Sleep -Milliseconds 30
  [Emu]::MoveAbs($p.X, $p.Y); Start-Sleep -Milliseconds 40
  [Emu]::LUp(); Start-Sleep -Milliseconds 500
}
function SteadyDrag {
  # Right-button camera turn: 60 remote moves of +5 px, about 60 Hz. Expected sum (+300,+0).
  Hover 250 450
  $p = At 250 450
  Assert-Foreground; [Emu]::RDown(); Start-Sleep -Milliseconds 60
  for ($i = 1; $i -le 60; $i++) { [Emu]::MoveAbs($p.X + 5 * $i, $p.Y); Start-Sleep -Milliseconds 16 }
  Start-Sleep -Milliseconds 50
  [Emu]::RUp(); Start-Sleep -Milliseconds 500
}
function StallDrag {
  # The measured ~500 ms stall followed by a catch-up jump. Expected sum (+250,+0) with one
  # 150 px tick that the correction cannot and should not hide.
  Hover 250 300
  $p = At 250 300
  Assert-Foreground; [Emu]::RDown(); Start-Sleep -Milliseconds 60
  for ($i = 1; $i -le 10; $i++) { [Emu]::MoveAbs($p.X + 5 * $i, $p.Y); Start-Sleep -Milliseconds 16 }
  Start-Sleep -Milliseconds 520
  [Emu]::MoveAbs($p.X + 200, $p.Y); Start-Sleep -Milliseconds 16
  for ($i = 1; $i -le 10; $i++) { [Emu]::MoveAbs($p.X + 200 + 5 * $i, $p.Y); Start-Sleep -Milliseconds 16 }
  Start-Sleep -Milliseconds 50
  [Emu]::RUp(); Start-Sleep -Milliseconds 500
}

foreach ($k in $HotkeysBefore) { Assert-ToolAlive; Assert-Foreground; [Emu]::CtrlAlt($k[0]); Start-Sleep -Milliseconds 300 }
ClickNoMotion
SteadyDrag
StallDrag
if ($OffThenRepeatClick) {
  Assert-ToolAlive; Assert-Foreground; [Emu]::CtrlAlt('C'); Start-Sleep -Milliseconds 300
  ClickNoMotion
}

} finally {
  # Always leave the desktop as it was: pointer back, probe closed, tool stopped. The tool is
  # asked to quit via its own hotkey so the footer gets written, and killed only as fallback.
  # Quit the tool while the probe is still in front, so even a leaked key lands there, and
  # only if the tool is actually alive and the probe really is the foreground window.
  if (-not $tool.HasExited -and $h -ne [IntPtr]::Zero -and [Emu]::GetForegroundWindow() -eq $h) {
    [Emu]::CtrlAlt('Q')
  }
  if (-not $tool.WaitForExit(10000)) { Stop-Process -Id $tool.Id -Force -ErrorAction SilentlyContinue }
  [Emu]::MoveAbs($orig.X, $orig.Y)
  Stop-Process -Id $probe.Id -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 300
  Start-Sleep -Milliseconds 300
}
Write-Output "run=$Name tool_exited=$($tool.HasExited) exit=$($tool.ExitCode)"
