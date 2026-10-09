param(
    [string]$Game = 'D:\ps5\gt7',
    [string]$Libs = 'C:\repos\AnyPS5-pr639\build\core\libs\libs',
    [string]$Tag = 'rapid1',
    [int]$MaxSeconds = 260,
    [string]$StopOn = 'static runtime image interface',
    [switch]$TraceCond50
)
$ErrorActionPreference = 'Stop'
if (Get-Process app -ErrorAction SilentlyContinue) { throw 'A game instance is already running; leave it alone.' }
$output = Join-Path $Game "diagnostics-rapid\$Tag"
if (Test-Path -LiteralPath $output) { throw "Capture already exists: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
Copy-Item "$Libs\*.prx" (Join-Path $Game 'libs') -Force
$env:PATH = "C:\tools\mingw64\bin;$env:PATH"
$env:APS5_NO_SNAPSHOT_CHECK = '1'
$env:APS5_WRITE_WATCH_IMPORTS = 'watch'
$env:APS5_PROFILE_DRAW = '1'
$env:APS5_DUMP_SHADER_FAILURES = '1'
$env:APS5_TRACE_BUFFER_VIEW = '1'
if ($TraceCond50) { $env:APS5_TRACE_COND50 = '1' } else { Remove-Item Env:APS5_TRACE_COND50 -ErrorAction SilentlyContinue }
$stderr = Join-Path $output 'stderr.log'
$started = Get-Date
$p = $null
try {
    $p = Start-Process -FilePath (Join-Path $Game 'app.exe') -WorkingDirectory $Game -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError $stderr
    "Started PID $($p.Id); output $output"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $p.HasExited -and $timer.Elapsed.TotalSeconds -lt $MaxSeconds) {
        Start-Sleep -Seconds 2
        $p.Refresh()
        if (Select-String -LiteralPath $stderr -Pattern 'skipped (dispatch|draw):' | Where-Object { $_.Line.Contains($StopOn) }) {
            "Captured target failure after $([math]::Round($timer.Elapsed.TotalSeconds, 1)) seconds"
            break
        }
    }
    "Window title: $($p.MainWindowTitle)"
} finally {
    if ($null -ne $p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force; "Stopped owned PID $($p.Id)" }
    Get-ChildItem -LiteralPath $Game -Filter 'shader_*.req' -File |
        Where-Object { $_.LastWriteTime -ge $started } |
        Copy-Item -Destination $output
    "Saved capture: $output"
}
