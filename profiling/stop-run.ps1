param([Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag)
$ErrorActionPreference = 'Stop'
$file = "D:\ps5\gt7\diagnostics-rapid\$Tag\process.json"
$identity = [System.Text.Json.JsonDocument]::Parse([System.IO.File]::ReadAllText($file)).RootElement
$processId = $identity.GetProperty('pid').GetInt32()
$path = $identity.GetProperty('path').GetString()
$started = $identity.GetProperty('started').GetString()
$process = Get-Process -Id $processId -ErrorAction SilentlyContinue
if ($null -eq $process) { Write-Output "PID $processId already exited"; return }
if ($process.Path -ne $path -or $process.StartTime.ToUniversalTime().ToString('o') -ne $started) { throw "PID $processId does not match the recorded identity for $Tag" }
Stop-Process -Id $processId -Force
$process.WaitForExit()
Write-Output "Stopped PID $processId ($Tag)"
