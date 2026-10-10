param([Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag, [string]$Dump = 'map_auto', [int]$TimeoutSeconds = 2400, [int]$LoadSeconds = 90)
$ErrorActionPreference = 'Stop'
$log = 'D:\ps5\gt7\run-err.log'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$seen = $false
$quiet = 0
$lastCount = 0
while ((Get-Date) -lt $deadline) {
    Start-Sleep 10
    if (-not (Get-Process app -ErrorAction SilentlyContinue)) { throw 'the game exited before reaching the menu' }
    $lines = Select-String -LiteralPath $log -Pattern '^\[present\] (\d+) presents' -ErrorAction SilentlyContinue
    if (-not $lines -or $lines.Count -eq $lastCount) { continue }
    $lastCount = $lines.Count
    $presents = [int]$lines[-1].Matches[0].Groups[1].Value
    $misses = Select-String -LiteralPath $log -Pattern '^\[pipecache\].* (\d+) misses' | Select-Object -Last 1
    $missCount = if ($misses) { [int]$misses.Matches[0].Groups[1].Value } else { 0 }
    if ($presents -ge 400) { $seen = $true; $quiet = 0 }
    elseif ($seen -and $presents -lt 200 -and $missCount -eq 0) { $quiet++ }
    else { $quiet = 0 }
    if ($quiet -ge 3) { break }
}
if ($quiet -lt 3) { throw 'the menu did not settle in time' }
Write-Output "menu settled at $(Get-Date -Format HH:mm:ss)"
Start-Sleep 20
& (Join-Path $PSScriptRoot 'press.ps1') -Tag $Tag -Keys @('ENTER')
Start-Sleep $LoadSeconds
if (-not (Get-Process app -ErrorAction SilentlyContinue)) { throw 'the game exited while loading the map' }
& (Join-Path $PSScriptRoot 'live.ps1') -Dump $Dump -MinWidth 3840 -TimeoutSeconds 150
