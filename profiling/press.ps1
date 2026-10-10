param([Parameter(Mandatory)][string]$Tag, [Parameter(Mandatory)][ValidateSet('ENTER','UP','DOWN','LEFT','RIGHT','ESCAPE','C')][string[]]$Keys, [int]$HoldMs = 400, [int]$GapMs = 700)
$ErrorActionPreference = 'Stop'
$identity = [System.Text.Json.JsonDocument]::Parse([System.IO.File]::ReadAllText("D:\ps5\gt7\diagnostics-rapid\$Tag\process.json")).RootElement
$processId = $identity.GetProperty('pid').GetInt32()
$process = Get-Process -Id $processId -ErrorAction Stop
if ($process.Path -ne $identity.GetProperty('path').GetString() -or $process.StartTime.ToUniversalTime().ToString('o') -ne $identity.GetProperty('started').GetString()) { throw "PID $processId does not match $Tag" }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GameInput {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr handle);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
'@
$codes = @{ ENTER = 0x0D; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; ESCAPE = 0x1B; C = 0x43 }
$extended = @{ UP = $true; DOWN = $true; LEFT = $true; RIGHT = $true }
$handle = $process.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) { throw "PID $processId has no main window" }
[void][GameInput]::ShowWindow($handle, 9)
[GameInput]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[GameInput]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[void][GameInput]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds 400
if ([GameInput]::GetForegroundWindow() -ne $handle) { throw "could not bring PID $processId to the foreground" }
foreach ($key in $Keys) {
    $code = [byte]$codes[$key]
    $scan = [byte][GameInput]::MapVirtualKey($code, 0)
    $flag = if ($extended[$key]) { 1 } else { 0 }
    [GameInput]::keybd_event($code, $scan, [uint32]$flag, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [GameInput]::keybd_event($code, $scan, [uint32]($flag -bor 2), [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $GapMs
}
Write-Output "sent $($Keys -join ' ') to PID $processId"
