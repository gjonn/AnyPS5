param(
    [Parameter(Mandatory)][int]$AttachPid,
    [Parameter(Mandatory)][string]$RunDirectory,
    [Parameter(Mandatory)][ValidateSet(0,1)][int]$MergeDrawBarriers,
    [Parameter(Mandatory)][ValidateSet(0,1)][int]$BdaTableDeviceLocal
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
if (-not $root.StartsWith('D:\ps5\gt7\diagnostics-rapid\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Control directory must be inside GT7 diagnostics' }
$identity = Get-Content -LiteralPath (Join-Path $root 'verified-process.json') -Raw | ConvertFrom-Json
$expectedStart = ([datetime]$identity.started).ToUniversalTime()
$game = Get-Process -Id $AttachPid
if ($identity.pid -ne $AttachPid -or $game.Path -ne 'D:\ps5\gt7\app.exe' -or $game.StartTime.ToUniversalTime() -ne $expectedStart) { throw 'Game identity changed' }
$configuration = Get-Content -LiteralPath (Join-Path $root 'configuration.json') -Raw | ConvertFrom-Json
$controlFile = Join-Path $root 'performance-controls.txt'
if ($configuration.liveControlFile -ne $controlFile) { throw 'This run was not started with this live-control file' }
$content = "merge_draw_barriers=$MergeDrawBarriers`nbda_table_device_local=$BdaTableDeviceLocal`n"
$current = (Get-Content -LiteralPath $controlFile -Raw).Replace("`r`n", "`n")
if ($current -eq $content) { Write-Output 'Control file already requests these settings; no transition was made'; return }
$log = 'D:\ps5\gt7\run-err.log'
$offset = (Get-Item -LiteralPath $log).Length
$temporary = Join-Path $root 'performance-controls.next'
[IO.File]::WriteAllText($temporary, $content, [Text.Encoding]::ASCII)
Move-Item -LiteralPath $temporary -Destination $controlFile -Force
$expected = "[perf-controls] merge_draw_barriers=$MergeDrawBarriers bda_table_device_local=$BdaTableDeviceLocal"
$watch = [Diagnostics.Stopwatch]::StartNew()
do {
    $game.Refresh()
    if ($game.HasExited -or $game.StartTime.ToUniversalTime() -ne $expectedStart) { throw 'Game exited during control update' }
    $stream = [IO.File]::Open($log, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -lt $offset) { throw 'Game log was truncated' }
        [void]$stream.Seek($offset, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($stream)
        $recent = $reader.ReadToEnd()
    } finally { $stream.Dispose() }
    if ($recent.Contains($expected)) {
        [pscustomobject]@{ time=(Get-Date).ToString('o'); pid=$AttachPid; mergeDrawBarriers=$MergeDrawBarriers; bdaTableDeviceLocal=$BdaTableDeviceLocal; acknowledgement=$expected } |
            Export-Csv -LiteralPath (Join-Path $root 'control-history.csv') -Append -NoTypeInformation
        Write-Output $expected
        return
    }
    Start-Sleep -Milliseconds 200
} while ($watch.Elapsed.TotalSeconds -lt 10)
throw 'Control file updated but game acknowledgement was not observed; keep this process running and inspect its state'
