param(
    [string]$Destination = "standalone-github"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$destinationRoot = Join-Path $projectRoot $Destination

$includePaths = @(
    ".gitignore",
    "LICENSE",
    "README.md",
    "platformio.ini",
    "build_deauth_detector.bat",
    "package_deauth_detector.bat",
    "flash_deauth_detector.bat",
    "export_standalone_repo.bat",
    "boards\dongles3.json",
    "examples\deauth_detector",
    "scripts\build_deauth_detector.ps1",
    "scripts\package_deauth_detector.ps1",
    "scripts\flash_deauth_detector.ps1",
    "scripts\export_standalone_repo.ps1"
)

if (Test-Path $destinationRoot) {
    Remove-Item -Recurse -Force $destinationRoot
}

New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null

foreach ($relativePath in $includePaths) {
    $sourcePath = Join-Path $projectRoot $relativePath
    $targetPath = Join-Path $destinationRoot $relativePath

    if (-not (Test-Path $sourcePath)) {
        throw "Missing export source path: $sourcePath"
    }

    if ((Get-Item $sourcePath) -is [System.IO.DirectoryInfo]) {
        New-Item -ItemType Directory -Force -Path $targetPath | Out-Null
        Copy-Item -Path (Join-Path $sourcePath "*") -Destination $targetPath -Recurse -Force
    } else {
        $targetDir = Split-Path -Parent $targetPath
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
        Copy-Item -Path $sourcePath -Destination $targetPath -Force
    }
}

$exportReadme = Join-Path $destinationRoot "PUBLISHING.md"
@(
    "# Publishing Notes",
    "",
    "This folder is a clean standalone export of the T-Dongle-S3 deauth detector.",
    "",
    "Recommended next steps:",
    "1. Create a new GitHub repository.",
    "2. Copy the contents of this folder into that repository.",
    "3. Commit the files.",
    "4. Optionally attach a merged firmware image from the parent repo's release/ folder to a GitHub release."
) | Set-Content -Path $exportReadme

Write-Host "Standalone repo exported to: $destinationRoot"