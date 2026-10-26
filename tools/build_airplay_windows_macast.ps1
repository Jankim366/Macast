[CmdletBinding()]
param(
    [string]$Version = "v0.4.1",
    [string]$SourceDirectory = "",
    [string]$VcpkgDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Join-Path $repoRoot "build"
if (-not $SourceDirectory) {
    $SourceDirectory = Join-Path $buildRoot "AirPlay-Windows"
}
if (-not $VcpkgDirectory) {
    $VcpkgDirectory = Join-Path $buildRoot "vcpkg"
}
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $SourceDirectory ".git"))) {
    git clone https://github.com/moieric11/AirPlay-Windows.git $SourceDirectory
}
git -C $SourceDirectory checkout $Version

Copy-Item -LiteralPath (Join-Path $repoRoot "macast\assets\icon.ico") `
    -Destination (Join-Path $SourceDirectory "src\macast.ico") -Force

$patchPath = Join-Path $PSScriptRoot "airplay-windows-macast-complete.patch"
$previousErrorPreference = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
git -C $SourceDirectory apply --recount --reverse --check $patchPath 2>&1 |
    Out-Null
$alreadyApplied = $LASTEXITCODE -eq 0
$ErrorActionPreference = $previousErrorPreference
if (-not $alreadyApplied) {
    git -C $SourceDirectory apply --recount $patchPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to apply airplay-windows-macast-complete.patch. Use a clean AirPlay-Windows checkout."
    }
} else {
    Write-Host "airplay-windows-macast-complete.patch is already applied."
}

# The overlay carries the final Macast integration files. Apply it after the
# upstream patches so repeated builds are deterministic even when the
# AirPlay-Windows checkout already contains an older integration revision.
Copy-Item -Path (Join-Path $PSScriptRoot "airplay-windows-overlay\*") `
    -Destination $SourceDirectory -Recurse -Force

if (-not (Test-Path -LiteralPath (Join-Path $VcpkgDirectory ".git"))) {
    git clone --filter=blob:none https://github.com/microsoft/vcpkg.git $VcpkgDirectory
}
$baseline = (Get-Content (Join-Path $SourceDirectory "vcpkg.json") -Raw |
    ConvertFrom-Json).'builtin-baseline'
git -C $VcpkgDirectory checkout $baseline
& (Join-Path $VcpkgDirectory "bootstrap-vcpkg.bat") -disableMetrics

$python = (Get-Command python -ErrorAction Stop).Source
$env:PATH = (Split-Path $python -Parent) + ";" + $env:PATH

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path -LiteralPath $cmake)) {
    throw "CMake was not found."
}

$buildDirectory = Join-Path $SourceDirectory "build"
$toolchain = Join-Path $VcpkgDirectory "scripts\buildsystems\vcpkg.cmake"
& $cmake -S $SourceDirectory -B $buildDirectory `
    -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static `
    -DVCPKG_HOST_TRIPLET=x64-windows-static
& $cmake --build $buildDirectory --config Release `
    --target airplay-windows --parallel 8

$binary = Join-Path $buildDirectory "Release\airplay-windows.exe"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "AirPlay-Windows build did not produce an executable."
}
$destination = Join-Path $repoRoot "bin\AirPlay-Windows"
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath $binary `
    -Destination (Join-Path $destination "airplay-windows-macast.exe") -Force

# The Macast build is statically linked. Keep only legal/docs files from the
# official multi-file release so the packaged runtime does not carry unused
# DLLs or a fallback executable that lacks the standard window behavior.
$keep = @(
    "airplay-windows-macast.exe",
    "LICENSE",
    "README.md",
    "THIRD_PARTY_NOTICES.md"
)
Get-ChildItem -File -LiteralPath $destination | Where-Object {
    $_.Name -notin $keep
} | ForEach-Object {
    [IO.File]::Delete($_.FullName)
}

Write-Host "Built Macast AirPlay-Windows at:"
Write-Host (Join-Path $destination "airplay-windows-macast.exe")
