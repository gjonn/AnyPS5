param(
    [string]$Capture = 'D:\ps5\gt7\diagnostics-rapid\rapid2',
    [string]$Label = 'baseline',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$env:PATH = "C:\tools\mingw64\bin;$repo\build\core\libs\libs\unpatched;$env:PATH"
$requests = @(Get-ChildItem -LiteralPath $Capture -Filter 'shader_*.req' -File | Sort-Object Name)
if ($requests.Count -eq 0) { throw "No shader requests in $Capture" }
$output = Join-Path $Capture "replay-$Label"
if (Test-Path -LiteralPath $output) { throw "Replay results already exist: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
if (-not $SkipBuild) {
    & cmake --build (Join-Path $repo 'build') --target agc_shader_replay -- -k 0 *> (Join-Path $output 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "Replay build failed; see $output\build.log" }
}
$exe = Join-Path $repo 'build\core\libs\prx\libSceAgcDriver\agc_shader_replay.exe'
$summary = foreach ($request in $requests) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $exe --images $request.FullName *> (Join-Path $output "$($request.BaseName).log")
    $code = $LASTEXITCODE
    $timer.Stop()
    [pscustomobject]@{
        Request = $request.Name
        SHA256 = (Get-FileHash -LiteralPath $request.FullName -Algorithm SHA256).Hash
        ExitCode = $code
        Seconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
    }
}
$summary | Export-Csv -LiteralPath (Join-Path $output 'summary.csv') -NoTypeInformation
$summary | Format-Table Request,ExitCode,Seconds
"Results: $output"
if ($summary | Where-Object ExitCode -NE 0) { exit 1 }
