param(
    [string]$Port = "COM16",
    [string]$Environment = "T-Dongle-S3",
    [string]$ImagePath
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$pioPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$pythonPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptoolPath = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
$releaseDir = Join-Path $projectRoot "release"

if ([string]::IsNullOrWhiteSpace($ImagePath)) {
    if (Test-Path $releaseDir) {
        $latestImage = Get-ChildItem -Path $releaseDir -Filter "deauth-detector-$Environment-*-merged.bin" |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $latestImage) {
            $ImagePath = $latestImage.FullName
        }
    }
}

if (-not [string]::IsNullOrWhiteSpace($ImagePath)) {
    if (-not (Test-Path $pythonPath)) {
        throw "PlatformIO Python runtime not found at $pythonPath"
    }
    if (-not (Test-Path $esptoolPath)) {
        throw "esptool.py not found at $esptoolPath"
    }
    if (-not (Test-Path $ImagePath)) {
        throw "Merged image not found: $ImagePath"
    }

    & $pythonPath $esptoolPath --chip esp32s3 --port $Port --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0000 $ImagePath
    if ($LASTEXITCODE -ne 0) {
        throw "esptool flash failed with exit code $LASTEXITCODE"
    }
    exit 0
}

if (-not (Test-Path $pioPath)) {
    $pioCommand = Get-Command pio -ErrorAction SilentlyContinue
    if ($null -eq $pioCommand) {
        throw "PlatformIO CLI not found. Install PlatformIO or ensure $pioPath exists."
    }
    $pioPath = $pioCommand.Source
}

Push-Location $projectRoot
try {
    & $pioPath run -e $Environment -t upload --upload-port $Port
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO flash failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}