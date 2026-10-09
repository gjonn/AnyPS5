param([string]$Tag = 'ngfx-elevated-1', [int]$StartAfterFrames = 5000, [int]$Frames = 5)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell' }
if (Get-Process app -ErrorAction SilentlyContinue) { throw 'A game instance is already running; close it first' }
. (Join-Path $PSScriptRoot 'game-env.ps1')
$ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe'
$gameRoot = 'D:\ps5\gt7'
$repoRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $gameRoot "diagnostics-rapid\$Tag"
New-Item -ItemType Directory -Force -Path $output | Out-Null
Copy-Item "$repoRoot\build\core\libs\libs\*.prx" "$gameRoot\libs\" -Force
$envList = (Get-ChildItem Env: | Where-Object { $_.Name -like 'APS5_*' } | ForEach-Object { "$($_.Name)=$($_.Value);" }) -join ' '
Set-Location $gameRoot
& $ngfx --activity 'GPU Trace Profiler' --platform 'Windows (x86_64)' --exe "$gameRoot\app.exe" --dir $gameRoot --env $envList --start-after-frames $StartAfterFrames --limit-to-frames $Frames --max-duration-ms 5000 --time-every-action --real-time-shader-profiler --auto-export --output-dir $output 2>&1 | Tee-Object -FilePath (Join-Path $output 'ngfx.txt')
Write-Output "Done. Results in $output. Close the game window if it is still open."
