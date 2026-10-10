param([ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag = 'boot', [ValidateRange(10,7200)][int]$Seconds = 480, [string[]]$ExtraEnv = @(), [ValidateSet(-1,0,1)][int]$InPlaceDrawInputs = -1)
$ErrorActionPreference = 'Stop'
if (Get-Process app -ErrorAction SilentlyContinue) { throw 'A game instance is already running; close it first' }
. (Join-Path $PSScriptRoot 'game-env.ps1')
Remove-Item Env:\APS5_NO_SPECIALIZED_MODULE_CACHE -ErrorAction SilentlyContinue
foreach ($pair in $ExtraEnv) { $name, $value = $pair -split '=', 2; Set-Item "Env:\$name" $value }
$gameRoot = 'D:\ps5\gt7'
$repoRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $gameRoot "diagnostics-rapid\$Tag"
if (Test-Path -LiteralPath $output) { throw "Evidence directory already exists: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
if ($InPlaceDrawInputs -ge 0) {
    $env:APS5_PERF_CONTROL = Join-Path $output 'performance-controls.txt'
    "merge_draw_barriers=1`nbda_table_device_local=0`nin_place_draw_inputs=$InPlaceDrawInputs`n" | Set-Content -LiteralPath $env:APS5_PERF_CONTROL -Encoding ascii
}
$sources = Get-ChildItem -LiteralPath "$repoRoot\core\shader\recompiler" -File -Recurse | Sort-Object FullName | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash
$sources | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'source-hashes.json')
$binaries = Get-ChildItem -LiteralPath "$repoRoot\build\core\libs\libs" -Filter '*.prx' -File | Sort-Object Name | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash
$binaries | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'binary-hashes.json')
Get-ChildItem Env: | Where-Object Name -like 'APS5_*' | Select-Object Name,Value | Sort-Object Name | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'environment.json')
Copy-Item "$repoRoot\build\core\libs\libs\*.prx" "$gameRoot\libs\" -Force
$game = Start-Process -FilePath "$gameRoot\app.exe" -WorkingDirectory $gameRoot -WindowStyle Normal -RedirectStandardOutput "$gameRoot\run-out.log" -RedirectStandardError "$gameRoot\run-err.log" -PassThru
$identity = [pscustomobject]@{ pid=$game.Id; path=$game.Path; started=$game.StartTime.ToUniversalTime().ToString('o') }
$identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'process.json')
$started = Get-Date
"started $($started.ToString('HH:mm:ss')) pid $($game.Id) env: $($ExtraEnv -join ' ')" | Set-Content (Join-Path $output 'controller.log')
Write-Output "Started $Tag PID $($game.Id) at $($identity.started); duration $Seconds seconds"
$firstPresent = $null
$logPosition = 0L
$logRemainder = ''
while (((Get-Date) - $started).TotalSeconds -lt $Seconds -and -not $game.HasExited) {
    Start-Sleep 2
    if ($game.HasExited) { break }
    $game.Refresh()
    $elapsed = ((Get-Date) - $started).TotalSeconds
    [pscustomobject]@{elapsedSeconds=$elapsed;utc=[datetime]::UtcNow.ToString('o');title=$game.MainWindowTitle;cpuSeconds=$game.TotalProcessorTime.TotalSeconds} | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $output 'samples.jsonl')
    $stream = [System.IO.File]::Open("$gameRoot\run-err.log",[System.IO.FileMode]::Open,[System.IO.FileAccess]::Read,[System.IO.FileShare]::ReadWrite)
    try {
        [void]$stream.Seek($logPosition,[System.IO.SeekOrigin]::Begin)
        $reader = [System.IO.StreamReader]::new($stream)
        $chunk = $reader.ReadToEnd()
        $logPosition = $stream.Position
        $parts = ($logRemainder + $chunk) -split "`n"
        $logRemainder = $parts[-1]
        foreach ($line in ($parts | Select-Object -SkipLast 1)) {
            if ($line -match '^\[(present|shader-disk-cache|pipecache|perf-controls|draws|draw-pipeline)\]') {
                [pscustomobject]@{elapsedSeconds=$elapsed;line=$line.TrimEnd()} | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $output 'events.jsonl')
            }
        }
    } finally { $stream.Dispose() }
    if ($null -eq $firstPresent -and (Select-String -Path "$gameRoot\run-err.log" -Pattern '^\[present\] \d+' -Quiet)) {
        $firstPresent = ((Get-Date) - $started).TotalSeconds
        "first present window line at +$([int]$firstPresent) s" | Add-Content (Join-Path $output 'controller.log')
    }
}
$exitedBeforeStop = $game.HasExited
"exited=$exitedBeforeStop after $([int]((Get-Date) - $started).TotalSeconds) s" | Add-Content (Join-Path $output 'controller.log')
if (-not $game.HasExited) {
    $current = Get-Process -Id $game.Id -ErrorAction Stop
    if ($current.Path -ne $identity.path -or $current.StartTime.ToUniversalTime().ToString('o') -ne $identity.started) { throw 'Process identity changed; refusing to stop it' }
    Stop-Process -Id $game.Id -Force
    $game.WaitForExit()
    'Stopped owned process at the requested duration' | Add-Content (Join-Path $output 'controller.log')
}
$game.WaitForExit()
[pscustomobject]@{exitedBeforeStop=$exitedBeforeStop;exitCode=$game.ExitCode;elapsedSeconds=((Get-Date)-$started).TotalSeconds;utc=[datetime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'exit.json')
Copy-Item "$gameRoot\run-err.log" (Join-Path $output 'run-err.log')
Copy-Item "$gameRoot\run-out.log" (Join-Path $output 'run-out.log')
Write-Output "Archived $Tag in $output"
