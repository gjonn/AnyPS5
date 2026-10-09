param([string]$Tag = 'vtune-settled-2', [int]$Seconds = 15)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell' }
$game = @(Get-Process app -ErrorAction SilentlyContinue)
if ($game.Count -ne 1) { throw "Expected exactly one running app.exe, found $($game.Count)" }
$game = $game[0]
$vtune = 'C:\Program Files (x86)\Intel\oneAPI\vtune\2026.4\bin64\vtune.exe'
$output = "D:\ps5\gt7\diagnostics-rapid\$Tag"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$log = Join-Path $output 'controller.log'
function Note([string]$t) { "$((Get-Date).ToString('HH:mm:ss')) $t" | Tee-Object -FilePath $log -Append }
Note "pid $($game.Id) title $($game.MainWindowTitle)"
Get-Content 'D:\ps5\gt7\run-err.log' -Tail 3000 | Where-Object { $_ -match '^\[present\] \d+|^\[pipecache\]' } | Select-Object -Last 4 | Out-File -Append $log
& $vtune -collect hotspots -knob sampling-mode=sw -data-limit=0 -knob enable-stack-collection=true -target-pid $game.Id -duration $Seconds -result-dir (Join-Path $output 'hotspots') 2>&1 | Out-File (Join-Path $output 'hotspots-collect.txt')
Note "hotspots done; title $((Get-Process -Id $game.Id).MainWindowTitle)"
& $vtune -collect threading -knob sampling-and-waits=sw -data-limit=0 -target-pid $game.Id -duration $Seconds -result-dir (Join-Path $output 'threading') 2>&1 | Out-File (Join-Path $output 'threading-collect.txt')
Note "threading done; title $((Get-Process -Id $game.Id).MainWindowTitle)"
Get-Content 'D:\ps5\gt7\run-err.log' -Tail 3000 | Where-Object { $_ -match '^\[present\] \d+|^\[pipecache\]' } | Select-Object -Last 4 | Out-File -Append $log
$names = foreach ($t in $game.Threads) { try { $h = [IntPtr]::Zero; [pscustomobject]@{ Tid = $t.Id } } catch {} }
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Tn {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint a, bool i, uint t);
  [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h, out IntPtr n);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  public static string Name(uint tid) { IntPtr h = OpenThread(0x1000, false, tid); if (h == IntPtr.Zero) return ""; IntPtr p; string s = ""; if (GetThreadDescription(h, out p) >= 0 && p != IntPtr.Zero) s = Marshal.PtrToStringUni(p); CloseHandle(h); return s; }
}
'@ -ErrorAction SilentlyContinue
$game.Refresh()
$game.Threads | ForEach-Object { [pscustomobject]@{ Tid = $_.Id; Name = [Tn]::Name([uint32]$_.Id); CpuMs = [math]::Round($_.TotalProcessorTime.TotalMilliseconds) } } | Export-Csv (Join-Path $output 'thread-names.csv') -NoTypeInformation
Note 'done; game left running'
