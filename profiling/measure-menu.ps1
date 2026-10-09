param([Parameter(Mandatory)][int]$AttachPid, [Parameter(Mandatory)][string]$OutFile, [ValidateRange(5,60)][int]$Seconds = 30, [string]$GameLog = 'D:\ps5\gt7\run-err.log')
$ErrorActionPreference = 'Stop'
$game = Get-Process -Id $AttachPid
if ($game.Path -ne 'D:\ps5\gt7\app.exe') { throw 'Target is not the GT7 process' }
$startTime = $game.StartTime
$logOffset = (Get-Item -LiteralPath $GameLog).Length
$samples = @()
$watch = [Diagnostics.Stopwatch]::StartNew()
do {
    $game.Refresh()
    if ($game.HasExited -or $game.StartTime -ne $startTime) { throw 'Measured process exited or changed' }
    $title = $game.MainWindowTitle
    if ($title -notmatch 'FPS:\s+([\d.]+)\s+\((\d+)\)') { throw "No frame counter in title: $title" }
    $samples += [pscustomobject]@{ time=(Get-Date).ToString('o'); seconds=$watch.Elapsed.TotalSeconds; frame=[long]$Matches[2]; titleFps=[double]$Matches[1]; cpu=$game.CPU; title=$title }
    if ($watch.Elapsed.TotalSeconds -ge $Seconds) { break }
    Start-Sleep -Milliseconds ([int][Math]::Max(0, [Math]::Min(5000, 1000 * ($Seconds - $watch.Elapsed.TotalSeconds))))
} while ($true)
$first = $samples[0]
$last = $samples[-1]
$logStream = [IO.File]::Open($GameLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try {
    if ($logStream.Length -lt $logOffset) { throw 'Game log was truncated during measurement' }
    [void]$logStream.Seek($logOffset, [IO.SeekOrigin]::Begin)
    $reader = [IO.StreamReader]::new($logStream)
    $intervalLog = $reader.ReadToEnd()
} finally { $logStream.Dispose() }
$reports = @($intervalLog -split '\r?\n' | Where-Object { $_ -match '^\[(present|pipecache|control-flow)\]' })
$presentationCounts = @([regex]::Matches($intervalLog, '(?m)^\[present\] (\d+) presents over') | ForEach-Object { [long]$_.Groups[1].Value })
$pipelineMisses = @([regex]::Matches($intervalLog, '(?m)^\[pipecache\][^\r\n]*? (\d+) misses,') | ForEach-Object { [long]$_.Groups[1].Value })
$newValidations = [regex]::Matches($intervalLog, '(?m)^\[control-flow\] validation=').Count
$warnings = @()
if ($presentationCounts.Count -lt 2) { $warnings += 'Fewer than two presentation reports; actual display throughput not confirmed for this window.' }
if ($presentationCounts -contains 0) { $warnings += 'A reporting interval had no actual presentations; frame-counter FPS is not a visible-menu result.' }
if ($pipelineMisses.Count -lt 2) { $warnings += 'Fewer than two graphics cache reports; compilation-free throughput not confirmed.' }
if (@($pipelineMisses | Where-Object { $_ -ne 0 }).Count -ne 0 -or $newValidations -ne 0) { $warnings += 'Shader validation or new graphics pipeline creation overlapped this measurement.' }
$result = [ordered]@{ pid=$AttachPid; processStarted=$startTime.ToString('o'); seconds=($last.seconds-$first.seconds); frames=($last.frame-$first.frame); fps=(($last.frame-$first.frame)/($last.seconds-$first.seconds)); displayAndCacheChecksPassed=($warnings.Count -eq 0); warnings=$warnings; presentationCounts=$presentationCounts; pipelineMisses=$pipelineMisses; newValidations=$newValidations; reports=$reports; samples=$samples }
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutFile
$result | ConvertTo-Json -Depth 5
