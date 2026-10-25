# Launch browser sim on Windows (xpack mingw must already have built pebble_sim.exe)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$mingw = Join-Path $env:USERPROFILE "node_modules\@xpack-dev-tools\mingw-w64-gcc\.content\bin"
if (Test-Path $mingw) { $env:Path = "$mingw;$env:Path" }

$exe = Join-Path $here "pebble_sim.exe"
if (-not (Test-Path $exe)) {
    Write-Host "Missing $exe - run: python sim/build.py"
    exit 1
}

$env:PORT = if ($env:PORT) { $env:PORT } else { "8792" }
if (-not $env:SIM_SCALE) { $env:SIM_SCALE = "1" }  # full-res text
Write-Host "Open http://localhost:$($env:PORT)  (SIM_SCALE=$($env:SIM_SCALE))"
Set-Location $here
python server.py
