param(
    [Parameter(Mandatory)][string]$RunDirectory,
    [ValidateRange(40,180)][int]$SegmentSeconds = 75
)
$ErrorActionPreference = 'Stop'
$runRoot = [IO.Path]::GetFullPath($RunDirectory)
if (-not $runRoot.StartsWith('D:\ps5\gt7\diagnostics-rapid\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Run must be inside diagnostics' }
$identity = Get-Content -LiteralPath (Join-Path $runRoot 'process.json') -Raw | ConvertFrom-Json
$game = Get-Process -Id $identity.pid -ErrorAction Stop
if ($game.Path -ne $identity.path -or $game.StartTime.ToUniversalTime() -ne ([datetime]$identity.started).ToUniversalTime()) { throw 'Process identity mismatch' }
$segmentsFile = Join-Path $runRoot 'ab-segments.csv'
if (Test-Path -LiteralPath $segmentsFile) { throw 'A/B evidence already exists' }
$index = 0
foreach ($enabled in @(1,0,1)) {
    & (Join-Path $PSScriptRoot 'set-live-performance.ps1') -AttachPid $game.Id -RunDirectory $runRoot -MergeDrawBarriers 1 -BdaTableDeviceLocal 0 -InPlaceDrawInputs $enabled
    $start = [datetime]::UtcNow
    $name = "segment-$index-inputs-$enabled"
    Write-Output "$name started $($start.ToString('o'))"
    while (([datetime]::UtcNow - $start).TotalSeconds -lt $SegmentSeconds) {
        $game.Refresh()
        if ($game.HasExited) { throw 'Game exited during measurement' }
        $gpu = & nvidia-smi --query-gpu=temperature.gpu,clocks.gr,utilization.gpu,power.draw --format=csv,noheader
        [pscustomobject]@{utc=[datetime]::UtcNow.ToString('o');segment=$name;gpu=$gpu;title=$game.MainWindowTitle} | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $runRoot 'ab-samples.jsonl')
        Start-Sleep -Seconds 5
    }
    [pscustomobject]@{name=$name;enabled=$enabled;start=$start.ToString('o');end=[datetime]::UtcNow.ToString('o')} | Export-Csv -LiteralPath $segmentsFile -Append -NoTypeInformation
    Get-Content -LiteralPath 'D:\ps5\gt7\run-err.log' -Tail 3500 | Set-Content -LiteralPath (Join-Path $runRoot "$name-tail.log")
    $index++
}
Write-Output 'A/B sequence complete; direct reads remain enabled.'
