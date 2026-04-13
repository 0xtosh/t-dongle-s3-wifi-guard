param(
    [string]$Environment = "T-Dongle-S3"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$pioPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"

if (-not (Test-Path $pioPath)) {
    $pioCommand = Get-Command pio -ErrorAction SilentlyContinue
    if ($null -eq $pioCommand) {
        throw "PlatformIO CLI not found. Install PlatformIO or ensure $pioPath exists."
    }
    $pioPath = $pioCommand.Source
}

Push-Location $projectRoot
try {
    & $pioPath run -e $Environment
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO build failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}