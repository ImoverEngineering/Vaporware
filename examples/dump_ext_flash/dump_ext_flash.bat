@echo off
cd /d "%~dp0"

echo.
echo  ========================================
echo   Vape — Dump External Flash (GT25Q80A)
echo  ========================================
echo.

if not exist "build\dump_ext_flash.bin" (
    echo  ERROR: build\dump_ext_flash.bin not found.
    echo  Run build_dump.bat first.
    pause
    exit /b 1
)

echo  Running dump_ext_flash.py ...
echo  (ST-Link must be plugged in and vape must be on)
echo.

python dump_ext_flash.py
if errorlevel 1 (
    echo.
    echo  DUMP FAILED.
    pause
    exit /b 1
)

echo.
echo  ========================================
echo   Original flash saved to:
echo   build\original_ext_flash.bin
echo  ========================================
echo.
pause
