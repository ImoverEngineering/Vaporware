@echo off
:: flash_delay_hit.bat - build, generate, and flash delay_hit firmware via ST-Link

cd /d "%~dp0"

echo.
echo  ========================================
echo   Vape Flasher - Delay Hit (60s + bomb)
echo  ========================================
echo.

echo  [1/5] Building firmware...
call build_delay_hit.bat >nul 2>&1
if errorlevel 1 (
    echo  ERROR: build failed. Run build_delay_hit.bat directly to see the error.
    pause
    exit /b 1
)

if not exist "build\delay_hit.bin" (
    echo  ERROR: build\delay_hit.bin not found after build.
    pause
    exit /b 1
)
for %%A in ("build\delay_hit.bin") do echo        Firmware: %%~zA bytes  [build\delay_hit.bin]

echo  [2/5] Generating direct_flash.tcl...
python gen_direct_flash.py >nul 2>&1
if errorlevel 1 (
    echo  ERROR: gen_direct_flash.py failed.
    pause
    exit /b 1
)

echo  [3/5] Waking WSL...
wsl echo ready >nul 2>&1
if errorlevel 1 (
    echo  ERROR: WSL not available.
    pause
    exit /b 1
)

echo  [4/5] Attaching ST-Link to WSL...
usbipd attach --wsl --busid 1-2 >nul 2>&1
ping -n 3 127.0.0.1 >nul

echo  [5/5] Flashing firmware...
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
echo   Done! Press the button to arm.
echo  ========================================
echo.
pause
