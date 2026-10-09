param([Parameter(Mandatory)][int]$AttachPid, [Parameter(Mandatory)][string]$OutDir, [int]$Top = 4, [int]$Passes = 300, [int]$IntervalMs = 20)
$ErrorActionPreference = 'Stop'
$game = Get-Process -Id $AttachPid
if ($game.Path -ne 'D:\ps5\gt7\app.exe') { throw 'Unexpected process' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$before = @{}
foreach ($t in $game.Threads) { try { $before[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
$startTitle = $game.MainWindowTitle
Start-Sleep -Seconds 10
$game.Refresh()
$rows = foreach ($t in $game.Threads) { try { if ($before.ContainsKey($t.Id)) { [pscustomobject]@{ Tid = $t.Id; CpuMs = [math]::Round($t.TotalProcessorTime.TotalMilliseconds - $before[$t.Id]) } } } catch {} }
$rows = $rows | Sort-Object CpuMs -Descending
$rows | Export-Csv (Join-Path $OutDir 'thread-cpu.csv') -NoTypeInformation
"title $startTitle -> $($game.MainWindowTitle)" | Set-Content (Join-Path $OutDir 'titles.txt')
$tids = @($rows | Select-Object -First $Top | ForEach-Object { $_.Tid })
& (Join-Path $PSScriptRoot 'sample-threads.ps1') -AttachPid $AttachPid -ThreadIds $tids -Passes $Passes -IntervalMs $IntervalMs -MaxHits 60 -OutFile (Join-Path $OutDir 'samples.txt') | Out-Null
$rows | Select-Object -First 10
