@echo off
cd /d "%~dp0"
:: flash_vape.bat — combined flash: ext_flash_writer → write video → flash_video player
::
:: Prerequisites:
::   1. build_flash_video.bat            (builds both firmwares)
::   2. python tools\prep_frames.py ...  (produces build\ext_flash_video.bin)
::   3. python gen_flash.py              (produces build\combined_flash.tcl)

echo.
echo  ========================================
echo   Vape Flasher - Flash Video (Ext Flash)
echo  ========================================
echo.

for %%F in (build\ext_flash_writer.bin build\ext_flash_video.bin build\flash_video.bin build\combined_flash.tcl) do (
    if not exist "%%F" (
        echo  ERROR: %%F not found.
        echo  Run build_flash_video.bat, prep_frames.py, and gen_flash.py first.
        pause
        exit /b 1
    )
)

for %%A in ("build\flash_video.bin")     do echo  Player  firmware : %%~zA bytes
for %%A in ("build\ext_flash_video.bin") do echo  Video blob      : %%~zA bytes
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

echo  [3/3] Running combined flash (writer + video write + player)...
echo         This may take several minutes for large video blobs.
echo.

wsl openocd -f n32g031.openocd.cfg ^
  -c "tcl_port disabled; telnet_port disabled; gdb_port disabled" ^
  -c "init" ^
  -c "source {build/combined_flash.tcl}" ^
  -c "exit" 2>&1

if errorlevel 1 (
    echo.
    echo  FLASH FAILED - check ST-Link, vape power, no other OpenOCD running
    pause
    exit /b 1
)

echo.
echo  ========================================
echo   Done! Unplug ST-Link and enjoy.
echo  ========================================
echo.
pause
