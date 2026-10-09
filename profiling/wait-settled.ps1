param([Parameter(Mandatory)][int]$AttachPid, [int]$TimeoutSeconds = 900, [int]$Quiet = 2)
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$streak = 0
$lastPipe = ''
while ((Get-Date) -lt $deadline) {
    $game = Get-Process -Id $AttachPid -ErrorAction SilentlyContinue
    if (-not $game) { 'game exited'; exit 2 }
    $tail = Get-Content D:\ps5\gt7\run-err.log -Tail 4000
    $present = $tail | Where-Object { $_ -match '^\[present\] (\d+) presents' } | Select-Object -Last 1
    $pipe = $tail | Where-Object { $_ -match '^\[pipecache\]' } | Select-Object -Last 1
    $presents = if ($present -match '^\[present\] (\d+) presents') { [int]$Matches[1] } else { 999 }
    $misses = if ($pipe -match '(\d+) misses') { [int]$Matches[1] } else { 999 }
    if ($pipe -ne $lastPipe) {
        $lastPipe = $pipe
        if ($presents -lt 300 -and $misses -eq 0) { $streak++ } else { $streak = 0 }
    }
    if ($streak -ge $Quiet) { "settled: presents $presents, title $($game.MainWindowTitle)"; exit 0 }
    Start-Sleep -Seconds 5
}
'timeout'; exit 1
