@echo off
cd /d "%~dp0"
echo.
echo  ==============================
echo   Flash optimized firmware
echo  ==============================
echo.

wsl echo ready >nul 2>&1
if errorlevel 1 ( echo ERROR: WSL not available & pause & exit /b 1 )

usbipd attach --wsl --busid 1-2 >nul 2>&1
ping -n 3 127.0.0.1 >nul

echo [1/2] Flashing frame-counter firmware (optimized bit-bang)...
wsl openocd -f n32g031.openocd.cfg ^
  -c "tcl_port disabled; telnet_port disabled; gdb_port disabled" ^
  -c "init" ^
  -c "source {direct_flash.tcl}" ^
  -c "exit" 2>&1

if errorlevel 1 (
  echo.
  echo  FLASH FAILED - power-cycle vape and try again
  pause & exit /b 1
)

echo.
echo [2/2] Measuring FPS (runs for 5 seconds)...
ping -n 4 127.0.0.1 >nul

wsl openocd -f n32g031.openocd.cfg ^
  -c "tcl_port disabled; telnet_port disabled; gdb_port disabled" ^
  -c "init" ^
  -c "source {measure_fps.tcl}" ^
  -c "exit" 2>&1

echo.
echo Done.
pause
