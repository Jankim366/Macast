[CmdletBinding()]
param(
    [string]$Version = "0.4.1",
    [string]$Destination = "",
    [string]$ExpectedSha256 = "5f1792a96efffe84b220edfa7286fa7670806226aa4d0b4b6c7e33ddd7747bf9"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Destination) {
    $Destination = Join-Path $repoRoot "bin\AirPlay-Windows"
}
$Destination = [IO.Path]::GetFullPath($Destination)
$repoPrefix = $repoRoot.TrimEnd('\') + '\'
if (-not $Destination.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "AirPlay-Windows destination must stay inside the Macast repository."
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) (
    "macast-airplay-windows-" + [Guid]::NewGuid().ToString("N"))
$archive = Join-Path $temporary "airplay-windows.zip"
$extracted = Join-Path $temporary "extracted"
$customBackup = Join-Path $temporary "airplay-windows-macast.exe"
$customDestination = Join-Path $Destination "airplay-windows-macast.exe"
$url = "https://github.com/moieric11/AirPlay-Windows/releases/download/v$Version/airplay-windows-v$Version-x64.zip"

try {
    New-Item -ItemType Directory -Path $temporary | Out-Null
    Write-Host "Downloading AirPlay-Windows v$Version..."
    Invoke-WebRequest -Uri $url -OutFile $archive

    $actualSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $ExpectedSha256.ToLowerInvariant()) {
        throw "AirPlay-Windows SHA-256 mismatch. Expected $ExpectedSha256, got $actualSha256."
    }

    Expand-Archive -LiteralPath $archive -DestinationPath $extracted
    $executable = Get-ChildItem -Recurse -File -LiteralPath $extracted `
        -Filter "airplay-windows.exe" | Select-Object -First 1
    if (-not $executable) {
        throw "airplay-windows.exe was not found in the release archive."
    }
    $sourceDirectory = $executable.Directory.FullName

    if (Test-Path -LiteralPath $customDestination) {
        Copy-Item -LiteralPath $customDestination -Destination $customBackup
    }
    if (Test-Path -LiteralPath $Destination) {
        [IO.Directory]::Delete($Destination, $true)
    }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    Copy-Item -Path (Join-Path $sourceDirectory "*") `
        -Destination $Destination -Recurse -Force
    if (Test-Path -LiteralPath $customBackup) {
        Copy-Item -LiteralPath $customBackup -Destination $customDestination
    }

    foreach ($required in @(
            "airplay-windows.exe",
            "SDL2.dll",
            "avcodec-62.dll",
            "avformat-62.dll",
            "avutil-60.dll")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Destination $required))) {
            throw "AirPlay-Windows runtime file is missing: $required"
        }
    }

    Write-Host "Prepared AirPlay-Windows v$Version in $Destination"
}
finally {
    if ([IO.Directory]::Exists($temporary)) {
        [IO.Directory]::Delete($temporary, $true)
    }
}
