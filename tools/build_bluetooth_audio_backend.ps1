[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceDirectory = Join-Path $PSScriptRoot "bluetooth-audio-backend"
$buildDirectory = Join-Path $sourceDirectory "build"

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path -LiteralPath $cmake)) {
    throw "CMake was not found."
}

& $cmake -S $sourceDirectory -B $buildDirectory `
    -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) {
    throw "Unable to configure the Bluetooth audio backend."
}

& $cmake --build $buildDirectory --config Release --parallel 8
if ($LASTEXITCODE -ne 0) {
    throw "Unable to build the Bluetooth audio backend."
}

$binary = Join-Path $buildDirectory "Release\macast-bluetooth-audio.exe"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "The Bluetooth build did not produce an executable."
}

$destination = Join-Path $repoRoot "bin\BluetoothAudio"
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath $binary `
    -Destination (Join-Path $destination "macast-bluetooth-audio.exe") `
    -Force

Write-Host "Built Macast Bluetooth audio backend at:"
Write-Host (Join-Path $destination "macast-bluetooth-audio.exe")
