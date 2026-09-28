
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root ".."
$build = Join-Path $src "out"

Write-Host "Configuring x64 Release..."
cmake -S $src -B $build -A x64

Write-Host "Building..."
cmake --build $build --config Release

$dll = Join-Path $build "Release\pd2_vr_chat_buffer.dll"
if (!(Test-Path $dll)) {
    throw "Build completed but DLL was not found: $dll"
}

Write-Host ""
Write-Host "Built:"
Write-Host $dll
Write-Host ""
Write-Host "Copy this DLL into the mod folder next to mod.txt and bridge.lua."
