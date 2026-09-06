# PowerShell script to build ZMK firmware locally using Docker Desktop
# Prerequisites: Docker Desktop must be running.

$ErrorActionPreference = "Stop"
$ProjectDir = (Get-Item -Path $PSScriptRoot).Parent.FullName
$VolumeName = "zmk-workspace"
$ImageName = "zmkfirmware/zmk-build-arm:stable"

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "   Dixo Keyboard Local Docker Builder     " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

# 1. Check Docker Desktop
try {
    docker info | Out-Null
} catch {
    Write-Host "Error: Docker Desktop is not running! Please start Docker Desktop." -ForegroundColor Red
    exit 1
}

# 2. Create persistent volume for caching ZMK and Zephyr
docker volume create $VolumeName | Out-Null

Write-Host "Building firmware in Docker container..." -ForegroundColor Yellow
docker run --rm -v "${VolumeName}:/work" -v "${ProjectDir}:/config" $ImageName bash /config/scripts/docker_build.sh

if (Test-Path "$ProjectDir\zmk.uf2") {
    Write-Host "==========================================" -ForegroundColor Green
    Write-Host "SUCCESS! Firmware compiled:" -ForegroundColor Green
    Write-Host "  -> $ProjectDir\zmk.uf2" -ForegroundColor Cyan
    Write-Host "Double-tap Reset on XIAO BLE and drag zmk.uf2 to the USB drive." -ForegroundColor Green
    Write-Host "==========================================" -ForegroundColor Green
} else {
    Write-Host "Error: zmk.uf2 was not created." -ForegroundColor Red
    exit 1
}
