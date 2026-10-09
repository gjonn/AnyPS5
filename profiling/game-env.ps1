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

function Get-MenuWindow([string]$Log = 'D:\ps5\gt7\run-err.log') {
    $tail = Get-Content $Log -Tail 4000 -ErrorAction SilentlyContinue
    $present = $tail | Where-Object { $_ -match '^\[present\] (\d+) presents' } | Select-Object -Last 1
    $pipe = $tail | Where-Object { $_ -match '^\[pipecache\]' } | Select-Object -Last 1
    [pscustomobject]@{ presents = if ($present -match '^\[present\] (\d+) presents') { [int]$Matches[1] } else { -1 }; misses = if ($pipe -match '(\d+) misses') { [int]$Matches[1] } else { -1 }; key = "$present|$pipe" }
}

function Wait-MenuSettled([System.Diagnostics.Process]$Game, [int]$TimeoutSeconds = 1800, [scriptblock]$Note = { param($t) Write-Output $t }, [int]$QuietWindows = 6) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastKey = ''; $streak = 0; $loadingSeen = $false
    while ((Get-Date) -lt $deadline -and -not $Game.HasExited) {
        Start-Sleep 2
        $w = Get-MenuWindow
        if ($w.key -eq $lastKey) { continue }
        $lastKey = $w.key
        & $Note "window presents=$($w.presents) misses=$($w.misses)"
        if ($w.presents -ge 450) { $loadingSeen = $true; $streak = 0; continue }
        if ($loadingSeen -and $w.presents -ge 20 -and $w.presents -lt 300 -and $w.misses -eq 0) { $streak++ } else { $streak = 0 }
        if ($streak -ge $QuietWindows) { return $true }
    }
    return $false
}
