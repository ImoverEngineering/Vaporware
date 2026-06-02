@echo off
cd /d "%~dp0"
:: build_flash_video.bat - compile flash_video + ext_flash_writer firmwares
::
:: Produces two binaries:
::   build\flash_video.bin       - playback firmware (reads ext flash, drives display)
::   build\ext_flash_writer.bin  - utility firmware  (writes video blob to ext flash)
::
:: Workflow:
::   1. build_flash_video.bat
::   2. python tools\prep_frames.py <image_or_video> [--fps 6]
::   3. python gen_flash.py
::   4. flash_vape.bat

set GCC="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-gcc.exe"
set OBJCOPY="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-objcopy.exe"
set SIZE="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-size.exe"

set VAPORWARE=%~dp0..\..\src

set CPU=-mcpu=cortex-m0 -mthumb
set INC=-I%VAPORWARE%\include
set CFLAGS=%CPU% %INC% -Os -ffunction-sections -fdata-sections -Wall -std=c11

if not exist build mkdir build

echo [1/5] startup.s
%GCC% %CPU% -x assembler-with-cpp -c %VAPORWARE%\src\startup.s -o build\startup.o || goto :error

echo [2/5] system.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\system.c  -o build\system.o  || goto :error

echo [3/5] display.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\display.c -o build\display.o || goto :error

echo [4/5] vape.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\vape.c    -o build\vape.o    || goto :error

echo [5/5] flash_video.c
%GCC% %CFLAGS% -c src\flash_video.c -o build\flash_video.o || goto :error

echo Linking flash_video...
%GCC% %CPU% -T%VAPORWARE%\n32g031.ld -Wl,--gc-sections -nostdlib -lnosys ^
  build\startup.o build\system.o build\display.o build\vape.o ^
  build\flash_video.o ^
  -o build\flash_video.elf || goto :error

%OBJCOPY% -O binary build\flash_video.elf build\flash_video.bin || goto :error
%OBJCOPY% -O ihex   build\flash_video.elf build\flash_video.hex || goto :error
%SIZE% build\flash_video.elf

for %%A in ("build\flash_video.bin") do set FW_BYTES=%%~zA
if %FW_BYTES% GTR 8192 (
    echo ERROR: flash_video.bin is %FW_BYTES% B -- exceeds 8192 B. Optimise flash_video.c.
    goto :error
)
echo flash_video firmware: %FW_BYTES% bytes

echo [+1] ext_flash_writer.c
%GCC% %CFLAGS% -c src\ext_flash_writer.c -o build\ext_flash_writer.o || goto :error

echo Linking ext_flash_writer...
%GCC% %CPU% -T%VAPORWARE%\n32g031.ld -Wl,--gc-sections -nostdlib -lnosys ^
  build\startup.o build\system.o ^
  build\ext_flash_writer.o ^
  -o build\ext_flash_writer.elf || goto :error

%OBJCOPY% -O binary build\ext_flash_writer.elf build\ext_flash_writer.bin || goto :error
%SIZE% build\ext_flash_writer.elf

echo.
echo Build SUCCESS.
echo   flash_video.bin      - playback firmware
echo   ext_flash_writer.bin - external flash writer utility
echo.
echo Next: python tools\prep_frames.py ^<image_or_video^> [--fps 6]
echo       python gen_flash.py
echo       flash_vape.bat
goto :eof

:error
echo BUILD FAILED
exit /b 1
