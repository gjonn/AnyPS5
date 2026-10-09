param([string]$Tag = 'nsys-elevated-3', [int]$TransitionSeconds = 20, [int]$SettledSeconds = 15, [int]$TimeoutSeconds = 1500, [switch]$SettledOnly, [switch]$NoSampling)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $NoSampling -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell' }
if (Get-Process app -ErrorAction SilentlyContinue) { throw 'A game instance is already running; close it first' }
$nsys = 'C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1\target-windows-x64\nsys.exe'
$gameRoot = 'D:\ps5\gt7'
$repoRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $gameRoot "diagnostics-rapid\$Tag"
New-Item -ItemType Directory -Force -Path $output | Out-Null
Copy-Item "$repoRoot\build\core\libs\libs\*.prx" "$gameRoot\libs\" -Force
Get-FileHash "$gameRoot\libs\*.prx" | Export-Csv (Join-Path $output 'modules.csv') -NoTypeInformation
$env:PATH = "C:\tools\mingw64\bin;$env:PATH"
$env:APS5_NO_SNAPSHOT_CHECK = '1'
$env:APS5_WRITE_WATCH_IMPORTS = 'watch'
$env:APS5_PROFILE_DRAW = '1'
$env:APS5_STAGING_POOL_MIB = '2048'
$env:APS5_PIPELINE_ORDERED_LABELS = '1'
$env:APS5_MERGE_DRAW_BARRIERS = '1'
$env:APS5_BDA_TABLE_DEVICE_LOCAL = '0'
$env:APS5_SHADER_DATA_DEVICE_LOCAL = '0'
$env:APS5_TRACE_ALIAS_FLUSH = '1'
foreach ($flag in 'APS5_TRACE_WAIT_TIMELINE','APS5_TRACE_EQUEUE_EVENTS','APS5_TRACE_GPU','APS5_TRACE_COND_CALLER','APS5_PROFILE_GPU','APS5_DRAW_INPUT_MEMO','APS5_PERF_CONTROL') { Remove-Item "Env:\$flag" -ErrorAction SilentlyContinue }
$log = Join-Path $output 'controller.log'
function Note([string]$text) { $line = "$((Get-Date).ToString('HH:mm:ss')) $text"; $line | Tee-Object -FilePath $log -Append }
Set-Location $gameRoot
$session = 'gt7menu' + (Get-Date).ToString('HHmmss')
$launch = Start-Process -FilePath $nsys -ArgumentList @('launch', "--session-new=$session", '--trace=vulkan', '--vulkan-gpu-workload=individual', '--wait=all', "$env:ComSpec", '/c', "`"$gameRoot\app.exe 1>$gameRoot\run-out.log 2>$gameRoot\run-err.log`"") -WorkingDirectory $gameRoot -RedirectStandardOutput (Join-Path $output 'nsys-launch-out.txt') -RedirectStandardError (Join-Path $output 'nsys-launch-err.txt') -PassThru
Note "nsys launch pid $($launch.Id)"
$game = $null
for ($i = 0; $i -lt 60 -and -not $game; $i++) { Start-Sleep 1; $game = Get-Process app -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $game) { throw 'app.exe did not start under nsys' }
Note "game pid $($game.Id)"
$startArgs = if ($NoSampling) { @('--sample=none', '--cpuctxsw=none', '--force-overwrite=true', '--export=sqlite') } else { @('--sample=process-tree', '--sampling-frequency=2000', '--backtrace=auto', '--cpuctxsw=process-tree', '--force-overwrite=true', '--export=sqlite') }
function Capture([string]$name, [int]$seconds) {
    $report = Join-Path $output $name
    Note "capture $name start (title $((Get-Process -Id $game.Id -ErrorAction SilentlyContinue | ForEach-Object MainWindowTitle)))"
    $proc = Start-Process -FilePath $nsys -ArgumentList (@('start', "--session=$session") + $startArgs + @("--output=$report")) -RedirectStandardOutput (Join-Path $output "$name-start-out.txt") -RedirectStandardError (Join-Path $output "$name-start-err.txt") -PassThru -WindowStyle Hidden
    if (-not $proc.WaitForExit(60000)) { Note "capture ${name}: nsys start did not return in 60 s" } else { Note "capture ${name}: nsys start exit $($proc.ExitCode)" }
    Start-Sleep -Seconds $seconds
    $stopProc = Start-Process -FilePath $nsys -ArgumentList @('stop', "--session=$session") -RedirectStandardOutput (Join-Path $output "$name-stop-out.txt") -RedirectStandardError (Join-Path $output "$name-stop-err.txt") -PassThru -WindowStyle Hidden
    if (-not $stopProc.WaitForExit(600000)) { Note "capture ${name}: nsys stop did not return in 10 min" } else { Note "capture ${name}: nsys stop exit $($stopProc.ExitCode)" }
    Note "capture $name stop (title $((Get-Process -Id $game.Id -ErrorAction SilentlyContinue | ForEach-Object MainWindowTitle)))"
    Get-Content "$gameRoot\run-err.log" -Tail 6000 | Set-Content (Join-Path $output "$name-tail.log")
}
$script:titleSample = $null
function Windows() {
    if ((Get-Item "$gameRoot\run-err.log" -ErrorAction SilentlyContinue).Length -eq 0) {
        $title = (Get-Process -Id $game.Id -ErrorAction SilentlyContinue | ForEach-Object MainWindowTitle)
        $frame = if ($title -match '\((\d+)\)') { [int]$Matches[1] } else { -1 }
        $now = Get-Date
        if ($frame -lt 0) { return [pscustomobject]@{ presents = -1; misses = -1; key = 'no-title' } }
        if ($null -eq $script:titleSample) { $script:titleSample = @{ time = $now; frame = $frame }; return [pscustomobject]@{ presents = -1; misses = -1; key = 'title-start' } }
        $elapsed = ($now - $script:titleSample.time).TotalSeconds
        if ($elapsed -lt 10) { return [pscustomobject]@{ presents = -1; misses = -1; key = "title-wait-$($script:titleSample.frame)" } }
        $presents = [int](($frame - $script:titleSample.frame) * 10 / $elapsed)
        $script:titleSample = @{ time = $now; frame = $frame }
        return [pscustomobject]@{ presents = $presents; misses = 0; key = "title-$frame" }
    }
    $tail = Get-Content "$gameRoot\run-err.log" -Tail 4000
    $present = $tail | Where-Object { $_ -match '^\[present\] (\d+) presents' } | Select-Object -Last 1
    $pipe = $tail | Where-Object { $_ -match '^\[pipecache\]' } | Select-Object -Last 1
    [pscustomobject]@{ presents = if ($present -match '^\[present\] (\d+) presents') { [int]$Matches[1] } else { -1 }; misses = if ($pipe -match '(\d+) misses') { [int]$Matches[1] } else { -1 }; key = "$present|$pipe" }
}
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$phase = 'loading'
$lastKey = ''
$streak = 0
$loadingSeen = $false
while ((Get-Date) -lt $deadline -and -not $game.HasExited) {
    Start-Sleep 2
    $w = Windows
    if ($w.key -eq $lastKey) { continue }
    $lastKey = $w.key
    Note "window presents=$($w.presents) misses=$($w.misses) phase=$phase"
    if ($phase -eq 'loading') {
        if ($w.presents -ge 450) { $loadingSeen = $true }
        if ($loadingSeen -and $w.presents -ge 0 -and $w.presents -lt 300) { if (-not $SettledOnly) { Capture 'transition' $TransitionSeconds }; $phase = 'settling' }
    } elseif ($phase -eq 'settling') {
        if ($w.presents -ge 20 -and $w.presents -lt 300 -and $w.misses -eq 0) { $streak++ } else { $streak = 0 }
        if ($streak -ge $(if ($w.key -like "title-*") { 4 } else { 2 })) { Capture 'settled' $SettledSeconds; $phase = 'done'; break }
    }
}
Note "controller finished phase=$phase; game left running for inspection"
Write-Output "Done. Reports in $output. Close the game window when finished."
