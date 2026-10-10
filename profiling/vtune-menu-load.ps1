param([string]$Tag = 'vtune-load-1', [int]$Seconds = 80)
$ErrorActionPreference = 'Stop'
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
$game = Start-Process -FilePath "$gameRoot\app.exe" -WorkingDirectory $gameRoot -WindowStyle Normal -RedirectStandardOutput "$gameRoot\run-out.log" -RedirectStandardError "$gameRoot\run-err.log" -PassThru
& $note "game pid $($game.Id)"
$loading = $false; $last = ''
while (-not $game.HasExited) {
    Start-Sleep 2
    $w = Get-MenuWindow
    if ($w.key -eq $last) { continue }
    $last = $w.key
    & $note "window presents=$($w.presents) misses=$($w.misses)"
    if ($w.presents -ge 450) { $loading = $true; continue }
    if ($loading -and $w.presents -ge 0 -and $w.presents -lt 300) { break }
}
& $note "menu-load stretch: attaching for $Seconds s"
& $vtune -collect hotspots -knob sampling-mode=sw -data-limit=0 -knob enable-stack-collection=true -target-pid $game.Id -duration $Seconds -result-dir (Join-Path $output 'hotspots') 2>&1 | Out-File (Join-Path $output 'collect.txt')
& $note "collected"
Get-Content "$gameRoot\run-err.log" -Tail 8000 | Set-Content (Join-Path $output 'tail.log')
if (-not $game.HasExited) { Stop-Process -Id $game.Id -Force; & $note "stopped game pid $($game.Id)" }
& $vtune -report hotspots -result-dir (Join-Path $output 'hotspots') -group-by thread,function -format csv -csv-delimiter comma -report-output (Join-Path $output 'thread-function.csv') 2>&1 | Out-Null
& $note 'done'
