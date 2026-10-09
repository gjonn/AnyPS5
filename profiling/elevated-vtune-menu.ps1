param([string]$Tag = 'vtune-elevated-2', [int]$Seconds = 20)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell' }
if (Get-Process app -ErrorAction SilentlyContinue) { throw 'A game instance is already running; close it first' }
. (Join-Path $PSScriptRoot 'game-env.ps1')
$vtune = 'C:\Program Files (x86)\Intel\oneAPI\vtune\2026.4\bin64\vtune.exe'
$gameRoot = 'D:\ps5\gt7'
$repoRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $gameRoot "diagnostics-rapid\$Tag"
New-Item -ItemType Directory -Force -Path $output | Out-Null
Copy-Item "$repoRoot\build\core\libs\libs\*.prx" "$gameRoot\libs\" -Force
$log = Join-Path $output 'controller.log'
$note = { param($t) "$((Get-Date).ToString('HH:mm:ss')) $t" | Tee-Object -FilePath $log -Append }
$game = Start-Process -FilePath "$gameRoot\app.exe" -WorkingDirectory $gameRoot -RedirectStandardOutput "$gameRoot\run-out.log" -RedirectStandardError "$gameRoot\run-err.log" -PassThru
& $note "game pid $($game.Id)"
if (-not (Wait-MenuSettled -Game $game -Note $note)) { & $note 'menu did not settle'; exit 1 }
& $note "settled; title $((Get-Process -Id $game.Id).MainWindowTitle)"
& $vtune -collect hotspots -knob sampling-mode=sw -data-limit=0 -knob enable-stack-collection=true -target-pid $game.Id -duration $Seconds -result-dir (Join-Path $output 'hotspots') 2>&1 | Out-File (Join-Path $output 'hotspots-collect.txt')
& $note "hotspots done; title $((Get-Process -Id $game.Id).MainWindowTitle)"
& $vtune -collect threading -knob sampling-and-waits=sw -data-limit=0 -target-pid $game.Id -duration $Seconds -result-dir (Join-Path $output 'threading') 2>&1 | Out-File (Join-Path $output 'threading-collect.txt')
& $note "threading done; title $((Get-Process -Id $game.Id).MainWindowTitle)"
& $vtune -report hotspots -result-dir (Join-Path $output 'hotspots') -group-by thread,function -format csv -csv-delimiter comma -report-output (Join-Path $output 'hotspots-thread-function.csv') 2>&1 | Out-Null
& $vtune -report hotspots -result-dir (Join-Path $output 'hotspots') -group-by function -format csv -csv-delimiter comma -report-output (Join-Path $output 'hotspots-function.csv') 2>&1 | Out-Null
& $vtune -report hotspots -result-dir (Join-Path $output 'threading') -group-by thread,sync-object -format csv -csv-delimiter comma -report-output (Join-Path $output 'threading-thread-sync.csv') 2>&1 | Out-Null
& $vtune -report hotspots -result-dir (Join-Path $output 'threading') -group-by thread -format csv -csv-delimiter comma -report-output (Join-Path $output 'threading-thread.csv') 2>&1 | Out-Null
Get-Content "$gameRoot\run-err.log" -Tail 6000 | Set-Content (Join-Path $output 'settled-tail.log')
& $note 'done; game left running'
Write-Output "Done. Results in $output. Close the game window when finished."
