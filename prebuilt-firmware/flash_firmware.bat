@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo   ESP32-S3 Wi-Fi Internet Radio Firmware Flasher
echo ========================================================
echo.

set /p COMPORT="Enter COM port of your ESP32-S3 (e.g. COM3): "
if "%COMPORT%"=="" (
    echo Error: COM port cannot be empty!
    pause
    exit /b 1
)

echo.
echo Flashing bootloader, partitions, and firmware to %COMPORT%...
echo.

python -m esptool --chip esp32s3 -p %COMPORT% -b 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo   Flashing SUCCESSFUL!
    echo ========================================================
) else (
    echo.
    echo ========================================================
    echo   Flashing FAILED! Check connection, COM port, or drivers.
    echo ========================================================
)

echo.
pause
