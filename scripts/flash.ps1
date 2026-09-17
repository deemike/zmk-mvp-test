# Auto-flasher for Seeed XIAO BLE
$uf2Path = "d:\Dixo-keyboard\zmk.uf2"

if (-not (Test-Path $uf2Path)) {
    Write-Host "Error: $uf2Path not found!" -ForegroundColor Red
    exit 1
}

$fileItem = Get-Item $uf2Path
$fileSize = $fileItem.Length
$fileTime = $fileItem.LastWriteTime

Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "       XIAO BLE Firmware Flasher                    " -ForegroundColor Cyan
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "Firmware file : $uf2Path" -ForegroundColor Yellow
Write-Host "File size     : $fileSize bytes" -ForegroundColor Yellow
Write-Host "Compiled at   : $fileTime" -ForegroundColor Yellow
Write-Host ""
Write-Host "--> WAITING FOR XIAO BLE IN BOOTLOADER MODE <--" -ForegroundColor Green
Write-Host "Please quickly DOUBLE-TAP the physical Reset button on Seeed XIAO BLE." -ForegroundColor White
Write-Host "Listening for removable UF2 drive (timeout 180s)..." -ForegroundColor Gray
Write-Host ""

$found = $false
$timeout = (Get-Date).AddSeconds(180)

while ((Get-Date) -lt $timeout) {
    $drives = Get-PSDrive -PSProvider FileSystem | Where-Object {
        $infoFile = Join-Path $_.Root "INFO_UF2.TXT"
        Test-Path $infoFile
    }

    if ($drives) {
        $targetDrive = $drives[0].Root
        Write-Host ""
        Write-Host "=== FOUND BOOTLOADER DRIVE: $targetDrive ===" -ForegroundColor Green
        
        $info = Get-Content (Join-Path $targetDrive "INFO_UF2.TXT") -ErrorAction SilentlyContinue
        Write-Host "Board info:" -ForegroundColor Gray
        $info | Select-Object -First 3 | ForEach-Object { Write-Host "   $_" -ForegroundColor DarkGray }

        Write-Host ""
        Write-Host "Copying zmk.uf2 to $targetDrive ..." -ForegroundColor Yellow
        
        $destFile = Join-Path $targetDrive "zmk.uf2"
        Copy-Item -Path $uf2Path -Destination $destFile -Force
        
        Write-Host "File transferred to bootloader successfully!" -ForegroundColor Green
        Write-Host "Waiting for device reboot..." -ForegroundColor Yellow
        
        $rebooted = $false
        for ($i = 0; $i -lt 30; $i++) {
            Start-Sleep -Milliseconds 200
            if (-not (Test-Path $targetDrive)) {
                $rebooted = $true
                break
            }
        }
        
        if ($rebooted) {
            Write-Host "====================================================" -ForegroundColor Green
            Write-Host "SUCCESS! Flashed and rebooted into new firmware!" -ForegroundColor Green
            Write-Host "====================================================" -ForegroundColor Green
        } else {
            Write-Host "Notice: drive still present. Please check device." -ForegroundColor Yellow
        }
        $found = $true
        break
    }
    Start-Sleep -Milliseconds 250
}

if (-not $found) {
    Write-Host "Timeout (180s). Bootloader drive not detected." -ForegroundColor Red
}
