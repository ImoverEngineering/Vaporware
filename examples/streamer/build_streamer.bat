@echo off
:: build_streamer.bat - SWD frame streamer for N32G031 + GC9107 128x160 LCD
:: Initialization mirrors flappy.c exactly (8 MHz, no PLL boost).
cd /d "%~dp0"

set GCC="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-gcc.exe"
set OBJCOPY="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-objcopy.exe"
set SIZE="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-size.exe"

set VAPORWARE=%~dp0..\..\src

set CPU=-mcpu=cortex-m0 -mthumb
set INC=-I%VAPORWARE%\include
set CFLAGS=%CPU% %INC% -Os -ffunction-sections -fdata-sections -Wall -std=c11

if not exist build mkdir build

echo [1/5] startup.s
%GCC% %CPU% -x assembler-with-cpp -c src\startup.s -o build\startup.o || goto :error

echo [2/5] system.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\system.c -o build\system.o || goto :error

echo [3/5] display.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\display.c -o build\display.o || goto :error

echo [4/5] battery.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\battery.c -o build\battery.o || goto :error

echo [5/5] streamer.c + link
%GCC% %CFLAGS% -c src\streamer.c -o build\streamer.o || goto :error

%GCC% %CPU% -Tn32g031.ld -Wl,--gc-sections -Wl,-Map=build\streamer.map -nostdlib -lnosys ^
  build\startup.o build\system.o build\display.o build\battery.o build\streamer.o ^
  -o build\streamer.elf || goto :error

%OBJCOPY% -O binary build\streamer.elf build\streamer.bin || goto :error
%OBJCOPY% -O ihex   build\streamer.elf build\streamer.hex || goto :error
%SIZE% build\streamer.elf

echo.
python gen_direct_flash.py || goto :error
echo.
echo Build SUCCESS: build\streamer.bin
echo Next: flash_vape.bat
goto :eof

:error
echo BUILD FAILED
exit /b 1
