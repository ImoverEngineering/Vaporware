@echo off
:: flash_vape.bat - flash streamer firmware to a vape via ST-Link
:: Run build_streamer.bat first, then python gen_direct_flash.py, then this.

cd /d "%~dp0"

echo.
echo  ========================================
echo   Vape Flasher - Streamer
echo  ========================================
echo.

if not exist "build\streamer.bin" (
    echo  ERROR: build\streamer.bin not found. Run build_streamer.bat first.
    pause
    exit /b 1
)

for %%A in ("build\streamer.bin") do echo  Firmware: %%~zA bytes  [build\streamer.bin]
echo.

echo  [1/3] Waking WSL...
wsl echo ready >nul 2>&1
if errorlevel 1 (
    echo  ERROR: WSL not available.
    pause
    exit /b 1
)

echo  [2/3] Attaching ST-Link to WSL...
usbipd attach --wsl --busid 1-2 >nul 2>&1
ping -n 3 127.0.0.1 >nul

echo  [3/3] Flashing firmware...
echo.
wsl openocd -f n32g031.openocd.cfg -c "tcl_port disabled; telnet_port disabled; gdb_port disabled" -c "init" -c "source direct_flash.tcl" -c "exit" 2>&1

if errorlevel 1 (
    echo.
    echo  FLASH FAILED - check ST-Link is plugged in, vape is on, no other OpenOCD running
    pause
    exit /b 1
)

echo.
echo  ========================================
echo   Done! Keep ST-Link plugged in for streaming.
echo  ========================================
echo.
pause
