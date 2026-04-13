param(
    [string]$Environment = "T-Dongle-S3",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build_deauth_detector.ps1"
$releaseDir = Join-Path $projectRoot "release"
$buildDir = Join-Path $projectRoot ".pio\build\$Environment"
$pythonPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptoolPath = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"

if (-not $SkipBuild) {
    & $buildScript -Environment $Environment
    if ($LASTEXITCODE -ne 0) {
        throw "Build step failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path $pythonPath)) {
    throw "PlatformIO Python runtime not found at $pythonPath"
}

if (-not (Test-Path $esptoolPath)) {
    throw "esptool.py not found at $esptoolPath"
}

$bootloader = Join-Path $buildDir "bootloader.bin"
$partitions = Join-Path $buildDir "partitions.bin"
$firmware = Join-Path $buildDir "firmware.bin"

foreach ($artifact in @($bootloader, $partitions, $firmware)) {
    if (-not (Test-Path $artifact)) {
        throw "Missing build artifact: $artifact"
    }
}

New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$mergedBin = Join-Path $releaseDir "deauth-detector-$Environment-$timestamp-merged.bin"
$manifest = Join-Path $releaseDir "deauth-detector-$Environment-$timestamp.txt"

Push-Location $projectRoot
try {
    & $pythonPath $esptoolPath --chip esp32s3 merge_bin -o $mergedBin --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0000 $bootloader 0x8000 $partitions 0x10000 $firmware
    if ($LASTEXITCODE -ne 0) {
        throw "esptool merge_bin failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

@(
    "Environment: $Environment"
    "Created: $timestamp"
    "Merged image: $(Split-Path $mergedBin -Leaf)"
    "Bootloader: 0x0000 -> $bootloader"
    "Partitions: 0x8000 -> $partitions"
    "Firmware: 0x10000 -> $firmware"
    "Flash mode: dio"
    "Flash frequency: 80m"
    "Flash size: 16MB"
) | Set-Content -Path $manifest

Write-Host "Release image created: $mergedBin"
Write-Host "Manifest created: $manifest"