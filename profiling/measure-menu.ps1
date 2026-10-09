param([string]$Tag = 'measure', [int]$Windows = 6, [string[]]$ExtraEnv = @(), [switch]$KeepRunning)
$ErrorActionPreference = 'Stop'
if (Get-Process app -ErrorAction SilentlyContinue) { throw 'A game instance is already running; close it first' }
. (Join-Path $PSScriptRoot 'game-env.ps1')
foreach ($pair in $ExtraEnv) { $name, $value = $pair -split '=', 2; Set-Item "Env:\$name" $value }
$gameRoot = 'D:\ps5\gt7'
$repoRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $gameRoot "diagnostics-rapid\$Tag"
New-Item -ItemType Directory -Force -Path $output | Out-Null
Copy-Item "$repoRoot\build\core\libs\libs\*.prx" "$gameRoot\libs\" -Force
$log = Join-Path $output 'controller.log'
$note = { param($t) "$((Get-Date).ToString('HH:mm:ss')) $t" | Tee-Object -FilePath $log -Append }
& $note "env: $($ExtraEnv -join ' ')"
$game = Start-Process -FilePath "$gameRoot\app.exe" -WorkingDirectory $gameRoot -WindowStyle Normal -RedirectStandardOutput "$gameRoot\run-out.log" -RedirectStandardError "$gameRoot\run-err.log" -PassThru
& $note "game pid $($game.Id) started $($game.StartTime.ToString('HH:mm:ss'))"
$settled = Wait-MenuSettled -Game $game -Note $note -QuietWindows 4
if (-not $settled) { & $note "menu did not settle (exited=$($game.HasExited))"; Get-Content "$gameRoot\run-err.log" -Tail 4000 | Set-Content (Join-Path $output 'tail.log'); exit 1 }
$seen = @{}
$counts = @()
$deadline = (Get-Date).AddSeconds(15 * $Windows + 30)
while ($counts.Count -lt $Windows -and (Get-Date) -lt $deadline -and -not $game.HasExited) {
    Start-Sleep 3
    $w = Get-MenuWindow
    if ($w.presents -ge 0 -and -not $seen.ContainsKey($w.key)) { $seen[$w.key] = 1; $counts += $w.presents; & $note "window presents=$($w.presents)" }
}
$tail = Get-Content "$gameRoot\run-err.log" -Tail 6000
$tail | Set-Content (Join-Path $output 'tail.log')
$pipelined = $tail | Where-Object { $_ -match '^\[draw\] pipelined' } | Select-Object -Last 1
$draws = $tail | Where-Object { $_ -match '^\[draws\] ' } | Select-Object -Last 1
$commit = if ($pipelined -match '(\d+) commits at ([\d.]+) us each') { "commits $($Matches[1]) at $($Matches[2]) us" } else { 'commits ?' }
$room = if ($pipelined -match 'waiting ([\d.]+) us per draw for room') { "room wait $($Matches[1]) us" } else { '' }
$kinds = if ($draws -match 'per kind \(avg us, count\): ([^;]+);') { $Matches[1] } else { '' }
$avg = if ($counts.Count) { [math]::Round(($counts | Measure-Object -Average).Average / 10, 2) } else { 0 }
& $note "RESULT fps=$avg windows=$($counts -join ',') | $commit | $room | $kinds"
if (-not $KeepRunning -and -not $game.HasExited) { Stop-Process -Id $game.Id -Force; & $note "stopped game pid $($game.Id)" }
