param([Parameter(Mandatory)][string]$Tag, [switch]$ReuseDeployed, [switch]$TraceWaits, [string]$CondCaller, [ValidateRange(-1,16384)][int]$DevicePoolMiB = -1, [switch]$OrderedLabels, [switch]$DrawInputMemo, [switch]$ProfileGpu, [switch]$DeviceLocalShaderData, [switch]$MergeDrawBarriers, [switch]$BdaTableDeviceLocal, [switch]$LiveControls)
$ErrorActionPreference = 'Stop'
if (Get-Process app -ErrorAction SilentlyContinue) { throw 'A game instance is already running' }
$gameRoot = 'D:\ps5\gt7'
$repoRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $gameRoot "diagnostics-rapid\$Tag"
if (Test-Path -LiteralPath $output) { throw "Run directory already exists: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
if (-not $ReuseDeployed) { Copy-Item "$repoRoot\build\core\libs\libs\*.prx" "$gameRoot\libs\" -Force }
$env:PATH = "C:\tools\mingw64\bin;$env:PATH"
$env:APS5_NO_SNAPSHOT_CHECK = '1'
$env:APS5_WRITE_WATCH_IMPORTS = 'watch'
$env:APS5_PROFILE_DRAW = '1'
$env:APS5_DUMP_SHADER_FAILURES = '1'
$env:APS5_TRACE_BUFFER_VIEW = '1'
if ($DevicePoolMiB -ge 0) { $env:APS5_STAGING_POOL_MIB = [string]$DevicePoolMiB }
if ($OrderedLabels) { $env:APS5_PIPELINE_ORDERED_LABELS = '1' }
if ($DrawInputMemo) { $env:APS5_DRAW_INPUT_MEMO = '1' }
if ($ProfileGpu) { $env:APS5_PROFILE_GPU = '1' }
if ($MergeDrawBarriers) { $env:APS5_MERGE_DRAW_BARRIERS = '1' } else { Remove-Item Env:\APS5_MERGE_DRAW_BARRIERS -ErrorAction SilentlyContinue }
$env:APS5_BDA_TABLE_DEVICE_LOCAL = if ($BdaTableDeviceLocal) { '1' } else { '0' }
if ($LiveControls) {
    $env:APS5_PERF_CONTROL = Join-Path $output 'performance-controls.txt'
    @("merge_draw_barriers=$([int][bool]$MergeDrawBarriers)", "bda_table_device_local=$([int][bool]$BdaTableDeviceLocal)") | Set-Content -LiteralPath $env:APS5_PERF_CONTROL -Encoding ascii
} else { Remove-Item Env:\APS5_PERF_CONTROL -ErrorAction SilentlyContinue }
$env:APS5_SHADER_DATA_DEVICE_LOCAL = if ($DeviceLocalShaderData) { '1' } else { '0' }
if ($CondCaller) { $env:APS5_TRACE_COND_CALLER = $CondCaller }
if ($TraceWaits) {
    $env:APS5_TRACE_WAIT_TIMELINE = '1'
    $env:APS5_TRACE_EQUEUE_EVENTS = '1'
    $env:APS5_TRACE_GPU = '1'
}
@{ reuseDeployed=[bool]$ReuseDeployed; traceWaits=[bool]$TraceWaits; condCaller=$env:APS5_TRACE_COND_CALLER; waitTimeline=$env:APS5_TRACE_WAIT_TIMELINE; equeueEvents=$env:APS5_TRACE_EQUEUE_EVENTS; gpuTrace=$env:APS5_TRACE_GPU; writeWatch=$env:APS5_WRITE_WATCH_IMPORTS; devicePoolMiB=$env:APS5_STAGING_POOL_MIB; orderedLabels=$env:APS5_PIPELINE_ORDERED_LABELS; drawInputMemo=$env:APS5_DRAW_INPUT_MEMO; profileGpu=$env:APS5_PROFILE_GPU; deviceLocalShaderData=$env:APS5_SHADER_DATA_DEVICE_LOCAL; mergeDrawBarriers=$env:APS5_MERGE_DRAW_BARRIERS; bdaTableDeviceLocal=$env:APS5_BDA_TABLE_DEVICE_LOCAL; liveControlFile=$env:APS5_PERF_CONTROL } | ConvertTo-Json | Set-Content (Join-Path $output 'configuration.json')
Get-FileHash "$gameRoot\libs\*.prx" | Export-Csv (Join-Path $output 'modules.csv') -NoTypeInformation
$game = Start-Process -FilePath "$gameRoot\app.exe" -WorkingDirectory $gameRoot -RedirectStandardOutput "$gameRoot\run-out.log" -RedirectStandardError "$gameRoot\run-err.log" -PassThru
$started = Get-Date
@{ pid=$game.Id; started=$started.ToString('o'); tag=$Tag } | ConvertTo-Json | Set-Content (Join-Path $output 'process.json')
Write-Output "Started game PID $($game.Id); evidence $output"
while (-not $game.WaitForExit(10000)) {
    $game.Refresh()
    [pscustomobject]@{time=(Get-Date).ToString('o'); seconds=((Get-Date)-$started).TotalSeconds; cpu=$game.CPU; title=$game.MainWindowTitle} | Export-Csv (Join-Path $output 'progress.csv') -NoTypeInformation -Append
}
$game.WaitForExit()
@{ pid=$game.Id; exitCode=$game.ExitCode; exited=(Get-Date).ToString('o') } | ConvertTo-Json | Set-Content (Join-Path $output 'exit.json')
Copy-Item "$gameRoot\run-err.log" (Join-Path $output 'stderr.log')
Copy-Item "$gameRoot\run-out.log" (Join-Path $output 'stdout.log')
Write-Output "Game PID $($game.Id) exited with code $($game.ExitCode)"
