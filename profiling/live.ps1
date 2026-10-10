param([string[]]$Set = @(), [string]$Dump = '', [int]$MinWidth = 1920, [int]$TimeoutSeconds = 120)
$ErrorActionPreference = 'Stop'
$gameRoot = 'D:\ps5\gt7'
$file = Join-Path $gameRoot 'debug-control.txt'
$values = [ordered]@{}
if (Test-Path -LiteralPath $file) {
    foreach ($line in Get-Content -LiteralPath $file) {
        if ($line -match '^([a-z_]+)=(.*)$' -and $Matches[1] -ne 'dump') { $values[$Matches[1]] = $Matches[2] }
    }
}
foreach ($pair in $Set) {
    $name, $value = $pair -split '=', 2
    $values[$name] = $value
}
if ($Dump -ne '') {
    if ($Dump -notmatch '^[a-zA-Z0-9_-]+$') { throw "Dump label must be alphanumeric: $Dump" }
    $values['dump_min_width'] = "$MinWidth"
}
$lines = foreach ($key in $values.Keys) { "$key=$($values[$key])" }
if ($Dump -ne '') { $lines = @($lines) + "dump=$Dump" }
Set-Content -LiteralPath $file -Value $lines -Encoding ascii
Write-Output "debug-control.txt:"; $lines | ForEach-Object { "  $_" }
if ($Dump -eq '') { return }
$target = Join-Path $gameRoot "dump_$Dump"
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline -and -not (Select-String -LiteralPath (Join-Path $gameRoot 'run-err.log') -Pattern ([regex]::Escape("storage images to dump_$Dump")) -Quiet)) { Start-Sleep 1 }
if (-not (Test-Path -LiteralPath $target)) { throw "No dump appeared at $target within $TimeoutSeconds s (is the game presenting with APS5_DEBUG_CONTROL set?)" }
Write-Output "dumped to $target"
