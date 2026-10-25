# Build pebble_sim.exe using xpack mingw-w64-gcc
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)
$mingw = Join-Path $env:USERPROFILE "node_modules\@xpack-dev-tools\mingw-w64-gcc\.content\bin"
if (-not (Test-Path "$mingw\x86_64-w64-mingw32-gcc.exe")) {
    Write-Host "Missing xpack mingw binaries at $mingw"
    Write-Host "Install @xpack-dev-tools/mingw-w64-gcc and ensure .content/bin exists."
    exit 1
}
$env:Path = "$mingw;$env:Path"
$env:CC = "$mingw\x86_64-w64-mingw32-gcc.exe"
$env:AR = "$mingw\x86_64-w64-mingw32-ar.exe"
$env:PYTHONIOENCODING = "utf-8"
Set-Location $root
if (-not (Test-Path "sim\lvgl\lv_version.h")) {
    python sim\setup_sim.py
}
python sim\build.py
Write-Host ""
Write-Host "Screenshots:  .\sim\pebble_sim.exe"
Write-Host "Browser:      .\sim\run_server.ps1   then http://localhost:8792"
