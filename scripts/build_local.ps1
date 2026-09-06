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

Write-Host "[1/3] Checking and preparing ZMK workspace in Docker..." -ForegroundColor Yellow
$InitCmd = @'
if [ ! -d "/work/app" ]; then
    echo "--- Initializing ZMK source tree (first time setup) ---"
    cd /work
    west init -m https://github.com/zmkfirmware/zmk --mr main .
    west update
    west zephyr-export
fi
'@

docker run --rm -v "${VolumeName}:/work" -v "${ProjectDir}:/config" $ImageName bash -c "$InitCmd"

Write-Host "[2/3] Compiling firmware (Seeed XIAO BLE + FIDO Scanner)..." -ForegroundColor Yellow
$BuildCmd = @'
cd /work
west build -p -b xiao_ble//zmk -s /work/app -d /work/build -- -DSHIELD=fido_scanner -DZMK_CONFIG=/config/config
if [ -f "/work/build/zephyr/zmk.uf2" ]; then
    cp /work/build/zephyr/zmk.uf2 /config/zmk.uf2
    echo "BUILD_SUCCESS"
else
    echo "BUILD_FAILED"
    exit 1
fi
'@

docker run --rm -v "${VolumeName}:/work" -v "${ProjectDir}:/config" $ImageName bash -c "$BuildCmd"

if (Test-Path "$ProjectDir\zmk.uf2") {
    Write-Host "==========================================" -ForegroundColor Green
    Write-Host "[3/3] SUCCESS! Firmware compiled:" -ForegroundColor Green
    Write-Host "  -> $ProjectDir\zmk.uf2" -ForegroundColor Cyan
    Write-Host "Double-tap Reset on XIAO BLE and drag zmk.uf2 to the USB drive." -ForegroundColor Green
    Write-Host "==========================================" -ForegroundColor Green
} else {
    Write-Host "Error: zmk.uf2 was not created." -ForegroundColor Red
    exit 1
}
